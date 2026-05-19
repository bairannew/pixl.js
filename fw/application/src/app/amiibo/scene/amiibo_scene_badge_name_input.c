/*
 * amiibo_scene_badge_name_input.c   (v8 重写)
 *
 * v7 行为: 把 /star/_inbox/<src> 复制到 /star/<ch>/<acc>/<new>.bin + 删 inbox 源.
 * v8 行为: 直接对 /star/<ch>/<acc>/<src> rename 成 /star/<ch>/<acc>/<new>.bin.
 *
 * 在 v8 里这个场景只有一个入口:
 *   badge_list 长按某个已有徽章 -> 设置 star_wait_rename_mode=true
 *                                  + star_badge_pending = 当前文件名 (含 .bin)
 *                                  + star_badge_autofill = 预填的中文名 (无 .bin)
 *                                  -> next_scene(BADGE_NAME_INPUT)
 *
 * wait_upload 在自动识别命中/未命中时都不会再跳到这里 (新建流程是它自己内部
 * 处理完后直接 back -> badge_list 的). 这样 stack 始终保持简单两层:
 *   [..., badge_list, badge_name_input]
 *
 * 流程:
 *   1) text_input 显示默认中文名 (来自 vfs_meta.notes 或 文件名去 .bin).
 *   2) 用户确认:
 *      a) 计算新 basename (UTF-8 安全 + 净化非法字符 + 长度预算).
 *      b) 与旧 basename 比较: 一样 -> 跳过 rename, 只更新 meta.
 *      c) 不一样 -> 找一个不冲突的目标文件名 (有冲突就加 _N), rename.
 *      d) 更新 vfs_meta.notes = display_name.
 *      e) toast "徽章已添加" -> previous_scene -> badge_list (会重扫目录).
 *   3) 用户取消 -> 啥都不做, previous_scene 回 badge_list.
 *
 * 文件名约束 (与 v7 一致):
 *   - VFS_MAX_PATH_LEN=64, 完整路径 "<dir>/<base>.bin" 必须 < 64
 *   - VFS_MAX_NAME_LEN=48, "<base>.bin" 必须 < 48
 *   - 中文按 UTF-8 边界裁断, 不切到 multi-byte 序列中间
 *   - 非法 ASCII 字符 (/\:?*"<>|) 替换为 '_'
 *   - 空串或全非法 -> 退化为 "badge"
 *
 *  ─ 光遇徽章定制版 (Sky Badge Edition) v8 - badge_name_input ─
 */
#include "amiibo_scene.h"
#include "app_amiibo.h"
#include "mini_app_launcher.h"
#include "mini_app_registry.h"
#include "i18n/language.h"
#include "nrf_log.h"
#include "vfs.h"
#include "vfs_meta.h"
#include "port/star_channels.h"

#include <stdio.h>
#include <string.h>

/* ============================================================ */
/*  UTF-8 / 文件名清理 (与 wait_upload.c 同, 各自一份避免链接耦合) */
/* ============================================================ */

static inline bool is_utf8_lead(unsigned char c) {
    return (c & 0xC0) != 0x80;
}
static size_t utf8_seq_len(unsigned char lead) {
    if ((lead & 0x80) == 0x00) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 0;
}
static size_t utf8_safe_prefix_bytes(const char *src, size_t max_bytes) {
    if (src == NULL || max_bytes == 0) return 0;
    size_t i = 0;
    while (i < max_bytes) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\0') return i;
        size_t n = utf8_seq_len(c);
        if (n == 0) n = 1;
        if (i + n > max_bytes) break;
        for (size_t k = 1; k < n; k++) {
            if (src[i + k] == '\0' || is_utf8_lead((unsigned char)src[i + k])) {
                return i;
            }
        }
        i += n;
    }
    return i;
}
static size_t utf8_copy_bounded(char *dst, size_t dst_cap, const char *src) {
    if (dst == NULL || dst_cap == 0) return 0;
    if (src == NULL) { dst[0] = '\0'; return 0; }
    size_t n = utf8_safe_prefix_bytes(src, dst_cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}
static void sanitize_filename_inplace(char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) {
            switch (c) {
                case '/': case '\\': case ':':
                case '?': case '*':  case '"':
                case '<': case '>':  case '|':
                    *s = '_';
                    break;
                default: break;
            }
        }
    }
}

/* ============================================================ */
/*  路径与 basename 构造                                          */
/* ============================================================ */

static size_t star_dir_strlen(app_amiibo_t *app) {
    const star_channel_t *ch = star_channels_get(app->star_channel_idx);
    const char *chname = ch ? ch->dir_name : "unknown";
    const char *cat = string_get_cstr(app->star_category_dir);
    if (cat == NULL || cat[0] == '\0') cat = "_default";
    return strlen(STAR_ROOT_FOLDER) + 1 +
           strlen(chname) + 1 +
           strlen(string_get_cstr(app->star_account_dir)) + 1 +
           strlen(cat);
}

static size_t star_basename_budget(app_amiibo_t *app, const char *ext) {
    size_t dir_len = star_dir_strlen(app);
    size_t ext_len = ext ? strlen(ext) : 0;
    if (dir_len + 1 + ext_len + 1 >= VFS_MAX_PATH_LEN) return 0;
    size_t by_path = (VFS_MAX_PATH_LEN - 1) - (dir_len + 1) - ext_len;
    if (ext_len + 1 >= VFS_MAX_NAME_LEN) return 0;
    size_t by_name = (VFS_MAX_NAME_LEN - 1) - ext_len;
    return by_path < by_name ? by_path : by_name;
}

/* 构造 "<display>[_N]<ext>" 形态的 basename (含扩展名). */
static bool star_build_basename(app_amiibo_t *app,
                                const char *display_name,
                                int suffix,
                                const char *ext,
                                char *out_base, size_t out_cap) {
    if (out_base == NULL || out_cap < 2) return false;
    char suffix_buf[8] = {0};
    size_t suffix_len = 0;
    if (suffix > 0) {
        snprintf(suffix_buf, sizeof(suffix_buf), "_%d", suffix);
        suffix_len = strlen(suffix_buf);
    }
    size_t ext_len = strlen(ext);

    size_t budget = star_basename_budget(app, ext);
    if (budget == 0 || budget <= suffix_len) return false;
    size_t name_budget = budget - suffix_len;
    if (name_budget + suffix_len + ext_len + 1 > out_cap) {
        if (out_cap <= suffix_len + ext_len + 1) return false;
        name_budget = out_cap - 1 - suffix_len - ext_len;
    }

    size_t n = utf8_safe_prefix_bytes(display_name ? display_name : "", name_budget);
    if (n == 0) {
        const char *fb = "badge";
        size_t fb_n = strlen(fb);
        if (fb_n > name_budget) fb_n = name_budget;
        memcpy(out_base, fb, fb_n);
        out_base[fb_n] = '\0';
    } else {
        memcpy(out_base, display_name, n);
        out_base[n] = '\0';
    }
    sanitize_filename_inplace(out_base);
    if (out_base[0] == '\0') {
        const char *fb = "badge";
        size_t fb_n = strlen(fb);
        if (fb_n > name_budget) fb_n = name_budget;
        memcpy(out_base, fb, fb_n);
        out_base[fb_n] = '\0';
    }
    if (suffix_len > 0) {
        size_t cur = strlen(out_base);
        memcpy(out_base + cur, suffix_buf, suffix_len);
        out_base[cur + suffix_len] = '\0';
    }
    size_t cur = strlen(out_base);
    memcpy(out_base + cur, ext, ext_len);
    out_base[cur + ext_len] = '\0';
    return true;
}

/* 当前徽章所在目录的绝对路径 (账号 + 分类, 不带末尾 '/'). */
static void format_account_path(app_amiibo_t *app, char *out, size_t out_cap) {
    const star_channel_t *ch = star_channels_get(app->star_channel_idx);
    const char *cat = string_get_cstr(app->star_category_dir);
    if (cat == NULL || cat[0] == '\0') cat = "_default";
    snprintf(out, out_cap, "%s/%s/%s/%s",
             STAR_ROOT_FOLDER,
             ch ? ch->dir_name : "unknown",
             string_get_cstr(app->star_account_dir),
             cat);
}

/* ============================================================ */
/*  Rename 核心                                                  */
/* ============================================================ */

/*
 * 把 /star/<ch>/<acc>/<star_badge_pending> rename 成
 *    /star/<ch>/<acc>/<sanitized(display_name)>.bin
 * 并更新 vfs_meta.notes = display_name.
 *
 * 返回:
 *    0  : 成功
 *   -1  : 路径预算耗尽
 *   -2  : 源文件不存在
 *   -3  : 目标位 100 次都被占
 *   -4  : rename 失败
 *   -5  : update_file_meta 失败
 */
static int32_t star_badge_rename(app_amiibo_t *app, const char *display_name) {
    vfs_driver_t *p_drv = vfs_get_driver(app->current_drive);

    char dir[VFS_MAX_PATH_LEN];
    char src_path[VFS_MAX_PATH_LEN];
    format_account_path(app, dir, sizeof(dir));

    /* 源路径 */
    int sn = snprintf(src_path, sizeof(src_path), "%s/%s",
                      dir, string_get_cstr(app->star_badge_pending));
    if (sn < 0 || (size_t)sn >= sizeof(src_path)) return -1;

    vfs_obj_t sobj;
    if (p_drv->stat_file(src_path, &sobj) != VFS_OK) return -2;

    /* 目标 basename (含 .bin), 处理冲突 */
    char tgt_base[VFS_MAX_NAME_LEN];
    char tgt_path[VFS_MAX_PATH_LEN];
    const char *src_basename = string_get_cstr(app->star_badge_pending);

    int suffix = 0;
    bool need_rename = true;
    bool chosen = false;
    while (suffix < 100) {
        if (!star_build_basename(app, display_name, suffix, ".bin",
                                  tgt_base, sizeof(tgt_base))) {
            return -1;
        }
        int tn = snprintf(tgt_path, sizeof(tgt_path), "%s/%s", dir, tgt_base);
        if (tn < 0 || (size_t)tn >= sizeof(tgt_path)) return -1;

        /* 如果新文件名跟现有的相同 -> 不用 rename, 只更新 meta */
        if (strcmp(tgt_base, src_basename) == 0) {
            need_rename = false;
            chosen = true;
            break;
        }
        vfs_obj_t exist;
        if (p_drv->stat_file(tgt_path, &exist) != VFS_OK) {
            chosen = true;
            break;
        }
        suffix++;
    }
    if (!chosen) return -3;

    /* rename (若需要) */
    if (need_rename) {
        int32_t rres = p_drv->rename_file(src_path, tgt_path);
        if (rres != VFS_OK) {
            NRF_LOG_ERROR("badge_rename: rename failed err=%d", rres);
            return -4;
        }
    }

    /* 写 meta.notes */
    vfs_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    /* 保留旧 meta 的其它字段, 如果有 */
    vfs_meta_decode(sobj.meta, sizeof(sobj.meta), &meta);
    meta.has_notes = true;
    /* v8.2-fix9 修复 2 (b): 防御性剥尾 ".bin" 后再写 notes.
     *
     * 用户报告:
     *   "重命名徽章名称后, 名称会叠加加长, 比如原名称叫益, 重名成益豪确认后,
     *    标签的名称会变成益豪.bin益豪.bin"
     *
     * 触发条件: display_name 因为某条预填路径 / 历史脏数据带了 ".bin" 尾巴.
     * 之前 utf8_copy_bounded 直接把它写进 meta.notes, 然后:
     *   - star_build_basename 在 display_name 末尾再追加 ".bin",
     *     文件名变成 "<name>.bin.bin"
     *   - notes 也存成 "<name>.bin"
     *   - badge_list 渲染 text(filename) + sub_text(notes) 同一行
     *     -> 用户看到 "<name>.bin<name>.bin" 这种叠加变长的怪相
     *
     * fix9 修复 2(a) 在 badge_list 长按预填时统一剥过一次, 这里在 rename
     * 入口再做一次防御性剥尾. 两层兜底, 任何路径 (老存储 / 用户错按 /
     * 别的 scene 跳来) 都不会让 ".bin" 渗进 notes.
     *
     * 注意: 这里只动 notes 的内容, **不动磁盘文件名**. 磁盘文件名由
     * 上方 star_build_basename(display_name, ..., ".bin", ...) 单独构造,
     * 它内部会做 utf8 安全切片 + sanitize + 加扩展名, 哪怕 display_name
     * 带 ".bin", 出来的也只是文件名里多一段冗余文字而非两层 ".bin". */
    {
        const char *notes_src = display_name;
        char notes_clean[VFS_META_MAX_NOTES_SIZE];
        size_t dn_len = strlen(display_name);
        if (dn_len > 4 && strcmp(display_name + dn_len - 4, ".bin") == 0) {
            size_t cp = dn_len - 4;
            if (cp >= sizeof(notes_clean)) cp = sizeof(notes_clean) - 1;
            memcpy(notes_clean, display_name, cp);
            notes_clean[cp] = '\0';
            notes_src = notes_clean;
        }
        utf8_copy_bounded(meta.notes, sizeof(meta.notes), notes_src);
    }

    uint8_t meta_buf[VFS_MAX_META_LEN];
    memset(meta_buf, 0, sizeof(meta_buf));
    vfs_meta_encode(meta_buf, sizeof(meta_buf), &meta);
    int32_t mres = p_drv->update_file_meta(tgt_path, meta_buf, sizeof(meta_buf));
    if (mres != VFS_OK) {
        NRF_LOG_ERROR("badge_rename: update_file_meta failed err=%d", mres);
        /* rename 已经成功, meta 失败不致命 — 退化为没有 notes 副标题 */
        return -5;
    }

    NRF_LOG_INFO("badge_rename ok: %s -> %s",
                 nrf_log_push(src_path), nrf_log_push(tgt_path));
    return 0;
}

/* ============================================================ */
/*  场景回调                                                     */
/* ============================================================ */

static void amiibo_scene_badge_name_input_text_cb(mui_text_input_event_t event,
                                                   mui_text_input_t *p_text_input) {
    app_amiibo_t *app = p_text_input->user_data;

    /* v8.2-fix5: BADGE_NAME_INPUT 现在只从 ACTION_MENU 跳进来 (badge_list
     * 长按徽章 -> action_menu -> 选 "重命名" -> 这里). 所以无论成功 / 失败 /
     * 用户取消, 都得连 action_menu 一并 pop 掉, 用 back_scene(2) 直接回到
     * badge_list. 否则用户改完名又看见 action_menu 那一层, 体验突兀. */
    if (event != MUI_TEXT_INPUT_EVENT_CONFIRMED) {
        /* 取消: 啥都不做, 回 badge_list */
        mui_scene_dispatcher_back_scene(app->p_scene_dispatcher, 2);
        return;
    }

    const char *display_name = mui_text_input_get_input_text(p_text_input);
    if (display_name == NULL || display_name[0] == '\0') {
        display_name = "badge";
    }

    int32_t err = star_badge_rename(app, display_name);
    if (err != 0 && err != -5) {
        /* -5 = rename 成功但 meta 写入失败, 仍视为整体成功. */
        NRF_LOG_WARNING("star_badge_rename failed err=%d", err);
        mui_toast_view_show(app->p_toast_view,
                            getLangString(_L_STAR_BADGE_SAVE_FAILED));
        mui_scene_dispatcher_back_scene(app->p_scene_dispatcher, 2);
        return;
    }

    mui_toast_view_show(app->p_toast_view, getLangString(_L_STAR_BADGE_SAVED));
    mui_scene_dispatcher_back_scene(app->p_scene_dispatcher, 2);
}

void amiibo_scene_badge_name_input_on_enter(void *user_data) {
    app_amiibo_t *app = user_data;

    mui_text_input_set_header(app->p_text_input,
                              getLangString(_L_STAR_INPUT_BADGE_NAME));
    mui_text_input_set_input_text(app->p_text_input,
                                  string_get_cstr(app->star_badge_autofill));
    mui_text_input_set_event_cb(app->p_text_input,
                                amiibo_scene_badge_name_input_text_cb);
    mui_view_dispatcher_switch_to_view(app->p_view_dispatcher, AMIIBO_VIEW_ID_INPUT);
}

void amiibo_scene_badge_name_input_on_exit(void *user_data) {
    (void)user_data;
}
