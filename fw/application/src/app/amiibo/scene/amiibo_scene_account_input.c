/*
 * amiibo_scene_account_input.c
 *
 * 账号文本输入界面 — 同时承担 "新建账号" 和 "重命名账号" 两种模式.
 *
 *   v8.2 起, 复用同一个 scene:
 *     app->star_rename_mode == false : 新建 — text_input 留空,
 *         确认后在 /star/<ch>/ 下 mkdir <name> 并把新账号设为 current.
 *     app->star_rename_mode == true  : 重命名 — text_input 预填
 *         app->star_account_dir, 确认后 rename old -> new.
 *         (默认账号在调用方已被拦截, 不会进到这里.)
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

/* v8.2: 账号名最大字节数受 VFS 路径预算约束.
 *
 * 完整路径形如 "/star/<channel>/<account>/<category>/<badge>".
 * v8.1 在这里 reserve 了 VFS_MAX_NAME_LEN-1=47 字节给未来 badge 文件名,
 * 算到 huawei_global 之类 13 字符长渠道时, 直接返回 budget=0 / -5,
 * 导致输入两个中文字 (6 字节) 都被判 "账号名无效".
 *
 * v8.2 改: 给后续 "<category>/<badge>" 整体留 28 字节预算 (够
 * "_default/X.bin" 这种最短情况 + 一些中文徽章名), 剩下的全部留给账号名,
 * 同时仍受 12 字节上限约束以保证 4 级路径不爆.
 *
 *   预算公式:
 *      budget_for_account =
 *          VFS_MAX_PATH_LEN - dir_len - 2 (两个'/') - RESERVE_CHILD - 1 ('\0')
 *
 *   RESERVE_CHILD = 28 (够 "_default/Xx.bin" + 余量, 长徽章名靠
 *                       badge_name_input 的 UTF-8 安全裁剪兜底).
 *
 *   account 上限 = min(budget_for_account, 16) — 16 字节足够 5 个汉字.
 */
#define STAR_ACCOUNT_RESERVE_CHILD   28
#define STAR_ACCOUNT_NAME_HARD_MAX   16

static size_t star_account_name_max_bytes(uint32_t channel_idx) {
    const star_channel_t *ch = star_channels_get(channel_idx);
    const char *chname = ch ? ch->dir_name : "unknown";
    /* 目录长度 = "/star" + "/" + chname */
    size_t dir_len = strlen(STAR_ROOT_FOLDER) + 1 + strlen(chname);
    /* 预算: VFS_MAX_PATH_LEN - dir_len - 2 (两个 '/') - RESERVE_CHILD - 1 ('\0') */
    if (VFS_MAX_PATH_LEN <= dir_len + 2 + STAR_ACCOUNT_RESERVE_CHILD + 1) {
        /* 极端情况: 渠道目录就吃了大部分路径 — 仍然允许 1 字节, 后续
         * badge 名只能 1-2 字符, 用户能看到但能感知到拥挤 */
        return 1;
    }
    size_t budget = VFS_MAX_PATH_LEN - dir_len - 2 - STAR_ACCOUNT_RESERVE_CHILD - 1;
    if (budget > STAR_ACCOUNT_NAME_HARD_MAX) budget = STAR_ACCOUNT_NAME_HARD_MAX;
    return budget;
}

static bool is_valid_account_name(const char *name, uint32_t channel_idx) {
    if (name == NULL || name[0] == '\0') return false;
    /* 不允许 "default" (会与系统默认账号冲突) */
    if (strcmp(name, STAR_DEFAULT_ACCOUNT_DIR) == 0) return false;
    /* 不允许包含路径分隔符 */
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return false;
    /* 按当前渠道路径预算动态限制账号名字节长度 */
    size_t max_bytes = star_account_name_max_bytes(channel_idx);
    if (max_bytes == 0) return false;
    if (strlen(name) > max_bytes) return false;
    return true;
}

/* 把 /star/<ch>/<old_name> 递归内容 (子目录+文件) 移到 /star/<ch>/<new_name>.
 * 思路: 直接 rename_file 老目录 (LFS/FATFS 都支持目录 rename).
 * 失败时返回 false. */
static bool star_account_rename(app_amiibo_t *app, const char *new_name) {
    const star_channel_t *ch = star_channels_get(app->star_channel_idx);
    if (ch == NULL) return false;
    vfs_driver_t *p_drv = vfs_get_driver(app->current_drive);

    char old_path[VFS_MAX_PATH_LEN];
    char new_path[VFS_MAX_PATH_LEN];
    int n1 = snprintf(old_path, sizeof(old_path), "%s/%s/%s",
                      STAR_ROOT_FOLDER, ch->dir_name,
                      string_get_cstr(app->star_account_dir));
    int n2 = snprintf(new_path, sizeof(new_path), "%s/%s/%s",
                      STAR_ROOT_FOLDER, ch->dir_name, new_name);
    if (n1 < 0 || (size_t)n1 >= sizeof(old_path)) return false;
    if (n2 < 0 || (size_t)n2 >= sizeof(new_path)) return false;

    /* 同名 — 啥也不做, 视为成功 */
    if (strcmp(old_path, new_path) == 0) return true;

    /* 目标已存在? 拒绝 */
    vfs_obj_t obj;
    if (p_drv->stat_file(new_path, &obj) == VFS_OK) return false;

    /* v9.0-fix1: 修复 "蓝牙模式下账号重命名静默失败".
     *
     * 用户反馈 (v8.2-fix11 之后):
     *   "蓝牙模式下，修改账号文件夹名称和分类文件夹名称，点击确定修改后
     *    无法修改 什么也不提示"
     *
     * v8.2-fix11 加了 df_proto_vfs_close_active_upload() 兜底, 但实测仍然
     * 失败. 根因: BLE 客户端 (网页 / 手机 app) 通常会主动轮询账号目录
     * (列目录 / 读 meta), close 之后毫秒级再次 open. 我们关掉句柄后, 在
     * 走到 rename_dir 之前, 客户端那一帧轮询又把另一个文件打开了 →
     * LittleFS 仍然拒绝 rename (父目录有活句柄).
     *
     * 解法: 关 → rename, 重复最多 3 次. 每次都先强制关一遍再 rename,
     * 给客户端的下一帧 polling 落到关闭窗口里. 实测 3 次足够吸收任何
     * 现行 BLE 客户端的轮询节奏.
     *
     * v9.0-fix2 升级: 仅关 file 句柄还不够 — BLE dir_read 是分块协议,
     * 客户端在两次发包之间设备会保留 dir 句柄, 这条路径之前没人覆盖.
     * 这里把 active_dir 一并关掉, 然后才是历史的关 file + rename 循环. */
    int32_t r = VFS_ERR_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        df_proto_vfs_close_active_upload();
        df_proto_vfs_close_active_dir();
        r = p_drv->rename_dir(old_path, new_path);
        if (r == VFS_OK) break;
        NRF_LOG_WARNING("account rename attempt %d failed (%d), retrying after force-close",
                        attempt, (int)r);
    }
    if (r != VFS_OK) {
        NRF_LOG_ERROR("account rename %s -> %s failed (%d)",
                      nrf_log_push(old_path), nrf_log_push(new_path), (int)r);
        return false;
    }
    /* 同步 app 状态: 当前 account_dir 改成新名, 让上层重进 account_select
     * 时不会拿着失效的旧目录名 */
    string_set_str(app->star_account_dir, new_name);
    return true;
}

static void amiibo_scene_account_input_text_cb(mui_text_input_event_t event,
                                                mui_text_input_t *p_text_input) {
    app_amiibo_t *app = p_text_input->user_data;

    /* v8.2-fix6: 在 rename 模式下, 入口栈是
     *   [..., account_select, action_menu, account_input]
     * 用户期望操作完 (无论确认 / 取消 / 校验失败) 直接回到 account_select,
     * 不要再回到 action_menu 那一层. 用 back_scene(2) 一次性 pop 两层.
     *
     * 新建模式 (rename_mode == false) 入口栈是
     *   [..., account_select, account_input]
     * 这种情况下保留 previous_scene 单层退栈即可. */
    const bool was_rename = app->star_rename_mode;
    #define ACCOUNT_INPUT_POP_BACK()                                            \
        do {                                                                    \
            if (was_rename) {                                                   \
                mui_scene_dispatcher_back_scene(app->p_scene_dispatcher, 2);    \
            } else {                                                            \
                mui_scene_dispatcher_previous_scene(app->p_scene_dispatcher);   \
            }                                                                   \
        } while (0)

    if (event != MUI_TEXT_INPUT_EVENT_CONFIRMED) {
        /* CANCELLED 或未来其它: 直接回退 */
        ACCOUNT_INPUT_POP_BACK();
        return;
    }

    const char *name = mui_text_input_get_input_text(p_text_input);

    if (!is_valid_account_name(name, app->star_channel_idx)) {
        mui_toast_view_show(app->p_toast_view,
                            getLangString(_L_STAR_ACCOUNT_NAME_INVALID));
        ACCOUNT_INPUT_POP_BACK();
        return;
    }

    const star_channel_t *ch = star_channels_get(app->star_channel_idx);
    if (ch == NULL) {
        ACCOUNT_INPUT_POP_BACK();
        return;
    }

    /* 分支: 新建 vs 重命名 */
    if (app->star_rename_mode) {
        bool ok = star_account_rename(app, name);
        if (!ok) {
            mui_toast_view_show(app->p_toast_view, getLangString(_L_FAILED));
        } else {
            /* v9.0-fix1: 成功路径也给 toast. 老版本只有失败 toast, 用户
             * 即使成功也以为"什么都没发生"(尤其是改成的名字字形跟原来
             * 接近的时候), 跟报障 "什么也不提示" 一致.
             *
             * v9.0-fix2: 改成专用文案 "账号已重命名". 之前借用了
             * _L_STAR_BADGE_SAVED = "徽章已添加", 用户改完账号看到
             * "徽章已添加" 反而以为没改成功 — 这也是 "什么也不提示"
             * 报障的一部分语义错位. */
            mui_toast_view_show(app->p_toast_view, getLangString(_L_STAR_ACCOUNT_RENAMED));
        }
        app->star_rename_mode = false;
        /* was_rename 拍照在 reset 之前, ACCOUNT_INPUT_POP_BACK 仍然走 2 层 */
        ACCOUNT_INPUT_POP_BACK();
        return;
    }

    /* 新建模式 */
    char path[VFS_MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s/%s",
             STAR_ROOT_FOLDER, ch->dir_name, name);

    vfs_driver_t *p_drv = vfs_get_driver(app->current_drive);
    vfs_obj_t obj;
    if (p_drv->stat_file(path, &obj) == VFS_OK) {
        mui_toast_view_show(app->p_toast_view,
                            getLangString(_L_STAR_ACCOUNT_EXISTS));
        ACCOUNT_INPUT_POP_BACK();
        return;
    }

    int32_t res = p_drv->create_dir(path);
    if (res != VFS_OK) {
        mui_toast_view_show(app->p_toast_view, getLangString(_L_FAILED));
    }
    ACCOUNT_INPUT_POP_BACK();
    #undef ACCOUNT_INPUT_POP_BACK
}

void amiibo_scene_account_input_on_enter(void *user_data) {
    app_amiibo_t *app = user_data;

    /* 根据 rename_mode 选标题 + 预填 */
    if (app->star_rename_mode) {
        mui_text_input_set_header(app->p_text_input,
                                  getLangString(_L_STAR_INPUT_NEW_ACCOUNT_NAME));
        mui_text_input_set_input_text(app->p_text_input,
                                      string_get_cstr(app->star_account_dir));
    } else {
        mui_text_input_set_header(app->p_text_input,
                                  getLangString(_L_STAR_INPUT_ACCOUNT_NAME));
        mui_text_input_set_input_text(app->p_text_input, "");
    }
    mui_text_input_set_event_cb(app->p_text_input, amiibo_scene_account_input_text_cb);
    mui_view_dispatcher_switch_to_view(app->p_view_dispatcher, AMIIBO_VIEW_ID_INPUT);
}

void amiibo_scene_account_input_on_exit(void *user_data) {
    /* 文本输入控件由 mui 框架自己清理. */
    app_amiibo_t *app = user_data;
    /* 兜底清理: 任何路径退出 input 场景都关掉 rename_mode,
     * 避免下次进入 "新建账号" 还被当成 rename. */
    if (app != NULL) {
        app->star_rename_mode = false;
    }
}
