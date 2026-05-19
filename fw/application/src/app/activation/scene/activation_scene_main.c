/*
 * activation_scene_main.c
 *
 * Single scene with the activation UI:
 *   - Shows the device PIN.
 *   - Lets the user enter the activation code.
 *   - On correct code → persists state, transitions to the desktop.
 *   - On wrong code → shows a msg box and returns to the list.
 *
 * The scene cannot be backed out of by design (no exit/back item in
 * the list). The hardware long-press still triggers system sleep via
 * the existing bsp shutdown handler, so the device isn't unrecoverable.
 */

#include "app_activation.h"
#include "activation_scene.h"
#include "activation.h"

#include "mui_icons.h"
#include "mini_app_launcher.h"
#include "mini_app_registry.h"

#include <stdlib.h>
#include <string.h>

/* Buffer for the formatted PIN line ("PIN: 12345678") — module-local
 * so it stays alive while the list_view is showing it. */
static char m_pin_line[24];

static void format_pin_line(void) {
    snprintf(m_pin_line, sizeof(m_pin_line), "PIN: %s",
             activation_get_pin_string());
}

/* ------------------------------------------------------------ */
/*  Forward decls                                               */
/* ------------------------------------------------------------ */

static void show_main_list(app_activation_t *app);
static void enter_text_input(app_activation_t *app);

/* ------------------------------------------------------------ */
/*  Msg box callback (after wrong-code error)                   */
/* ------------------------------------------------------------ */

static void on_msg_box_event(mui_msg_box_event_t event, mui_msg_box_t *p_msg_box) {
    app_activation_t *app = p_msg_box->user_data;
    /* Any button press dismisses the error and returns to the list. */
    (void)event;
    show_main_list(app);
}

static void show_error(app_activation_t *app, const char *msg) {
    mui_msg_box_set_header(app->p_msg_box, "提示");
    mui_msg_box_set_message(app->p_msg_box, msg);
    mui_msg_box_set_btn_text(app->p_msg_box, NULL, "确定", NULL);
    mui_msg_box_set_btn_focus(app->p_msg_box, 1);
    mui_msg_box_set_event_cb(app->p_msg_box, on_msg_box_event);
    mui_view_dispatcher_switch_to_view(app->p_view_dispatcher,
                                       ACTIVATION_VIEW_ID_MSG_BOX);
}

/* ------------------------------------------------------------ */
/*  Text-input callback (user pressed Enter on the code)        */
/* ------------------------------------------------------------ */

static void on_text_input_event(mui_text_input_event_t event,
                                mui_text_input_t *p_text_input) {
    app_activation_t *app = p_text_input->user_data;

    if (event == MUI_TEXT_INPUT_EVENT_CANCELLED) {
        /* User backed out of the keyboard — return to PIN view. */
        show_main_list(app);
        return;
    }
    if (event != MUI_TEXT_INPUT_EVENT_CONFIRMED) {
        return;
    }

    const char *text = mui_text_input_get_input_text(p_text_input);
    if (text == NULL || text[0] == '\0') {
        show_error(app, "请输入激活码");
        return;
    }

    /* atol so we handle the (rare) negative case where PIN <= 27.
     * Long is at least 32 bits on this toolchain. */
    long entered = atol(text);

    if (activation_try_activate((int32_t)entered)) {
        /* Activated. Hand off to the desktop. The launcher will
         * kill this app on its way to launching the new one. */
        mini_app_launcher_run(mini_app_launcher(), MINI_APP_ID_DESKTOP);
        return;
    }

    show_error(app, "激活码错误");
}

/* ------------------------------------------------------------ */
/*  List-view callback (user selected an item)                  */
/* ------------------------------------------------------------ */

static void on_list_selected(mui_list_view_event_t event,
                             mui_list_view_t *p_list_view,
                             mui_list_item_t *p_item) {
    app_activation_t *app = p_list_view->user_data;
    if (p_item == NULL) return;
    /* item->user_data carries the action tag. */
    intptr_t tag = (intptr_t)p_item->user_data;
    if (tag == 1) {
        /* "输入激活码" */
        enter_text_input(app);
    }
    /* Other items (PIN/ID display lines) are non-actionable. */
}

/* ------------------------------------------------------------ */
/*  Screen builders                                             */
/* ------------------------------------------------------------ */

static void show_main_list(app_activation_t *app) {
    format_pin_line();

    mui_list_view_clear_items(app->p_list_view);
    mui_list_view_add_item(app->p_list_view, ICON_KEY,  m_pin_line,        (void *)0);
    mui_list_view_add_item(app->p_list_view, ICON_NEW,  "输入激活码",       (void *)1);
    mui_list_view_set_selected_cb(app->p_list_view, on_list_selected);

    mui_view_dispatcher_switch_to_view(app->p_view_dispatcher,
                                       ACTIVATION_VIEW_ID_LIST);
}

static void enter_text_input(app_activation_t *app) {
    mui_text_input_clear_input_text(app->p_text_input);
    mui_text_input_set_header(app->p_text_input, "输入激活码");
    mui_text_input_set_event_cb(app->p_text_input, on_text_input_event);
    /* Start cursor on '1' so the user is immediately on a digit. */
    mui_text_input_set_focus_key(app->p_text_input, '1');
    mui_view_dispatcher_switch_to_view(app->p_view_dispatcher,
                                       ACTIVATION_VIEW_ID_INPUT);
}

/* ------------------------------------------------------------ */
/*  Scene entry / exit                                          */
/* ------------------------------------------------------------ */

void activation_scene_main_on_enter(void *user_data) {
    app_activation_t *app = user_data;
    show_main_list(app);
}

void activation_scene_main_on_exit(void *user_data) {
    app_activation_t *app = user_data;
    mui_list_view_set_selected_cb(app->p_list_view, NULL);
    mui_list_view_clear_items(app->p_list_view);
}
