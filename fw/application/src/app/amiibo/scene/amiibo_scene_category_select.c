/*
 * amiibo_scene_category_select.c   (v8.2 新增)
 *
 * 当前账号下的"分类"目录列表 (在 account 与 badge 之间新插入的一层).
 *
 *   /star/<channel>/<account>/<category>/<badge>.bin
 *
 *   - "默认分类" (磁盘目录名 = "_default") 永远存在, 永远在最上面,
 *     不可改名, 不可删除. 第一次进账号时自动创建.
 *   - 其它分类由用户在 "新建分类" 行进入文本输入添加, 显示名 = 磁盘目录名.
 *   - 短按某分类 -> 进入徽章列表 (该账号 / 该分类下).
 *   - 长按非默认分类 -> 进入 action_menu (重命名 / 删除 / 取消).
 *   - 长按默认分类 -> toast 提示 "默认分类不可改名/删除".
 *   - 短按 / 长按 "新建分类" -> 进入 category_input 新建.
 *   - 返回 -> 回到账号选择.
 *
 *  ─ 光遇徽章定制版 v8.2 — category_select ─
 */
#include "amiibo_scene.h"
#include "app_amiibo.h"
#include "mui_list_view.h"
#include "nrf_log.h"
#include "vfs.h"
#include "i18n/language.h"
#include "port/star_channels.h"

#include <stdio.h>
#include <string.h>

#define ICON_DEFAULT_CAT  0xe1f0
#define ICON_CAT          0xe1d6
#define ICON_NEW          0xe1ed
#define ICON_BACK         0xe069

#define CATEGORY_DEFAULT_DIR  "_default"

#define CATEGORY_ITEM_NEW    0xFFFFFFFEu
#define CATEGORY_ITEM_BACK   0xFFFFFFFFu
/* v9.0-fix1 */
#define CATEGORY_ITEM_MAIN   0xFFFFFFFDu
#define ICON_HOME            0xe1f0

/* "0" = 默认分类
 * "1..N" = 用户分类 (text 即目录名) */

static bool format_account_path(app_amiibo_t *app, char *out, size_t cap) {
    const star_channel_t *ch = star_channels_get(app->star_channel_idx);
    if (ch == NULL) return false;
    int n = snprintf(out, cap, "%s/%s/%s",
                     STAR_ROOT_FOLDER, ch->dir_name,
                     string_get_cstr(app->star_account_dir));
    return n > 0 && (size_t)n < cap;
}

/* 确保 "_default" 分类目录存在 (幂等). 失败不致命, 列表里它还是会显示
 * (短按时 badge_list 会再次 create_dir). */
static void ensure_default_category(app_amiibo_t *app) {
    char acc_path[VFS_MAX_PATH_LEN];
    char def_path[VFS_MAX_PATH_LEN];
    if (!format_account_path(app, acc_path, sizeof(acc_path))) return;
    int n = snprintf(def_path, sizeof(def_path), "%s/%s",
                     acc_path, CATEGORY_DEFAULT_DIR);
    if (n < 0 || (size_t)n >= sizeof(def_path)) return;
    vfs_driver_t *p_drv = vfs_get_driver(app->current_drive);
    /* 账号目录可能也还没建 — 先确保 */
    p_drv->create_dir(acc_path);
    p_drv->create_dir(def_path);
}

static void amiibo_scene_category_select_reload(app_amiibo_t *app) {
    vfs_driver_t *p_drv = vfs_get_driver(app->current_drive);
    vfs_dir_t dir;
    vfs_obj_t obj;
    char acc_path[VFS_MAX_PATH_LEN];

    mui_list_view_clear_items(app->p_list_view);

    if (!format_account_path(app, acc_path, sizeof(acc_path))) return;

    /* 1. 默认分类 */
    mui_list_view_add_item(app->p_list_view, ICON_DEFAULT_CAT,
                           getLangString(_L_STAR_DEFAULT_CATEGORY),
                           (void *)(uintptr_t)0);

    /* 2. 扫描账号目录, 列出除 "_default" 外的子目录. */
    uint32_t cat_idx = 1;
    int32_t res = p_drv->open_dir(acc_path, &dir);
    if (res == VFS_OK) {
        while (p_drv->read_dir(&dir, &obj) == VFS_OK) {
            if (obj.type == VFS_TYPE_DIR &&
                strcmp(obj.name, CATEGORY_DEFAULT_DIR) != 0) {
                mui_list_view_add_item(app->p_list_view, ICON_CAT, obj.name,
                                       (void *)(uintptr_t)cat_idx);
                cat_idx++;
            }
        }
        p_drv->close_dir(&dir);
    }

    /* 3. 新建分类 */
    mui_list_view_add_item(app->p_list_view, ICON_NEW,
                           getLangString(_L_STAR_NEW_CATEGORY),
                           (void *)(uintptr_t)CATEGORY_ITEM_NEW);
    /* 4. 返回 */
    mui_list_view_add_item(app->p_list_view, ICON_BACK,
                           getLangString(_L_BACK),
                           (void *)(uintptr_t)CATEGORY_ITEM_BACK);
    /* v9.0-fix1: 5. 返回主菜单 */
    mui_list_view_add_item(app->p_list_view, ICON_HOME,
                           getLangString(_L_BACK_TO_MAIN_MENU),
                           (void *)(uintptr_t)CATEGORY_ITEM_MAIN);
}

static void amiibo_scene_category_select_on_selected(mui_list_view_event_t event,
                                                      mui_list_view_t *p_list_view,
                                                      mui_list_item_t *p_item) {
    app_amiibo_t *app = p_list_view->user_data;
    uintptr_t tag = (uintptr_t)p_item->user_data;

    if (tag == CATEGORY_ITEM_BACK) {
        if (event == MUI_LIST_VIEW_EVENT_SELECTED) {
            mui_scene_dispatcher_previous_scene(app->p_scene_dispatcher);
        }
        return;
    }

    if (tag == CATEGORY_ITEM_MAIN) {
        /* v9.0-fix1: 一键回主菜单. 栈: [channel, account, category_select],
         * pop 2 → channel_select. */
        if (event == MUI_LIST_VIEW_EVENT_SELECTED ||
            event == MUI_LIST_VIEW_EVENT_LONG_SELECTED) {
            mui_scene_dispatcher_back_scene(app->p_scene_dispatcher, 2);
        }
        return;
    }

    if (tag == CATEGORY_ITEM_NEW) {
        if (event == MUI_LIST_VIEW_EVENT_SELECTED ||
            event == MUI_LIST_VIEW_EVENT_LONG_SELECTED) {
            app->star_rename_mode = false;       /* 新建, 不是 rename */
            mui_scene_dispatcher_next_scene(app->p_scene_dispatcher,
                                            AMIIBO_SCENE_CATEGORY_INPUT);
        }
        return;
    }

    /* 默认分类 (tag==0) 或用户分类 */
    if (event == MUI_LIST_VIEW_EVENT_SELECTED) {
        /* 短按 — 进入徽章列表 */
        if (tag == 0) {
            string_set_str(app->star_category_dir, CATEGORY_DEFAULT_DIR);
        } else {
            string_set(app->star_category_dir, p_item->text);
        }
        mui_scene_dispatcher_next_scene(app->p_scene_dispatcher,
                                        AMIIBO_SCENE_BADGE_LIST);
    } else if (event == MUI_LIST_VIEW_EVENT_LONG_SELECTED) {
        /* 长按 — 默认分类拦截; 用户分类弹 action_menu */
        if (tag == 0) {
            mui_toast_view_show(app->p_toast_view,
                                getLangString(_L_STAR_CATEGORY_DEFAULT_NOT_EDITABLE));
            return;
        }
        /* 把目标分类名暂存到 star_category_dir, 给 action_menu / input 用 */
        string_set(app->star_category_dir, p_item->text);
        app->star_rename_target_kind = STAR_RENAME_KIND_CATEGORY;
        mui_scene_dispatcher_next_scene(app->p_scene_dispatcher,
                                        AMIIBO_SCENE_ACTION_MENU);
    }
}

void amiibo_scene_category_select_on_enter(void *user_data) {
    app_amiibo_t *app = user_data;
    ensure_default_category(app);
    amiibo_scene_category_select_reload(app);
    mui_list_view_set_selected_cb(app->p_list_view, amiibo_scene_category_select_on_selected);
    mui_list_view_set_user_data(app->p_list_view, app);
    mui_view_dispatcher_switch_to_view(app->p_view_dispatcher, AMIIBO_VIEW_ID_LIST);
}

void amiibo_scene_category_select_on_exit(void *user_data) {
    app_amiibo_t *app = user_data;
    mui_list_view_set_selected_cb(app->p_list_view, NULL);
    mui_list_view_clear_items(app->p_list_view);
}
