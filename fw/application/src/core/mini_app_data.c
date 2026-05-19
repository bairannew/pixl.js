#include "mini_app_defines.h"
#include "app_status_bar.h"
#include "app_desktop.h"
#include "app_amiibo.h"
#include "app_ble.h"
#include "app_player.h"
#include "app_settings.h"
#include "app_amiibolink.h"
#include "app_amiidb.h"
#include "app_chameleon.h"
#include "app_game.h"
#include "app_activation.h"
#include <stddef.h>

// ============================================================
//  光遇徽章定制版 app 注册表 (Sky Badge Edition)
//  --------------------------------------------------------
//  保留: status_bar / desktop / amiibo (Star模拟器) / settings
//  注释掉: amiidb / amiibolink / chameleon / game / player
//  目的: 专注光遇徽章模拟功能, 移除无关菜单项
//  说明: 光遇徽章本质是 NTAG215, 通过 amiibo 模拟器 (现已重命名为
//        "Star 模拟器") 即可加载 .bin dump 并模拟, 不需要 chameleon
//        的通用卡片菜单。chameleon 源码保留在仓库里但不挂菜单。
//  如需恢复某项, 把对应行的注释去掉即可
// ============================================================

const mini_app_t* mini_app_registry[] = {
    &app_status_bar_info,
    &app_desktop_info,
#ifdef APP_LEGLAMIIBO_ENABLE
    &app_amiibo_info,           // [光遇定制] Star模拟器 = 光遇徽章核心 (NTAG215)
#endif
    // &app_amiidb_info,        // [光遇定制] 关闭 amiibo 数据库
    // &app_amiibolink_info,    // [光遇定制] 关闭 AmiiboLink 兼容
    // &app_chameleon_info,     // [光遇定制] 关闭"光遇徽章"二级菜单, 改走 Star模拟器
#ifdef APP_PLAYER_ENABLE
    &app_player_info,
#endif
#ifdef APP_GAME_ENABLE
    &app_game_info,
#endif
#ifdef APP_LEGLAMIIBO_ENABLE
    &app_ble_info, 
#endif
    &app_settings_info,
    /* Activation gate — sys=true, never appears in the desktop launcher.
     * mini_app_launcher gates first launch on this when not yet activated. */
    &app_activation_info
};

const uint32_t mini_app_num = sizeof(mini_app_registry) / sizeof(mini_app_registry[0]);
