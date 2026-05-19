#ifndef APP_ACTIVATION_H
#define APP_ACTIVATION_H

#include "mini_app_defines.h"
#include "mini_app_registry.h"
#include "mui_include.h"

#include "mui_list_view.h"
#include "mui_text_input.h"
#include "mui_msg_box.h"
#include "mui_toast_view.h"

typedef struct {
    mui_list_view_t       *p_list_view;
    mui_text_input_t      *p_text_input;
    mui_msg_box_t         *p_msg_box;
    mui_view_dispatcher_t *p_view_dispatcher;
    mui_scene_dispatcher_t *p_scene_dispatcher;

    mui_toast_view_t      *p_toast_view;
    mui_view_dispatcher_t *p_view_dispatcher_toast;
} app_activation_t;

typedef enum {
    ACTIVATION_VIEW_ID_LIST = 0,
    ACTIVATION_VIEW_ID_INPUT,
    ACTIVATION_VIEW_ID_MSG_BOX,
    ACTIVATION_VIEW_ID_TOAST
} activation_view_id_t;

extern mini_app_t app_activation_info;

#endif /* APP_ACTIVATION_H */
