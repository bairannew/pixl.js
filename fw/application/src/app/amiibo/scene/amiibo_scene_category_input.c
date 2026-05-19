/*
 * amiibo_scene_category_input.c   (v8.2 新增)
 *
 * 分类文本输入界面 — 新建 + 重命名 两用.
 *
 *   app->star_rename_mode == false : 新建 — 留空, 确认后 mkdir
 *       /star/<ch>/<acct>/<name>
 *   app->star_rename_mode == true  : 重命名 — 预填 star_category_dir,
 *       确认后 rename old -> new.
 *
 * 路径预算: 比账号名更紧 — 因为多了一层 account.
 */
#include "amiibo_scene.h"
#include "app_amiibo.h"
#include "i18n/language.h"
#include "nrf_log.h"
#include "vfs.h"
#include "port/star_channels.h"
#include "df_proto_vfs.h"   /* v8.2-fix11: 强制关 BLE 上传句柄, 修"蓝牙连接下重命名失败" */

#include <stdio.h>
#include <string.h>

#define CATEGORY_DEFAULT_DIR  "_default"

/* category 路径预算计算:
 *   /star/<ch>/<acct>/<cat>/<badge>.bin
 *
 *   留 RESERVE_BADGE 字节给后续 badge (含 .bin 扩展名 + 一些中文).
 *   16 = "X.bin" 加 ~3 个汉字, 长徽章名靠后续 UTF-8 安全裁剪兜底.
 *   HARD_MAX = 12 字节上限 (4 个汉字), 防止路径吃太多. */
#define STAR_CATEGORY_RESERVE_BADGE   16
#define STAR_CATEGORY_NAME_HARD_MAX   12

static size_t star_category_name_max_bytes(app_amiibo_t *app) {
    const star_channel_t *ch = star_channels_get(app->star_channel_idx);
    if (ch == NULL) return 1;
    /* dir_len = "/star/<ch>/<acct>" */
    size_t dir_len = strlen(STAR_ROOT_FOLDER) + 1 +
                     strlen(ch->dir_name) + 1 +
                     strlen(string_get_cstr(app->star_account_dir));
    /* 预算: VFS_MAX_PATH_LEN - dir_len - 2 (两个 '/') - RESERVE_BADGE - 1 ('\0') */
    if (VFS_MAX_PATH_LEN <= dir_len + 2 + STAR_CATEGORY_RESERVE_BADGE + 1) {
        return 1;
    }
    size_t budget = VFS_MAX_PATH_LEN - dir_len - 2 - STAR_CATEGORY_RESERVE_BADGE - 1;
    if (budget > STAR_CATEGORY_NAME_HARD_MAX) budget = STAR_CATEGORY_NAME_HARD_MAX;
    return budget;
}

static bool is_valid_category_name(app_amiibo_t *app, const char *name) {
    if (name == NULL || name[0] == '\0') return false;
    /* 不允许默认分类目录名 */
    if (strcmp(name, CATEGORY_DEFAULT_DIR) == 0) return false;
    /* 路径分隔符 */
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return false;
    size_t max_bytes = star_category_name_max_bytes(app);
    if (max_bytes == 0) return false;
    if (strlen(name) > max_bytes) return false;
    return true;
}

static bool format_category_path(app_amiibo_t *app, const char *cat_name,
                                  char *out, size_t cap) {
    const star_channel_t *ch = star_channels_get(app->star_channel_idx);
    if (ch == NULL) return false;
    int n = snprintf(out, cap, "%s/%s/%s/%s",
                     STAR_ROOT_FOLDER, ch->dir_name,
                     string_get_cstr(app->star_account_dir),
                     cat_name);
    return n > 0 && (size_t)n < cap;
}

static bool star_category_rename(app_amiibo_t *app, const char *new_name) {
    vfs_driver_t *p_drv = vfs_get_driver(app->current_drive);
    char old_path[VFS_MAX_PATH_LEN];
    char new_path[VFS_MAX_PATH_LEN];
    if (!format_category_path(app, string_get_cstr(app->star_category_dir),
                              old_path, sizeof(old_path))) return false;
    if (!format_category_path(app, new_name, new_path, sizeof(new_path))) return false;
    if (strcmp(old_path, new_path) == 0) return true;

    vfs_obj_t obj;
    if (p_drv->stat_file(new_path, &obj) == VFS_OK) return false;

    /* v9.0-fix1: 修复 "蓝牙模式下分类重命名静默失败" — 与 account_input.c
     * 同根. 关 + rename 重复 3 次, 吸收 BLE 客户端的轮询抢占. 见 account_input
     * 同函数注释.
     *
     * v9.0-fix2 升级: 仅关 file 句柄还不够 — BLE dir_read 是分块协议, 客户端
     * 在两次发包之间设备会保留 dir 句柄. 这里把 active_dir 一并关掉. */
    int32_t r = VFS_ERR_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        df_proto_vfs_close_active_upload();
        df_proto_vfs_close_active_dir();
        r = p_drv->rename_dir(old_path, new_path);
        if (r == VFS_OK) break;
        NRF_LOG_WARNING("category rename attempt %d failed (%d), retrying after force-close",
                        attempt, (int)r);
    }
    if (r != VFS_OK) {
        NRF_LOG_ERROR("category rename failed (%d)", (int)r);
        return false;
    }
    string_set_str(app->star_category_dir, new_name);
    return true;
}

static void amiibo_scene_category_input_text_cb(mui_text_input_event_t event,
                                                 mui_text_input_t *p_text_input) {
    app_amiibo_t *app = p_text_input->user_data;

    /* v8.2-fix6: rename 模式下入口栈是
     *   [..., category_select, action_menu, category_input]
     * 用户期望操作完直接回到 category_select, 跳过 action_menu.
     * 新建模式入口栈只有一层 category_input, previous_scene 单层退栈即可. */
    const bool was_rename = app->star_rename_mode;
    #define CATEGORY_INPUT_POP_BACK()                                           \
        do {                                                                    \
            if (was_rename) {                                                   \
                mui_scene_dispatcher_back_scene(app->p_scene_dispatcher, 2);    \
            } else {                                                            \
                mui_scene_dispatcher_previous_scene(app->p_scene_dispatcher);   \
            }                                                                   \
        } while (0)

    if (event != MUI_TEXT_INPUT_EVENT_CONFIRMED) {
        CATEGORY_INPUT_POP_BACK();
        return;
    }
    const char *name = mui_text_input_get_input_text(p_text_input);
    if (!is_valid_category_name(app, name)) {
        mui_toast_view_show(app->p_toast_view,
                            getLangString(_L_STAR_CATEGORY_NAME_INVALID));
        CATEGORY_INPUT_POP_BACK();
        return;
    }
    if (app->star_rename_mode) {
        bool ok = star_category_rename(app, name);
        if (!ok) {
            mui_toast_view_show(app->p_toast_view, getLangString(_L_FAILED));
        } else {
            /* v9.0-fix1: 成功也给 toast, 见 account_input 同位置注释.
             * v9.0-fix2: 改成专用 "分类已重命名" 文案, 不再借用
             * "徽章已添加" 这种语义不对的 toast. */
            mui_toast_view_show(app->p_toast_view, getLangString(_L_STAR_CATEGORY_RENAMED));
        }
        app->star_rename_mode = false;
        CATEGORY_INPUT_POP_BACK();
        return;
    }
    /* 新建 */
    char path[VFS_MAX_PATH_LEN];
    if (!format_category_path(app, name, path, sizeof(path))) {
        mui_toast_view_show(app->p_toast_view, getLangString(_L_FAILED));
        CATEGORY_INPUT_POP_BACK();
        return;
    }
    vfs_driver_t *p_drv = vfs_get_driver(app->current_drive);
    vfs_obj_t obj;
    if (p_drv->stat_file(path, &obj) == VFS_OK) {
        mui_toast_view_show(app->p_toast_view,
                            getLangString(_L_STAR_CATEGORY_EXISTS));
        CATEGORY_INPUT_POP_BACK();
        return;
    }
    int32_t res = p_drv->create_dir(path);
    if (res != VFS_OK) {
        mui_toast_view_show(app->p_toast_view, getLangString(_L_FAILED));
    }
    CATEGORY_INPUT_POP_BACK();
    #undef CATEGORY_INPUT_POP_BACK
}

void amiibo_scene_category_input_on_enter(void *user_data) {
    app_amiibo_t *app = user_data;
    if (app->star_rename_mode) {
        mui_text_input_set_header(app->p_text_input,
                                  getLangString(_L_STAR_INPUT_NEW_CATEGORY_NAME));
        mui_text_input_set_input_text(app->p_text_input,
                                      string_get_cstr(app->star_category_dir));
    } else {
        mui_text_input_set_header(app->p_text_input,
                                  getLangString(_L_STAR_INPUT_CATEGORY_NAME));
        mui_text_input_set_input_text(app->p_text_input, "");
    }
    mui_text_input_set_event_cb(app->p_text_input, amiibo_scene_category_input_text_cb);
    mui_view_dispatcher_switch_to_view(app->p_view_dispatcher, AMIIBO_VIEW_ID_INPUT);
}

void amiibo_scene_category_input_on_exit(void *user_data) {
    app_amiibo_t *app = user_data;
    if (app != NULL) {
        app->star_rename_mode = false;
    }
}
