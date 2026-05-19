/*
 * amiibo_scene_channel_select.c
 *
 * v4 Star 模拟器入口: 显示固定的 11 个渠道列表.
 *
 *   - 不可增删, 不可编辑.
 *   - 短按 -> 进入该渠道账号列表.
 *   - 长按 -> 无动作 (渠道是固定的).
 *   - 返回 -> 退出 Star 模拟器.
 *
 * 进入某渠道前会自动确保 /star/<channel_dir>/ 与
 *   /star/<channel_dir>/default/  这两个目录存在.
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

#define ICON_CHANNEL 0xe1d6   /* 用 folder 图标, 与文件浏览器风格一致 */
#define ICON_BACK    0xe069
#define ICON_HOME    0xe1f0

/* 确保渠道目录与默认账号目录存在 (幂等). */
static void star_ensure_channel_dirs(app_amiibo_t *app, const char *channel_dir) {
    vfs_driver_t *p_drv = vfs_get_driver(app->current_drive);
    char path[VFS_MAX_PATH_LEN];

    /* /star */
    p_drv->create_dir(STAR_ROOT_FOLDER);
    /* v8: 不再创建 /star/_inbox 目录 — v7 的 inbox 流程已废弃,
     * "新建徽章" 直接在账号目录里写占位 + 等 BLE 写入. */
    /* /star/<channel> */
    snprintf(path, sizeof(path), "%s/%s", STAR_ROOT_FOLDER, channel_dir);
    p_drv->create_dir(path);
    /* /star/<channel>/default */
    snprintf(path, sizeof(path), "%s/%s/%s",
             STAR_ROOT_FOLDER, channel_dir, STAR_DEFAULT_ACCOUNT_DIR);
    p_drv->create_dir(path);
}

static void amiibo_scene_channel_select_on_selected(mui_list_view_event_t event,
                                                     mui_list_view_t *p_list_view,
                                                     mui_list_item_t *p_item) {
    app_amiibo_t *app = p_list_view->user_data;
    uint32_t idx = (uint32_t)p_item->user_data;

    if (event != MUI_LIST_VIEW_EVENT_SELECTED) {
        /* 长按: 渠道是固定的, 啥也不做 */
        return;
    }

    if (idx >= STAR_CH_COUNT) {
        /* "返回" 项 */
        mini_app_launcher_kill(mini_app_launcher(), MINI_APP_ID_AMIIBO);
        return;
    }

    const star_channel_t *ch = star_channels_get(idx);
    if (ch == NULL) return;

    star_ensure_channel_dirs(app, ch->dir_name);

    app->star_channel_idx = (int32_t)idx;
    /* 进入账号列表; 由 account_select scene 自己根据 star_channel_idx 渲染 */
    mui_scene_dispatcher_next_scene(app->p_scene_dispatcher, AMIIBO_SCENE_ACCOUNT_SELECT);
}

void amiibo_scene_channel_select_on_enter(void *user_data) {
    app_amiibo_t *app = user_data;

    mui_list_view_clear_items(app->p_list_view);

    for (uint32_t i = 0; i < STAR_CH_COUNT; i++) {
        const star_channel_t *ch = star_channels_get(i);
        mui_list_view_add_item(app->p_list_view, ICON_CHANNEL,
                               getLangString(ch->name_i18n_key),
                               (void *)(uintptr_t)i);
    }
    /* 最后一项: 返回 */
    mui_list_view_add_item(app->p_list_view, ICON_BACK,
                           getLangString(_L_BACK),
                           (void *)(uintptr_t)STAR_CH_COUNT);

    mui_list_view_set_selected_cb(app->p_list_view, amiibo_scene_channel_select_on_selected);
    mui_list_view_set_user_data(app->p_list_view, app);
    mui_view_dispatcher_switch_to_view(app->p_view_dispatcher, AMIIBO_VIEW_ID_LIST);
}

void amiibo_scene_channel_select_on_exit(void *user_data) {
    app_amiibo_t *app = user_data;
    mui_list_view_set_selected_cb(app->p_list_view, NULL);
    mui_list_view_clear_items(app->p_list_view);
}
