#include "app_amiibo.h"
#include "mini_app_registry.h"

#include "mui_include.h"

#include "amiibo_detail_view.h"

#include "amiibo_scene.h"

#include "mui_list_view.h"

#include "cache.h"
#include "m-string.h"
#include "settings.h"

#include "amiibo_helper.h"
#include "i18n/language.h"
#include "star_upload_hook.h"   /* v8: 关闭时强制清 upload hook */

static void app_amiibo_on_run(mini_app_inst_t *p_app_inst);
static void app_amiibo_on_kill(mini_app_inst_t *p_app_inst);
static void app_amiibo_on_event(mini_app_inst_t *p_app_inst, mini_app_event_t *p_event);

static void app_amiibo_try_mount_drive(app_amiibo_t *p_app_inst) {
    vfs_driver_t *p_driver = vfs_get_driver(p_app_inst->current_drive);
    if (p_driver->mounted()) {
        amiibo_helper_try_load_amiibo_keys_from_vfs();
    } else {
        int32_t err = p_driver->mount();
        amiibo_helper_try_load_amiibo_keys_from_vfs();
    }
}

void app_amiibo_on_run(mini_app_inst_t *p_app_inst) {

    app_amiibo_t *p_app_handle = mui_mem_malloc(sizeof(app_amiibo_t));
    memset(p_app_handle, 0, sizeof(app_amiibo_t));

    p_app_inst->p_handle = p_app_handle;
    p_app_handle->p_view_dispatcher = mui_view_dispatcher_create();
    p_app_handle->p_amiibo_detail_view = amiibo_detail_view_create();
    p_app_handle->p_amiibo_detail_view->user_data = p_app_handle;
    p_app_handle->p_list_view = mui_list_view_create();
    mui_list_view_set_user_data(p_app_handle->p_list_view, p_app_handle);
    p_app_handle->p_text_input = mui_text_input_create();
    mui_text_input_set_user_data(p_app_handle->p_text_input, p_app_handle);
    p_app_handle->p_msg_box = mui_msg_box_create();
    mui_msg_box_set_user_data(p_app_handle->p_msg_box, p_app_handle);

    p_app_handle->p_toast_view = mui_toast_view_create();
    mui_toast_view_set_user_data(p_app_handle->p_toast_view, p_app_handle);

    p_app_handle->p_scene_dispatcher = mui_scene_dispatcher_create();

    string_init(p_app_handle->current_file);
    string_init(p_app_handle->current_folder);
    string_array_init(p_app_handle->amiibo_files);

    /* v4: 3-level Star navigator state */
    p_app_handle->star_channel_idx = -1;
    string_init(p_app_handle->star_account_dir);
    string_init(p_app_handle->star_badge_pending);
    string_init(p_app_handle->star_badge_autofill);

    /* v8.2: 4 级导航 + 重命名状态 */
    string_init(p_app_handle->star_category_dir);
    p_app_handle->star_rename_mode = false;
    p_app_handle->star_rename_target_kind = STAR_RENAME_KIND_ACCOUNT;

    /* v8: wait_upload 流程相关 */
    string_init(p_app_handle->star_wait_dir_full);
    string_init(p_app_handle->star_wait_placeholder);
    p_app_handle->star_wait_hook_fired  = false;
    p_app_handle->star_wait_rename_mode = false;

    mui_scene_dispatcher_set_user_data(p_app_handle->p_scene_dispatcher, p_app_handle);
    mui_scene_dispatcher_set_scene_defines(p_app_handle->p_scene_dispatcher, amiibo_scene_defines, AMIIBO_SCENE_MAX);
    /* v4: enter Star navigator at channel-select instead of legacy file browser */
    mui_mui_scene_dispatcher_set_default_scene_id(p_app_handle->p_scene_dispatcher, AMIIBO_SCENE_CHANNEL_SELECT);

    mui_view_dispatcher_add_view(p_app_handle->p_view_dispatcher, AMIIBO_VIEW_ID_LIST,
                                 mui_list_view_get_view(p_app_handle->p_list_view));
    mui_view_dispatcher_add_view(p_app_handle->p_view_dispatcher, AMIIBO_VIEW_ID_DETAIL,
                                 amiibo_detail_view_get_view(p_app_handle->p_amiibo_detail_view));
    mui_view_dispatcher_add_view(p_app_handle->p_view_dispatcher, AMIIBO_VIEW_ID_INPUT,
                                 mui_text_input_get_view(p_app_handle->p_text_input));
    mui_view_dispatcher_add_view(p_app_handle->p_view_dispatcher, AMIIBO_VIEW_ID_MSG_BOX,
                                 mui_msg_box_get_view(p_app_handle->p_msg_box));

    mui_view_dispatcher_attach(p_app_handle->p_view_dispatcher, MUI_LAYER_FULLSCREEN);

    p_app_handle->p_view_dispatcher_toast = mui_view_dispatcher_create();
    mui_view_dispatcher_add_view(p_app_handle->p_view_dispatcher_toast, AMIIBO_VIEW_ID_TOAST,
                                 mui_toast_view_get_view(p_app_handle->p_toast_view));
    mui_view_dispatcher_attach(p_app_handle->p_view_dispatcher_toast, MUI_LAYER_TOAST);
    mui_view_dispatcher_switch_to_view(p_app_handle->p_view_dispatcher_toast, AMIIBO_VIEW_ID_TOAST);

    extern const ntag_t default_ntag215;
    APP_ERROR_CHECK(ntag_emu_init(&default_ntag215));

    settings_data_t *p_settings = settings_get_data();

    if (p_app_inst->p_retain_data) {
        app_amiibo_cache_data_t *p_cache_data = (app_amiibo_cache_data_t *)p_app_inst->p_retain_data;
        if (p_cache_data->cached_enabled) {
            p_app_handle->current_drive = p_cache_data->current_drive;
            string_set_str(p_app_handle->current_file, p_cache_data->current_file);
            string_set_str(p_app_handle->current_folder, p_cache_data->current_folder);
            p_app_handle->current_focus_index = p_cache_data->current_focus_index;
            memcpy(&(p_app_handle->ntag), &(cache_get_data()->ntag), sizeof(ntag_t));

            if (p_app_handle->current_drive == VFS_DRIVE_EXT || p_app_handle->current_drive == VFS_DRIVE_INT) {
                app_amiibo_try_mount_drive(p_app_handle);
            }

            /* v4: simplified restore — always come back to the channel selector.
             * The previous "remember exact scene" logic referenced FILE_BROWSER
             * which is no longer the main path. Restoring deep state (badge
             * detail view) across hibernate is feature-creep we're skipping. */
            (void)p_cache_data->current_scene_id;
            mui_scene_dispatcher_next_scene(p_app_handle->p_scene_dispatcher, AMIIBO_SCENE_CHANNEL_SELECT);
            return;
        }
    }

    p_app_handle->current_drive = vfs_get_default_drive();
    string_set_str(p_app_handle->current_folder, "/");
    app_amiibo_try_mount_drive(p_app_handle);
    mui_scene_dispatcher_next_scene(p_app_handle->p_scene_dispatcher, AMIIBO_SCENE_CHANNEL_SELECT);
}

void app_amiibo_on_kill(mini_app_inst_t *p_app_inst) {
    app_amiibo_t *p_app_handle = p_app_inst->p_handle;

    /* v8: 防御性 — 如果 app 在 wait_upload 场景里被强杀 (低电关机 / launcher
     * 切走), 当前还挂着 hook 指针 (user_data 指向即将被释放的 app 句柄).
     * 这里强制清掉, 防止后续 BLE 写入触发 use-after-free. */
    star_upload_hook_clear();

    uint32_t current_scene_id = mui_scene_dispatcher_current_scene(p_app_handle->p_scene_dispatcher);
    if (app_amiibo_info.hibernate_enabled &&
        (current_scene_id == AMIIBO_SCENE_AMIIBO_DETAIL || current_scene_id == AMIIBO_SCENE_FILE_BROWSER)) {
        app_amiibo_cache_data_t p_cache_data = {0};
        p_cache_data.cached_enabled = true;
        strcpy(p_cache_data.current_file, string_get_cstr(p_app_handle->current_file));
        strcpy(p_cache_data.current_folder, string_get_cstr(p_app_handle->current_folder));
        p_cache_data.current_drive = p_app_handle->current_drive;
        p_cache_data.current_scene_id = current_scene_id;
        p_cache_data.current_focus_index = mui_list_view_get_focus(p_app_handle->p_list_view);

        memcpy(p_app_inst->p_retain_data, &p_cache_data, sizeof(app_amiibo_cache_data_t));
    } else {
        memset(p_app_inst->p_retain_data, 0, CACHEDATASIZE);
        memset(&(cache_get_data()->ntag), 0, sizeof(ntag_t));
    }

    mui_view_dispatcher_detach(p_app_handle->p_view_dispatcher, MUI_LAYER_FULLSCREEN);
    mui_view_dispatcher_free(p_app_handle->p_view_dispatcher);
    mui_list_view_free(p_app_handle->p_list_view);
    mui_text_input_free(p_app_handle->p_text_input);
    mui_msg_box_free(p_app_handle->p_msg_box);
    mui_scene_dispatcher_free(p_app_handle->p_scene_dispatcher);
    amiibo_detail_view_free(p_app_handle->p_amiibo_detail_view);

    mui_view_dispatcher_detach(p_app_handle->p_view_dispatcher_toast, MUI_LAYER_TOAST);
    mui_view_dispatcher_free(p_app_handle->p_view_dispatcher_toast);
    mui_toast_view_free(p_app_handle->p_toast_view);

    string_clear(p_app_handle->current_file);
    string_clear(p_app_handle->current_folder);
    string_array_clear(p_app_handle->amiibo_files);

    /* v4 cleanup */
    string_clear(p_app_handle->star_account_dir);
    string_clear(p_app_handle->star_badge_pending);
    string_clear(p_app_handle->star_badge_autofill);

    /* v8.2 cleanup */
    string_clear(p_app_handle->star_category_dir);

    /* v8 cleanup */
    string_clear(p_app_handle->star_wait_dir_full);
    string_clear(p_app_handle->star_wait_placeholder);

    mui_mem_free(p_app_handle);

    p_app_inst->p_handle = NULL;
}

void app_amiibo_on_event(mini_app_inst_t *p_app_inst, mini_app_event_t *p_event) {}

mini_app_t app_amiibo_info = {.id = MINI_APP_ID_AMIIBO,
                              .name = "Star模拟器",
                              .name_i18n_key = _L_APP_AMIIBO,
                              .icon = 0xe082,
                              .sys = false,
                              .deamon = false,
                              .hibernate_enabled = true,
                              .icon_32x32 = &app_amiibo_emulator_32x32,
                              .run_cb = app_amiibo_on_run,
                              .kill_cb = app_amiibo_on_kill,
                              .on_event_cb = app_amiibo_on_event};
