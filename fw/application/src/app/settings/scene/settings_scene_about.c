#include "app_settings.h"
#include "i18n/language.h"
#include "mini_app_launcher.h"
#include "nrf_pwr_mgmt.h"
#include "settings.h"
#include "settings_scene.h"
#include "utils2.h"
#include "version2.h"
#include "mui_icons.h"


static void settings_scene_about_list_view_on_selected(mui_list_view_event_t event, mui_list_view_t *p_list_view,
                                                          mui_list_item_t *p_item) {
    app_settings_t *app = p_list_view->user_data;
    if(p_item->icon == ICON_BACK){
        mui_scene_dispatcher_previous_scene(app->p_scene_dispatcher);
    }
}

void settings_scene_about_on_enter(void *user_data) {

    app_settings_t *app = user_data;

    /* v8.2: 关于页改成 "作者：白染网络 / 测试：益豪 / Star徽章助手 / QQ群" 四行,
     * 不再展示原版 pixl.js 的 "开源项目 / GPL 2.0 授权 / github.com/solosky/pixl.js".
     *
     * 之所以**不**走 i18n key:
     *   - 这几行在中英文界面下都展示一样,
     *     没有翻译诉求, 留作硬编码反而省去给 15 个 locale 文件全部塞同一条
     *     字符串的麻烦.
     *   - QQ 群号是固定 ASCII, 直接 literal.
     */
    mui_list_view_add_item(app->p_list_view, ICON_FILE, "作者：白染网络", NULL_USER_DATA);
    mui_list_view_add_item(app->p_list_view, ICON_FILE, "测试：益豪", NULL_USER_DATA);
    mui_list_view_add_item(app->p_list_view, ICON_FILE, "Star徽章助手", NULL_USER_DATA);
    mui_list_view_add_item(app->p_list_view, ICON_FILE, "QQ群：1032717817", NULL_USER_DATA);
    mui_list_view_add_item(app->p_list_view, ICON_BACK, getLangString(_L_BACK), NULL_USER_DATA);

    mui_list_view_set_selected_cb(app->p_list_view, settings_scene_about_list_view_on_selected);
    mui_view_dispatcher_switch_to_view(app->p_view_dispatcher, SETTINGS_VIEW_ID_MAIN);
}

void settings_scene_about_on_exit(void *user_data) {
    app_settings_t *app = user_data;
    mui_list_view_clear_items(app->p_list_view);
    mui_list_view_set_selected_cb(app->p_list_view, NULL);
}
