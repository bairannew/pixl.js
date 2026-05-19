/*
 * amiibo_scene_action_menu.c   (v8.2 新增, v8.2-fix5 扩展支持 BADGE)
 *
 * 长按账号 / 分类 / 徽章时弹出的 "重命名 / 删除 / 取消" 操作菜单.
 *
 * 用户体验:
 *   account_select 长按非默认账号  -> 进入这个场景 -> 选 "重命名" / "删除"
 *   category_select 长按非默认分类 -> 进入这个场景 -> 选 "重命名" / "删除"
 *   badge_list 长按某个徽章 (v8.2-fix5 新增)
 *                                  -> 进入这个场景 -> 选 "重命名" / "删除"
 *
 * 数据流:
 *   调用方在 push 这个场景前先填好 app->star_rename_target_kind:
 *     STAR_RENAME_KIND_ACCOUNT  : 目标 = 当前 star_account_dir   (目录)
 *     STAR_RENAME_KIND_CATEGORY : 目标 = 当前 star_category_dir  (目录)
 *     STAR_RENAME_KIND_BADGE    : 目标 = 当前 star_badge_pending (单文件)
 *
 *   选 "重命名":
 *     - kind=ACCOUNT  -> star_rename_mode=true, next_scene(ACCOUNT_INPUT)
 *     - kind=CATEGORY -> star_rename_mode=true, next_scene(CATEGORY_INPUT)
 *     - kind=BADGE    -> next_scene(BADGE_NAME_INPUT) — BADGE_NAME_INPUT
 *                        负责 back_scene(2) 一次性回到 badge_list, 用户
 *                        不会再看到 action_menu 那一层.
 *
 *   选 "删除":
 *     - kind=ACCOUNT/CATEGORY -> 递归删整个目录 (含子目录)
 *     - kind=BADGE            -> 直接 remove_file 单文件 (不能走递归,
 *                                否则会把整张分类目录扫空, 灾难性).
 *     删完都 back_scene(2) 回到上层列表.
 *
 *   选 "取消" / 返回 -> previous_scene.
 *
 *  ─ 光遇徽章定制版 v8.2-fix5 — action_menu ─
 */
#include "amiibo_scene.h"
#include "app_amiibo.h"
#include "i18n/language.h"
#include "mui_list_view.h"
#include "mui_msg_box.h"
#include "mui_toast_view.h"
#include "nrf_log.h"
#include "vfs.h"
#include "port/star_channels.h"
#include "df_proto_vfs.h"   /* v9.0-fix2: 强制关 BLE 文件/目录句柄, 防删除被 LFS 卡住 */

#include <stdio.h>
#include <string.h>

#define ICON_EDIT    0xe1f4
#define ICON_DEL     0xe173
#define ICON_BACK    0xe069

#define ACTION_RENAME   0xA0u
#define ACTION_DELETE   0xA1u
#define ACTION_BACK     0xA2u


/* ============================================================ */
/*  递归删目录 (复用 account_select 的实现, 这里再写一份免循环依赖)*/
/* ============================================================ */
static int32_t action_remove_dir_recursive(vfs_driver_t *p_drv, const char *dir) {
    vfs_dir_t dh;
    vfs_obj_t obj;
    int32_t worst = VFS_OK;

    if (p_drv->open_dir(dir, &dh) != VFS_OK) {
        return p_drv->remove_dir(dir);
    }
    while (p_drv->read_dir(&dh, &obj) == VFS_OK) {
        char child[VFS_MAX_PATH_LEN];
        int n = snprintf(child, sizeof(child), "%s/%s", dir, obj.name);
        if (n < 0 || (size_t)n >= sizeof(child)) {
            worst = VFS_ERR_FAIL;
            continue;
        }
        int32_t r;
        if (obj.type == VFS_TYPE_DIR) {
            r = action_remove_dir_recursive(p_drv, child);
        } else {
            r = p_drv->remove_file(child);
        }
        if (r != VFS_OK) worst = r;
    }
    p_drv->close_dir(&dh);

    int32_t r = p_drv->remove_dir(dir);
    if (r != VFS_OK) return r;
    return worst;
}


/* ============================================================ */
/*  路径拼接                                                     */
/* ============================================================ */
static bool action_format_target_path(app_amiibo_t *app, char *out, size_t cap) {
    const star_channel_t *ch = star_channels_get(app->star_channel_idx);
    if (ch == NULL) return false;
    if (app->star_rename_target_kind == STAR_RENAME_KIND_ACCOUNT) {
        int n = snprintf(out, cap, "%s/%s/%s",
                         STAR_ROOT_FOLDER, ch->dir_name,
                         string_get_cstr(app->star_account_dir));
        return n > 0 && (size_t)n < cap;
    } else if (app->star_rename_target_kind == STAR_RENAME_KIND_CATEGORY) {
        int n = snprintf(out, cap, "%s/%s/%s/%s",
                         STAR_ROOT_FOLDER, ch->dir_name,
                         string_get_cstr(app->star_account_dir),
                         string_get_cstr(app->star_category_dir));
        return n > 0 && (size_t)n < cap;
    } else if (app->star_rename_target_kind == STAR_RENAME_KIND_BADGE) {
        /* v8.2-fix5: 徽章是单文件, 5 段路径
         * /star/<ch>/<account>/<category>/<badge_filename>.
         * star_badge_pending 已经在 badge_list 长按时填好了 (含 .bin). */
        const char *cat = string_get_cstr(app->star_category_dir);
        if (cat == NULL || cat[0] == '\0') cat = "_default";
        int n = snprintf(out, cap, "%s/%s/%s/%s/%s",
                         STAR_ROOT_FOLDER, ch->dir_name,
                         string_get_cstr(app->star_account_dir),
                         cat,
                         string_get_cstr(app->star_badge_pending));
        return n > 0 && (size_t)n < cap;
    }
    return false;
}


/* ============================================================ */
/*  删除确认 msg_box                                             */
/* ============================================================ */
static void action_menu_delete_confirm_cb(mui_msg_box_event_t event,
                                           mui_msg_box_t *p_msg_box) {
    app_amiibo_t *app = p_msg_box->user_data;
    if (event != MUI_MSG_BOX_EVENT_SELECT_LEFT) {
        /* 取消删除 -> 一并 pop action_menu, 回到上层列表 */
        mui_scene_dispatcher_previous_scene(app->p_scene_dispatcher);
        mui_scene_dispatcher_previous_scene(app->p_scene_dispatcher);
        return;
    }
    char path[VFS_MAX_PATH_LEN];
    if (!action_format_target_path(app, path, sizeof(path))) {
        mui_toast_view_show(app->p_toast_view, getLangString(_L_FAILED));
        mui_scene_dispatcher_previous_scene(app->p_scene_dispatcher);
        mui_scene_dispatcher_previous_scene(app->p_scene_dispatcher);
        return;
    }
    vfs_driver_t *p_drv = vfs_get_driver(app->current_drive);
    /* v9.0-fix2: 删除前先把 BLE 残留的 file / dir 句柄关干净. 跟 rename /
     * migrate 同根: 删除大目录走 action_remove_dir_recursive, 在递归扫描
     * 期间任意一个文件被 BLE 客户端打开就会让对应 remove_file 失败, 整个
     * 删除半途而废, 用户看到 "确认删除后只删了一半 / 完全没删" 的现象. */
    int32_t r = VFS_ERR_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        df_proto_vfs_close_active_upload();
        df_proto_vfs_close_active_dir();
        if (app->star_rename_target_kind == STAR_RENAME_KIND_BADGE) {
            /* v8.2-fix5: 徽章是单文件, 直接 remove_file. 不要走目录递归
             * 删除 (那会扫整个分类目录 -> 全空删干净, 灾难性). */
            r = p_drv->remove_file(path);
        } else {
            r = action_remove_dir_recursive(p_drv, path);
        }
        if (r == VFS_OK) break;
        NRF_LOG_WARNING("action_menu delete attempt %d failed (%d), retrying after force-close",
                        attempt, (int)r);
    }
    if (r != VFS_OK) {
        mui_toast_view_show(app->p_toast_view, getLangString(_L_FAILED));
    }
    /* 删完: 清掉 target 的状态字段, 避免上层用悬挂目录名/文件名 */
    if (app->star_rename_target_kind == STAR_RENAME_KIND_ACCOUNT) {
        string_reset(app->star_account_dir);
    } else if (app->star_rename_target_kind == STAR_RENAME_KIND_CATEGORY) {
        string_reset(app->star_category_dir);
    } else if (app->star_rename_target_kind == STAR_RENAME_KIND_BADGE) {
        string_reset(app->star_badge_pending);
    }
    /* 双 pop: action_menu -> 上层列表 (account_select / category_select /
     * badge_list). badge_list 的 on_enter 会重扫目录, 删掉的条目自动消失. */
    mui_scene_dispatcher_previous_scene(app->p_scene_dispatcher);
    mui_scene_dispatcher_previous_scene(app->p_scene_dispatcher);
}

static void action_menu_prompt_delete(app_amiibo_t *app) {
    static char msg[96];
    const char *target_name;
    L_StringID confirm_tmpl;
    if (app->star_rename_target_kind == STAR_RENAME_KIND_ACCOUNT) {
        target_name  = string_get_cstr(app->star_account_dir);
        confirm_tmpl = _L_STAR_ACCOUNT_DELETE_CONFIRM;
    } else if (app->star_rename_target_kind == STAR_RENAME_KIND_CATEGORY) {
        target_name  = string_get_cstr(app->star_category_dir);
        confirm_tmpl = _L_STAR_CATEGORY_DELETE_CONFIRM;
    } else {
        /* STAR_RENAME_KIND_BADGE: 单文件删除 */
        target_name  = string_get_cstr(app->star_badge_pending);
        confirm_tmpl = _L_STAR_BADGE_DELETE_CONFIRM;
    }
    snprintf(msg, sizeof(msg), getLangString(confirm_tmpl), target_name);

    mui_msg_box_set_header(app->p_msg_box, getLangString(_L_STAR_CONFIRM_DELETE));
    mui_msg_box_set_message(app->p_msg_box, msg);
    mui_msg_box_set_btn_text(app->p_msg_box,
                             getLangString(_L_CONFIRM), NULL,
                             getLangString(_L_CANCEL));
    mui_msg_box_set_btn_focus(app->p_msg_box, 2);  /* 默认焦点 Cancel, 防误删 */
    mui_msg_box_set_event_cb(app->p_msg_box, action_menu_delete_confirm_cb);
    mui_view_dispatcher_switch_to_view(app->p_view_dispatcher, AMIIBO_VIEW_ID_MSG_BOX);
}


/* ============================================================ */
/*  列表选择回调                                                 */
/* ============================================================ */
static void amiibo_scene_action_menu_on_selected(mui_list_view_event_t event,
                                                  mui_list_view_t *p_list_view,
                                                  mui_list_item_t *p_item) {
    if (event != MUI_LIST_VIEW_EVENT_SELECTED) return;  /* 忽略长按 */

    app_amiibo_t *app = p_list_view->user_data;
    uintptr_t tag = (uintptr_t)p_item->user_data;

    if (tag == ACTION_BACK) {
        mui_scene_dispatcher_previous_scene(app->p_scene_dispatcher);
        return;
    }
    if (tag == ACTION_RENAME) {
        /* 切换到对应 input 场景, 同时打开 rename_mode 让 input 知道
         * 应该预填当前名 + 调用 rename 而不是 mkdir. */
        app->star_rename_mode = true;
        if (app->star_rename_target_kind == STAR_RENAME_KIND_ACCOUNT) {
            mui_scene_dispatcher_next_scene(app->p_scene_dispatcher,
                                            AMIIBO_SCENE_ACCOUNT_INPUT);
        } else if (app->star_rename_target_kind == STAR_RENAME_KIND_CATEGORY) {
            mui_scene_dispatcher_next_scene(app->p_scene_dispatcher,
                                            AMIIBO_SCENE_CATEGORY_INPUT);
        } else {
            /* v8.2-fix5: STAR_RENAME_KIND_BADGE -> 走老的 BADGE_NAME_INPUT.
             * badge_list 长按那条路径已经预填好 star_badge_pending /
             * star_badge_autofill / star_wait_rename_mode 这几个字段,
             * 这里只是中转一下: action_menu 把"删除"也并进来, 然后选
             * "重命名"再 push 到 BADGE_NAME_INPUT. 体验和原来直接长按
             * 跳 rename 等价. */
            mui_scene_dispatcher_next_scene(app->p_scene_dispatcher,
                                            AMIIBO_SCENE_BADGE_NAME_INPUT);
        }
        return;
    }
    if (tag == ACTION_DELETE) {
        action_menu_prompt_delete(app);
        return;
    }
}

void amiibo_scene_action_menu_on_enter(void *user_data) {
    app_amiibo_t *app = user_data;
    mui_list_view_clear_items(app->p_list_view);

    /* 标题用对应的 "账号操作" / "分类操作" / "徽章操作". list_view 本身
     * 没显式 title; 这里先加一行带标题的伪条目, 用户在视觉上能区分自己
     * 在哪 (实际上这条目无 action, 用 NULL_USER_DATA). */
    L_StringID title_id;
    if (app->star_rename_target_kind == STAR_RENAME_KIND_ACCOUNT) {
        title_id = _L_STAR_ACCOUNT_ACTION_TITLE;
    } else if (app->star_rename_target_kind == STAR_RENAME_KIND_CATEGORY) {
        title_id = _L_STAR_CATEGORY_ACTION_TITLE;
    } else {
        title_id = _L_STAR_BADGE_ACTION_TITLE;   /* v8.2-fix5 */
    }
    mui_list_view_add_item(app->p_list_view, 0, getLangString(title_id),
                           NULL_USER_DATA);

    mui_list_view_add_item(app->p_list_view, ICON_EDIT,
                           getLangString(_L_STAR_ACTION_RENAME),
                           (void *)(uintptr_t)ACTION_RENAME);
    mui_list_view_add_item(app->p_list_view, ICON_DEL,
                           getLangString(_L_STAR_ACTION_DELETE),
                           (void *)(uintptr_t)ACTION_DELETE);
    mui_list_view_add_item(app->p_list_view, ICON_BACK,
                           getLangString(_L_BACK),
                           (void *)(uintptr_t)ACTION_BACK);

    mui_list_view_set_selected_cb(app->p_list_view, amiibo_scene_action_menu_on_selected);
    mui_list_view_set_user_data(app->p_list_view, app);
    /* 默认焦点定到 "重命名" (第 2 项), 不是标题占位 */
    mui_list_view_set_focus(app->p_list_view, 1);
    mui_view_dispatcher_switch_to_view(app->p_view_dispatcher, AMIIBO_VIEW_ID_LIST);
}

void amiibo_scene_action_menu_on_exit(void *user_data) {
    app_amiibo_t *app = user_data;
    mui_list_view_set_selected_cb(app->p_list_view, NULL);
    mui_list_view_clear_items(app->p_list_view);
}
