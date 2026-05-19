#ifndef APP_AMIIBO_H
#define APP_AMIIBO_H

#include "amiibo_detail_view.h"
#include "mini_app_defines.h"
#include "mui_list_view.h"
#include "mui_msg_box.h"
#include "mui_scene_dispatcher.h"
#include "mui_text_input.h"
#include "mui_toast_view.h"
#include "ntag_def.h"
#include "vfs.h"

#include "mlib_common.h"

typedef struct {
    amiibo_detail_view_t *p_amiibo_detail_view;
    mui_list_view_t *p_list_view;
    mui_text_input_t *p_text_input;
    mui_msg_box_t *p_msg_box;
    mui_toast_view_t *p_toast_view;
    mui_view_dispatcher_t *p_view_dispatcher;
    mui_scene_dispatcher_t *p_scene_dispatcher;
    mui_view_dispatcher_t *p_view_dispatcher_toast;
    ntag_t ntag;

    /** file browser*/
    vfs_drive_t current_drive;
    string_t current_folder;
    string_t current_file;
    uint32_t current_focus_index;

    /**amiibo detail view*/
    string_array_t amiibo_files;
    bool reload_amiibo_files;

    /* === v4: Star 3 级导航 (Channel / Account / Badge) === */
    int32_t star_channel_idx;       /* 当前渠道索引, -1 = 未选 */
    string_t star_account_dir;      /* 当前账号目录名 (磁盘短码或用户输入) */
    string_t star_badge_pending;    /* 待重命名的徽章文件名 (在当前账号目录内) */
    string_t star_badge_autofill;   /* 自动识别得到的徽章名 (prefill text_input) */

    /* === v8.2: 4 级导航 — 在 account 与 badge 之间插入 category 层 ===
     *
     * 磁盘布局:
     *   /star/<channel>/<account>/<category>/<badge>.bin
     *
     *   <category> 中:
     *      "_default"  = 系统默认分类 (不可改名/删除, 进入账号时自动创建)
     *      其它字符串  = 用户自建分类, 显示名 = 目录名, 可改名/删除
     *
     * 状态字段:
     *   star_category_dir   : 当前分类的磁盘目录名 (短码 "_default" 或用户输入)
     *   star_rename_mode    : true=text_input 当 "rename" 用 (从长按菜单进入),
     *                         false=当 "new" 用 (从 "新建账号/分类" 入口进入)
     *   star_rename_target_kind: 当前 rename/new 输入对应的对象类型
     *                         (account / category), 让 text_input 回调
     *                         能区分要去哪个目录做 rename / mkdir.
     */
    string_t star_category_dir;
    bool     star_rename_mode;
    uint8_t  star_rename_target_kind;   /* 0=account, 1=category */

    /* === v8: "点新建徽章 -> 自动接收 BLE 数据 -> 自动识别 -> 自动改名" 流程 === */
    string_t star_wait_dir_full;       /* hook 监视前缀, 例 "E:/star/netease/default/_default/" */
    string_t star_wait_placeholder;    /* 占位文件名 (例 "new.bin", 可能带 _N 后缀) */
    bool     star_wait_hook_fired;     /* 本次等待中 hook 是否触发过 */
    bool     star_wait_rename_mode;    /* true=从 badge_list 长按进入的"改名"模式 */

} app_amiibo_t;

/* v8.2: star_rename_target_kind 取值 */
#define STAR_RENAME_KIND_ACCOUNT   0
#define STAR_RENAME_KIND_CATEGORY  1
/* v8.2-fix5: badge 也走 action_menu (长按徽章 -> 重命名/删除/取消).
 * 与 ACCOUNT/CATEGORY 不同的是, 它的目标是一个 .bin 文件而不是目录:
 *   - 重命名: 复用 BADGE_NAME_INPUT (用 app->star_badge_pending /
 *             star_badge_autofill / star_wait_rename_mode 这三个老字段)
 *   - 删除  : 直接 remove_file 单文件, 双 pop 回 badge_list */
#define STAR_RENAME_KIND_BADGE     2

typedef enum {
    AMIIBO_VIEW_ID_LIST,
    AMIIBO_VIEW_ID_DETAIL,
    AMIIBO_VIEW_ID_INPUT,
    AMIIBO_VIEW_ID_MSG_BOX,
    AMIIBO_VIEW_ID_TOAST
} amiibo_view_id_t;

typedef struct {
    bool cached_enabled;
    vfs_drive_t current_drive;
    char current_folder[128];
    char current_file[64];
    uint32_t current_focus_index;
    uint32_t current_scene_id;
} app_amiibo_cache_data_t;

extern mini_app_t app_amiibo_info;

#endif