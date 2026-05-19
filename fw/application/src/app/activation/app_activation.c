/*
 * app_activation.c
 *
 * Activation gate app. Launched in place of the desktop on first
 * boot (or after factory wipe). Displays the device PIN and accepts
 * the activation code. On success, persists state and kills itself —
 * the launcher will then bring up the desktop.
 */

#include "app_activation.h"
#include "activation_scene.h"

#include "activation.h"
#include "mui_include.h"

static void app_activation_on_run(mini_app_inst_t *p_app_inst);
static void app_activation_on_kill(mini_app_inst_t *p_app_inst);
static void app_activation_on_event(mini_app_inst_t *p_app_inst, mini_app_event_t *p_event);

void app_activation_on_run(mini_app_inst_t *p_app_inst) {
    app_activation_t *p_app = mui_mem_malloc(sizeof(app_activation_t));
    memset(p_app, 0, sizeof(*p_app));
    p_app_inst->p_handle = p_app;

    p_app->p_view_dispatcher = mui_view_dispatcher_create();

    p_app->p_list_view = mui_list_view_create();
    mui_list_view_set_user_data(p_app->p_list_view, p_app);

    p_app->p_text_input = mui_text_input_create();
    mui_text_input_set_user_data(p_app->p_text_input, p_app);

    p_app->p_msg_box = mui_msg_box_create();
    mui_msg_box_set_user_data(p_app->p_msg_box, p_app);

    p_app->p_scene_dispatcher = mui_scene_dispatcher_create();
    mui_scene_dispatcher_set_user_data(p_app->p_scene_dispatcher, p_app);
    mui_scene_dispatcher_set_scene_defines(p_app->p_scene_dispatcher,
                                           activation_scene_defines,
                                           ACTIVATION_SCENE_MAX);

    mui_view_dispatcher_add_view(p_app->p_view_dispatcher, ACTIVATION_VIEW_ID_LIST,
                                 mui_list_view_get_view(p_app->p_list_view));
    mui_view_dispatcher_add_view(p_app->p_view_dispatcher, ACTIVATION_VIEW_ID_INPUT,
                                 mui_text_input_get_view(p_app->p_text_input));
    mui_view_dispatcher_add_view(p_app->p_view_dispatcher, ACTIVATION_VIEW_ID_MSG_BOX,
                                 mui_msg_box_get_view(p_app->p_msg_box));

    mui_view_dispatcher_attach(p_app->p_view_dispatcher, MUI_LAYER_FULLSCREEN);

    p_app->p_toast_view = mui_toast_view_create();
    mui_toast_view_set_user_data(p_app->p_toast_view, p_app);
    p_app->p_view_dispatcher_toast = mui_view_dispatcher_create();
    mui_view_dispatcher_add_view(p_app->p_view_dispatcher_toast, ACTIVATION_VIEW_ID_TOAST,
                                 mui_toast_view_get_view(p_app->p_toast_view));
    mui_view_dispatcher_attach(p_app->p_view_dispatcher_toast, MUI_LAYER_TOAST);
    mui_view_dispatcher_switch_to_view(p_app->p_view_dispatcher_toast,
                                       ACTIVATION_VIEW_ID_TOAST);

    mui_scene_dispatcher_next_scene(p_app->p_scene_dispatcher, ACTIVATION_SCENE_MAIN);
}

void app_activation_on_kill(mini_app_inst_t *p_app_inst) {
    app_activation_t *p_app = p_app_inst->p_handle;
    if (p_app == NULL) return;

    mui_scene_dispatcher_free(p_app->p_scene_dispatcher);
    mui_view_dispatcher_detach(p_app->p_view_dispatcher, MUI_LAYER_FULLSCREEN);
    mui_view_dispatcher_free(p_app->p_view_dispatcher);

    mui_list_view_free(p_app->p_list_view);
    mui_text_input_free(p_app->p_text_input);
    mui_msg_box_free(p_app->p_msg_box);

    mui_toast_view_free(p_app->p_toast_view);
    mui_view_dispatcher_detach(p_app->p_view_dispatcher_toast, MUI_LAYER_TOAST);
    mui_view_dispatcher_free(p_app->p_view_dispatcher_toast);

    mui_mem_free(p_app);
    p_app_inst->p_handle = NULL;
}

void app_activation_on_event(mini_app_inst_t *p_app_inst, mini_app_event_t *p_event) {
    /* No external events handled. */
}

mini_app_t app_activation_info = {
    .id              = MINI_APP_ID_ACTIVATION,
    .name            = "设备激活",
    .name_i18n_key   = 0,                /* not localized — boot gate only */
    .icon            = 0xe01b,           /* ICON_KEY */
    .deamon          = false,
    .sys             = true,             /* system app, not user-launchable */
    .hibernate_enabled = false,
    .icon_32x32      = &app_settings_32x32, /* reused: never shown in launcher */
    .run_cb          = app_activation_on_run,
    .kill_cb         = app_activation_on_kill,
    .on_event_cb     = app_activation_on_event,
};
