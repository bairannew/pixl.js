/*
 * amiibo_scene_account_select.c
 *
 * 列出当前渠道下的所有账号 (含固定的"默认账号").
 *
 *   - 默认账号 (磁盘目录名 = "default") 永远存在, 永远在最上面,
 *     不可改名, 不可删除.
 *   - 其它账号由用户在 "新建账号" 行进入文本输入添加, 显示名 = 磁盘目录名.
 *   - 短按账号 -> 进入徽章列表.
 *   - 长按账号 -> v8.1: 删除该账号 (含其下所有 .bin), 弹 msg_box 确认.
 *                       默认账号长按仍然只 toast 提示不可改.
 *   - 短按 / 长按 "新建账号" -> 进入文本输入新建账号.
 *   - 返回 -> 回到渠道选择.
 *
 * v8.1 修复: 之前长按非默认账号也只 toast "默认账号不可改名" (复制粘贴 bug),
 *            用户反馈 "除了默认账号都可以删除才对, 你现在所有账号什么都没法
 *            删除啊" — 改成弹确认 + 递归删除.
 */
#include "amiibo_scene.h"
#include "app_amiibo.h"
#include "mini_app_launcher.h"
#include "mini_app_registry.h"
#include "mui_list_view.h"
#include "nrf_log.h"
#include "vfs.h"
#include "i18n/language.h"
#include "port/star_channels.h"

#include <stdio.h>
#include <string.h>

#define ICON_DEFAULT_USER  0xe1f0   /* home/star: 给默认账号 */
#define ICON_USER          0xe1d6   /* folder: 普通账号 */
#define ICON_NEW           0xe1ed   /* plus/file icon: 新建账号 */
#define ICON_BACK          0xe069
#define ICON_HOME          0xe1f0   /* v9.0-fix1: 返回主菜单 */

/* user_data 编码:
 *   0..N-1  : 账号在列表里的序号
 *   特殊值  : */
#define ACCOUNT_ITEM_NEW    0xFFFFFFFEu
#define ACCOUNT_ITEM_BACK   0xFFFFFFFFu
/* v9.0-fix1 */
#define ACCOUNT_ITEM_MAIN   0xFFFFFFFDu

static void format_channel_path(const star_channel_t *ch, char *path_out, size_t path_max) {
    snprintf(path_out, path_max, "%s/%s", STAR_ROOT_FOLDER, ch->dir_name);
}

static void amiibo_scene_account_select_reload(app_amiibo_t *app) {
    vfs_driver_t *p_drv = vfs_get_driver(app->current_drive);
    vfs_dir_t dir;
    vfs_obj_t obj;
    char channel_path[VFS_MAX_PATH_LEN];

    mui_list_view_clear_items(app->p_list_view);

    const star_channel_t *ch = star_channels_get(app->star_channel_idx);
    if (ch == NULL) {
        return;
    }

    format_channel_path(ch, channel_path, sizeof(channel_path));

    /* 1. 默认账号永远在第一位.  user_data 不指向真实目录索引,
     *    而是用文本字段作为 dir_name 来源. 我们额外存到 sub_text 里
     *    防止误删. 简化: 直接拿 text 当目录名, 默认账号 text=显示名
     *    "默认账号" 但实际目录是 "default" — 这会有冲突.
     *
     *    解决方案: 把 user_data 编成 "0 = default account",
     *    "1..N = 用户账号 (text 即目录名)", N+1 = 新建, N+2 = 返回. */
    mui_list_view_add_item(app->p_list_view, ICON_DEFAULT_USER,
                           getLangString(_L_STAR_DEFAULT_ACCOUNT),
                           (void *)(uintptr_t)0);

    /* 2. 扫描渠道目录, 列出除 "default" 外的子目录. */
    uint32_t account_idx = 1;
    int32_t res = p_drv->open_dir(channel_path, &dir);
    if (res == VFS_OK) {
        while (p_drv->read_dir(&dir, &obj) == VFS_OK) {
            if (obj.type == VFS_TYPE_DIR &&
                strcmp(obj.name, STAR_DEFAULT_ACCOUNT_DIR) != 0) {
                mui_list_view_add_item(app->p_list_view, ICON_USER, obj.name,
                                       (void *)(uintptr_t)account_idx);
                account_idx++;
            }
        }
        p_drv->close_dir(&dir);
    }

    /* 3. 新建账号 */
    mui_list_view_add_item(app->p_list_view, ICON_NEW,
                           getLangString(_L_STAR_NEW_ACCOUNT),
                           (void *)(uintptr_t)ACCOUNT_ITEM_NEW);

    /* 4. 返回 */
    mui_list_view_add_item(app->p_list_view, ICON_BACK,
                           getLangString(_L_BACK),
                           (void *)(uintptr_t)ACCOUNT_ITEM_BACK);

    /* v9.0-fix1: 5. 返回主菜单. account_select 本身离主菜单只差一层,
     * 加这条让用户不用区分"返回" vs "主菜单"的语义 — 哪一项更顺手就点哪个. */
    mui_list_view_add_item(app->p_list_view, ICON_HOME,
                           getLangString(_L_BACK_TO_MAIN_MENU),
                           (void *)(uintptr_t)ACCOUNT_ITEM_MAIN);
}

/* ============================================================ */
/*  v8.2: 长按删除的逻辑从这里搬到了 amiibo_scene_action_menu.c   */
/*                                                              */
/*  原 v8.1 在长按账号时直接弹删除确认 msg_box; v8.2 改成弹一个   */
/*  action_menu 让用户选 "重命名" / "删除" / "取消", 真正的递归    */
/*  删除与确认 msg_box 都搬到 action_menu.c 了, 这里只保留导航.   */
/*                                                              */
/*  保留的旧 helper 已被删除以避免 -Wunused-function 警告.         */
/* ============================================================ */

static void amiibo_scene_account_select_on_selected(mui_list_view_event_t event,
                                                     mui_list_view_t *p_list_view,
                                                     mui_list_item_t *p_item) {
    app_amiibo_t *app = p_list_view->user_data;
    uintptr_t tag = (uintptr_t)p_item->user_data;

    if (tag == ACCOUNT_ITEM_BACK) {
        if (event == MUI_LIST_VIEW_EVENT_SELECTED) {
            mui_scene_dispatcher_previous_scene(app->p_scene_dispatcher);
        }
        return;
    }

    if (tag == ACCOUNT_ITEM_MAIN) {
        /* v9.0-fix1: 一键回主菜单. 栈: [channel, account_select], pop 1.
         * 这里跟 ACCOUNT_ITEM_BACK 等价 (因为 account_select 紧贴 channel_select),
         * 但保留这个入口是为了 UI 一致 — 跟 category/badge_list 上的那一项
         * 出现在同样位置, 不让用户在跨层级时找不到"主菜单". */
        if (event == MUI_LIST_VIEW_EVENT_SELECTED ||
            event == MUI_LIST_VIEW_EVENT_LONG_SELECTED) {
            mui_scene_dispatcher_back_scene(app->p_scene_dispatcher, 1);
        }
        return;
    }

    if (tag == ACCOUNT_ITEM_NEW) {
        if (event == MUI_LIST_VIEW_EVENT_SELECTED ||
            event == MUI_LIST_VIEW_EVENT_LONG_SELECTED) {
            /* 新建模式 — 不开 rename */
            app->star_rename_mode = false;
            mui_scene_dispatcher_next_scene(app->p_scene_dispatcher,
                                            AMIIBO_SCENE_ACCOUNT_INPUT);
        }
        return;
    }

    /* 普通账号或默认账号 */
    if (event == MUI_LIST_VIEW_EVENT_SELECTED) {
        /* 短按 = 进入分类列表 (v8.2 改: 不再直接进徽章列表) */
        if (tag == 0) {
            string_set_str(app->star_account_dir, STAR_DEFAULT_ACCOUNT_DIR);
        } else {
            /* 普通账号: p_item->text 就是目录名 */
            string_set(app->star_account_dir, p_item->text);
        }
        mui_scene_dispatcher_next_scene(app->p_scene_dispatcher,
                                        AMIIBO_SCENE_CATEGORY_SELECT);
    } else if (event == MUI_LIST_VIEW_EVENT_LONG_SELECTED) {
        /* v8.2: 长按 = 弹 action_menu (重命名 / 删除 / 取消). 默认账号仍然禁止. */
        if (tag == 0) {
            mui_toast_view_show(app->p_toast_view,
                                getLangString(_L_STAR_DEFAULT_NOT_EDITABLE));
            return;
        }
        /* 把目录名暂存到 star_account_dir, 给 action_menu 用 */
        string_set(app->star_account_dir, p_item->text);
        app->star_rename_target_kind = STAR_RENAME_KIND_ACCOUNT;
        mui_scene_dispatcher_next_scene(app->p_scene_dispatcher,
                                        AMIIBO_SCENE_ACTION_MENU);
    }
}

void amiibo_scene_account_select_on_enter(void *user_data) {
    app_amiibo_t *app = user_data;
    amiibo_scene_account_select_reload(app);
    mui_list_view_set_selected_cb(app->p_list_view, amiibo_scene_account_select_on_selected);
    mui_list_view_set_user_data(app->p_list_view, app);
    mui_view_dispatcher_switch_to_view(app->p_view_dispatcher, AMIIBO_VIEW_ID_LIST);
}

void amiibo_scene_account_select_on_exit(void *user_data) {
    app_amiibo_t *app = user_data;
    mui_list_view_set_selected_cb(app->p_list_view, NULL);
    mui_list_view_clear_items(app->p_list_view);
}
