#include "amiibo_helper.h"
#include "amiibo_scene.h"
#include "app_amiibo.h"
#include "app_timer.h"
#include "cwalk2.h"
#include "db_header.h"
#include "i18n/language.h"
#include "mui_core.h"
#include "mui_list_view.h"
#include "nrf_log.h"
#include "ntag_emu.h"
#include "ntag_store.h"
#include "settings.h"
#include "vfs.h"
#include "vfs_meta.h"

/* v8.1 fix: NFC 写入 (手机贴卡) 命中后, 优先用 sky 数据库识别光遇徽章并改名;
 * 没命中再回落到原版 amiibo 数据库. BLE 上传路径在 badge_list.c 里已经接入了
 * 同一份解析, 这里补的是 ntag_update_cb -> ntag_update 这条 NFC 通路.
 *
 * 用户反馈: "新建徽章 新建了一个空标签 然后被写入BLE不会判断解析解密base64
 * 获取sk进行自动重命名bin啊" — 实测下来 BLE close hook 是接好的,
 * 但用户也会把手机的光遇徽章直接贴到 Pixl.js 上让设备模拟卡被写入,
 * 这条路径原来只查 amiibo 表, 光遇徽章查不到 -> 永远不改名.
 */
#include "sky_badge_parser.h"
#include "sky_badge_db.h"
/* v8.1-fix2: 按账号自动打开应用 —— 在仿真 tag 之前, 给 NDEF 注入 AAR */
#include "sky_aar_inject.h"
#include "star_channels.h"
/* v8.1-fix2 RAM 修: detail_emit_tag 在 TLSF 堆上借 ntag_t scratch 缓冲,
 * 避免 BSS 把 RAM 区撑爆触发 `region RAM overflowed with stack`. */
#include "mui_mem.h"
#include <string.h>

#define NRF_ERR_NOT_AMIIBO -1000
#define NRF_ERR_READ_ERROR -1001

APP_TIMER_DEF(m_amiibo_gen_delay_timer);

static void amiibo_scene_amiibo_detail_reload_files(app_amiibo_t *app);
/* v8.1-fix2: 前向声明, 让 ntag_gen / reload_ntag (在文件前部) 也能调用. */
static void detail_emit_tag(app_amiibo_t *app, ntag_t *clean_ntag);
/* v8.1-fix3: 前向声明 reload_ntag —— ntag_update_cb 在第 309/386 行就要调它,
 * 而真正的定义在第 416 行, 不加这一行 gcc 会按隐式 int() 推, 然后和 bool 定义
 * 冲突 -> "conflicting types for 'amiibo_scene_amiibo_detail_reload_ntag'". */
static bool amiibo_scene_amiibo_detail_reload_ntag(app_amiibo_t *app, const char *file_name);

static void amiibo_scene_amiibo_detail_msg_box_error_cb(mui_msg_box_event_t event, mui_msg_box_t *p_msg_box) {
    app_amiibo_t *app = p_msg_box->user_data;
    mui_scene_dispatcher_previous_scene(app->p_scene_dispatcher); /* v4: back to whatever pushed us (BADGE_LIST in new flow) */
}

static void amiibo_scene_amiibo_detail_reload_error(app_amiibo_t *app, const char *path, int32_t err_code) {

    char msg[64];
    strcpy(msg, path);
    strcat(msg, "\n");
    if (err_code == NRF_ERR_NOT_AMIIBO) {
        strcat(msg, getLangString(_L_NOT_AMIIBO_FILE));
    } else if (err_code == NRF_ERR_READ_ERROR) {
        strcat(msg, getLangString(_L_READ_FILE_FAILED));
    } else {
        strcat(msg, getLangString(_L_READ_FILE_FAILED));
    }

    mui_msg_box_set_header(app->p_msg_box, getLangString(_L_ERR));
    mui_msg_box_set_message(app->p_msg_box, msg);
    mui_msg_box_set_btn_text(app->p_msg_box, NULL, getLangString(_L_BACK), NULL);
    mui_msg_box_set_btn_focus(app->p_msg_box, 1);
    mui_msg_box_set_event_cb(app->p_msg_box, amiibo_scene_amiibo_detail_msg_box_error_cb);

    mui_view_dispatcher_switch_to_view(app->p_view_dispatcher, AMIIBO_VIEW_ID_MSG_BOX);
}

static int32_t ntag_read(vfs_driver_t *p_vfs_driver, const char *path, ntag_t *ntag) {

    vfs_obj_t obj;
    int32_t res;

    memset(ntag, 0, sizeof(ntag_t));
    res = p_vfs_driver->stat_file(path, &obj);
    if (res != VFS_OK) {
        return NRF_ERR_READ_ERROR;
    }

    if (obj.size != 540 && obj.size != 532 && obj.size != 572 && obj.size != 2048) {
        return NRF_ERR_NOT_AMIIBO;
    }

    uint8_t meta_size = obj.meta[0];
    if (meta_size > 0 && meta_size < 0xFF) {
        memcpy(ntag->notes, obj.meta + 3, meta_size - 2);
    }

    vfs_meta_t meta;
    memset(&meta, 0, sizeof(vfs_meta_t));
    vfs_meta_decode(obj.meta, sizeof(obj.meta), &meta);
    if (meta.has_notes) {
        memcpy(ntag->notes, meta.notes, strlen(meta.notes));
    }

    NRF_LOG_INFO("has_flag:%d flag:%d", meta.has_flags, meta.flags);
    if (meta.has_flags && (meta.flags & VFS_OBJ_FLAG_READONLY)) {
        ntag->read_only = true;
    }

    ntag_type_t tag_type = _ntag_type(obj.size);
    ntag->type = tag_type;

    res = p_vfs_driver->read_file_data(path, ntag->data, _ntag_data_size(ntag));
    if (res != 540 && res != 532 && res != 2048) {
        return NRF_ERR_READ_ERROR;
    }
    return NRF_SUCCESS;
}

static void ntag_gen(void *p_context) {
    ret_code_t err_code;
    app_amiibo_t *app = p_context;
    ntag_t *ntag_current = &app->ntag;

    err_code = amiibo_helper_rand_amiibo_uuid(ntag_current);
    if (err_code == NRF_SUCCESS) {
        /* v8.1-fix2: 同样走带 AAR 的仿真入口 */
        detail_emit_tag(app, &app->ntag);
        mui_update(mui());
    }
}

/* v8.1 helpers: 文件名清理 (拒收会让 vfs rename 失败的字符).
 * NTAG 写入触发改名时, 直接拿 sky_badge_entry_t.name_zh 当 basename,
 * 中文本身不会触发 FATFS 拒收, 但万一表里某条意外包含了 / : * ? " < > |
 * (历史数据迁移漏改), 这里也清掉. 与 badge_list.c 的 sanitize 同源. */
static void detail_sanitize_filename_inplace(char *s) {
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

/* v8.1: 尝试把刚写入的 ntag dump 当作光遇徽章解析, 命中则改名 + 写 meta.notes.
 * 返回 true = 命中并改名成功 (调用方应跳过 amiibo 兜底);
 *      false = 未命中或改名失败 (调用方按原逻辑回落 amiibo). */
static bool ntag_update_try_sky_rename(app_amiibo_t *app, ntag_t *p_ntag,
                                       const char *old_path) {
    /* 仅对占位/新建文件做自动改名, 避免误改用户手动命名的徽章 */
    const char *cur_basename = string_get_cstr(app->current_file);
    if (cur_basename == NULL || strncmp(cur_basename, "new", 3) != 0) {
        return false;
    }

    /* 一站式解析: NTAG dump -> NDEF URI -> ?s=BASE64 -> sk -> 数据库条目 */
    const sky_badge_entry_t *e =
        sky_badge_try_autofill(p_ntag->data, _ntag_data_size(p_ntag));
    if (e == NULL) {
        NRF_LOG_INFO("ntag_update: sky autofill miss, fallback to amiibo db");
        return false;
    }

    vfs_driver_t *p_driver = vfs_get_driver(app->current_drive);
    amiibo_detail_view_t *p_view = app->p_amiibo_detail_view;
    const char *folder = string_get_cstr(app->current_folder);

    /* v8.1-fix2: 目标 basename 把"中文名 + 中文备注"都拼进文件名里:
     *
     *     <中文名>（<中文备注>）.bin     - 有备注
     *     <中文名>.bin                    - 没备注
     *
     * 旧版本 (v8.1) 文件名只是 "<中文名>.bin", 而把
     * "<中文名> (<中文备注>)" 写到 vfs_meta.notes 当副标题。
     * 用户反馈说在 badge_list 里看到 "标题<中文名>.bin / 副标题<中文名>(中文备注)",
     * 中文名出现了两次, 太长难看。
     *
     * 现在改成: 文件名一次性给全, vfs_meta.notes 留空 (或只放 note),
     * 避免名字重复。冲突时仍然加 _N 后缀, 上限 99。
     *
     * 用全角括号 （） 避开 FATFS 文件系统对部分 ASCII 标点的限制, 同时
     * 与中文徽章备注风格保持一致 (备注内容里本来就没有半角括号需求)。
     */
    char new_path[VFS_MAX_PATH_LEN];
    char new_name[VFS_MAX_NAME_LEN];
    const bool has_note = (e->note_zh && e->note_zh[0]);
    for (int suffix = 0; suffix < 100; suffix++) {
        int n;
        if (suffix == 0) {
            if (has_note) {
                n = snprintf(new_name, sizeof(new_name), "%s（%s）.bin",
                             e->name_zh, e->note_zh);
            } else {
                n = snprintf(new_name, sizeof(new_name), "%s.bin", e->name_zh);
            }
        } else {
            if (has_note) {
                n = snprintf(new_name, sizeof(new_name), "%s（%s）_%d.bin",
                             e->name_zh, e->note_zh, suffix);
            } else {
                n = snprintf(new_name, sizeof(new_name), "%s_%d.bin",
                             e->name_zh, suffix);
            }
        }
        if (n < 0 || (size_t)n >= sizeof(new_name)) {
            /* 拼完了发现太长 —— 退回 "只用 name_zh" 再试一次,
             * 让长备注的徽章 (如新春礼包) 至少能拿到正确的中文名。 */
            if (has_note) {
                if (suffix == 0) {
                    n = snprintf(new_name, sizeof(new_name), "%s.bin",
                                 e->name_zh);
                } else {
                    n = snprintf(new_name, sizeof(new_name), "%s_%d.bin",
                                 e->name_zh, suffix);
                }
                if (n < 0 || (size_t)n >= sizeof(new_name)) {
                    NRF_LOG_WARNING("ntag_update: sky name too long, giving up");
                    return false;
                }
            } else {
                NRF_LOG_WARNING("ntag_update: sky name too long, giving up");
                return false;
            }
        }
        detail_sanitize_filename_inplace(new_name);

        cwalk_append_segment(new_path, folder, new_name);

        /* 这把命名是不是已经被自己占了 (BLE 抢先改名 + NFC 又写一次的极端情况) */
        if (strcmp(new_path, old_path) == 0) {
            return true;   /* 名字就是它, 不用 rename, 直接当成功 */
        }

        vfs_obj_t existing;
        if (p_driver->stat_file(new_path, &existing) != VFS_OK) {
            /* 目标名不存在, rename 上去 */
            int32_t r = p_driver->rename_file(old_path, new_path);
            if (r != VFS_OK) {
                NRF_LOG_ERROR("ntag_update: rename %s -> %s failed (%d)",
                              nrf_log_push((char *)old_path),
                              nrf_log_push(new_path), (int)r);
                return false;
            }
            break;
        }
        /* 撞名, 继续下一个 suffix */
    }

    /* v8.1-fix2: notes 副标题置空 —— 因为文件名里已经有了中文名 (+备注),
     * 没必要再在副标题里复述一遍。p_ntag->notes 也保持空, detail
     * 视图标题就用 new_name 自身, 不会再出现 "new.bin" 占位。 */
    vfs_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.has_notes = false;
    uint8_t meta_buf[VFS_MAX_META_LEN];
    memset(meta_buf, 0, sizeof(meta_buf));
    vfs_meta_encode(meta_buf, sizeof(meta_buf), &meta);
    (void)p_driver->update_file_meta(new_path, meta_buf, sizeof(meta_buf));

    memset(p_ntag->notes, 0, sizeof(p_ntag->notes));

    /* 同步 app 状态 + 视图: 显示改名后的徽章 */
    string_set_str(app->current_file, new_name);
    amiibo_detail_view_set_file_name(p_view, new_name);
    amiibo_scene_amiibo_detail_reload_files(app);

    NRF_LOG_INFO("ntag_update: sky autofill hit sk=%s -> %s",
                 nrf_log_push((char *)e->sk),
                 nrf_log_push(new_name));

    /* v8.2-fix6: 删掉了 v8.1-fix3 的 "改完名再自动建一个新 new.bin
     * 并把视图切到这个空 bin" 的连续感应逻辑.
     *
     * 用户反馈:
     *   "新建徽章进入到默认新建的bin后, 感应写入的标签后创建成功了,
     *    但是不会为这个进去的bin重命名, 而是重新创建了一个名称.
     *    导致创建了, 但是还在在这个空白的bin里面."
     *
     * 旧逻辑磁盘上是对的 (placeholder 已 rename 成 <中文名>.bin), 但视图
     * 被刷成了刚新建的 new.bin —— 用户看到的是"自己进的这个 bin 还空着,
     * 另外多了个有名字的 bin". 跟用户对"我现在就在这个 bin 里, 数据应该
     * 落到这个 bin"的心智模型不符.
     *
     * 现在: 感应完就停在改完名的徽章详情里. 想再刷下一张, 用户自己回
     * badge_list 点 "新建徽章" 走一遍 start_new_badge_flow —— 干净, 可预期. */

    return true;
}

static void ntag_update(app_amiibo_t *app, ntag_t *p_ntag) {
    amiibo_detail_view_t *p_amiibo_detail_view = app->p_amiibo_detail_view;
    memcpy(p_amiibo_detail_view->ntag, p_ntag, sizeof(ntag_t));

    vfs_driver_t *p_driver = vfs_get_driver(app->current_drive);

    char path[VFS_MAX_PATH_LEN];
    cwalk_append_segment(path, string_get_cstr(app->current_folder), string_get_cstr(app->current_file));

    // save to fs
    int32_t res = p_driver->write_file_data(path, p_ntag->data, _ntag_data_size(p_ntag));
    if (res > 0) {
        /* v8.1 fix: 先尝试光遇徽章识别 (sky_badge_parser + sky_badge_db).
         * 命中 => 已经改名 + 写 meta.
         * 没命中 => 回落到原版 amiibo 数据库查表 (Nintendo 手办兼容).
         *
         * v8.2-fix7: 把原来的"if(sky_rename) { mui_update; return; }"早返回拆
         * 成 if / else, 让无论走哪条识别分支, 最后都会走到下面统一的 AAR
         * 重塞 + mui_update —— 因为不管命中 sky 还是命中 amiibo db, 手机刚
         * 写进来的裸 URL 都已经覆盖了 ntag_emu 里之前 detail_emit_tag 注入
         * 过的 AAR, 必须再塞一次, 否则下次贴卡读取就拿不到 AAR. */
        if (!ntag_update_try_sky_rename(app, p_ntag, path)) {
            uint32_t head = to_little_endian_int32(&p_ntag->data[84]);
            uint32_t tail = to_little_endian_int32(&p_ntag->data[88]);

            const db_amiibo_t *amd = get_amiibo_by_id(head, tail);

            if (amd && strncmp(string_get_cstr(app->current_file), "new", 3) == 0) {
                char new_path[VFS_MAX_PATH_LEN];
                char new_name[VFS_MAX_NAME_LEN];
                snprintf(new_name, sizeof(new_name), "%s.bin", amd->name_en);
                cwalk_append_segment(new_path, string_get_cstr(app->current_folder), new_name);
                res = p_driver->rename_file(path, new_path);

                if (res == VFS_OK) {
                    string_set_str(app->current_file, new_name);
                    amiibo_detail_view_set_file_name(p_amiibo_detail_view, new_name);
                    amiibo_scene_amiibo_detail_reload_files(app);

                    /* v8.2-fix6: 与 sky 路径同步删掉自动新建占位的连续感应逻辑.
                     * 见 ntag_update_try_sky_rename 末尾的注释. 用户期望:
                     * 感应完就停在改完名的徽章详情里, 不要自己跳到新 new.bin. */
                }
            }
        }
    }

    /* ============================================================ */
    /*  v8.2-fix7: NFC 写卡完成后, 把 AAR 重新注入到模拟器内的 NTAG.   */
    /*                                                                */
    /*  背景: ntag_emu 的写入流程是                                   */
    /*    手机贴卡写 → HAL_NFC_EVENT_FIELD_OFF (dirty=1)              */
    /*           → update_ntag_handler → ntag_emu_set_tag(&self)     */
    /*           → update_cb → ntag_update (本函数)                  */
    /*  其中 ntag_emu_set_tag(&self) 是自拷贝, 拷的就是"手机刚写下的    */
    /*  裸 URL", 之前 reload_ntag / emit_tag 注入过的 AAR 直接被覆盖.   */
    /*                                                                */
    /*  结果: 用户的使用闭环                                            */
    /*    a. 进徽章 → reload_ntag 注入 AAR → ntag_emu 有 AAR.          */
    /*    b. 手机贴卡读 → 拿到 AAR → 直接拉起对应渠道 Sky 客户端. ✓     */
    /*    c. 手机贴卡写新 URL → ntag_emu 现在只有裸 URL, 没 AAR.       */
    /*    d. 手机再贴卡读 → 没 AAR → 系统弹"打开方式". ✗                */
    /*                                                                */
    /*  修法: 这里走一遍 detail_emit_tag —— 它会按 app->current_folder  */
    /*  解析当前渠道 (/star/<channel>/<account>/...), 查 star_channels  */
    /*  表拿 pkg_name (含本版本新加的 KUAISHOU = com.netease.sky.       */
    /*  kuaishou), 在副本上注入 AAR 后再 ntag_emu_set_tag.              */
    /*                                                                */
    /*  注意: 只动 ntag_emu 内存里的副本, 磁盘上的 .bin 文件还是裸数据 */
    /*  (上面 write_file_data 写下去的是 p_ntag->data, 不含 AAR), 跟    */
    /*  v8.1-fix2 "保持磁盘干净" 的策略一致, 不污染备份, 用户的 BLE     */
    /*  下载 / 备份还是能拿到没 AAR 的原始 dump.                        */
    /*                                                                */
    /*  非光遇徽章 (例 amiibo 手办在非 /star 路径下) 走不到 star 渠道,  */
    /*  detail_build_aar_ntag 返回 NULL, detail_emit_tag 自动回退到无   */
    /*  AAR 仿真, 行为跟没改之前一致, 不破坏 Nintendo 兼容性.           */
    /* ============================================================ */
    detail_emit_tag(app, p_ntag);

    mui_update(mui());
}

static void ntag_update_cb(ntag_event_type_t type, void *context, ntag_t *p_ntag) {

    app_amiibo_t *app = context;

    if (type == NTAG_EVENT_TYPE_WRITTEN) {
        ntag_update(app, p_ntag);
    } else if (type == NTAG_EVENT_TYPE_READ) {
        settings_data_t *p_settings = settings_get_data();
        if (p_settings->auto_gen_amiibo) {
            app_timer_stop(m_amiibo_gen_delay_timer);
            app_timer_start(m_amiibo_gen_delay_timer, APP_TIMER_TICKS(1000), app);
        }
    }
}

static bool amiibo_scene_amiibo_detail_reload_ntag(app_amiibo_t *app, const char *file_name) {
    char path[VFS_MAX_PATH_LEN];

    if (strlen(file_name) == 0) {
        return false;
    }

    cwalk_append_segment(path, string_get_cstr(app->current_folder), file_name);

    vfs_driver_t *p_vfs_driver = vfs_get_driver(app->current_drive);
    int32_t err = ntag_read(p_vfs_driver, path, &app->ntag);
    if (err != NRF_SUCCESS) {
        amiibo_scene_amiibo_detail_reload_error(app, file_name, err);
        string_set_str(app->current_file, "");
        return false;
    }
    string_set_str(app->current_file, file_name);
    amiibo_detail_view_set_file_name(app->p_amiibo_detail_view, file_name);
    amiibo_detail_view_set_ntag(app->p_amiibo_detail_view, &app->ntag);
    /* v8.1-fix2: 仿真前给副本注入 AAR (不动 app->ntag, 保持磁盘干净). */
    detail_emit_tag(app, app->p_amiibo_detail_view->ntag);

    return true;
}

static int amiibo_scene_amiibo_detail_list_item_cmp(const string_t *a, const string_t *b) { return string_cmp(*a, *b); }

/* v8.1-fix2 (RAM 修): 仿真侧专用 NTAG 缓冲, 跟磁盘 / app->ntag 解耦.
 *
 * 设计要点:
 *   - AAR 不能 inject 到 app->ntag 里, 否则用户后续做"设 UID / 随机 UID / 只读"
 *     等编辑操作时, write_file_data() 会把"已经塞了 AAR 的 ntag"写回磁盘,
 *     污染原始备份 (再次读取还会被解析模块当作多个 record, 再 inject 一次)。
 *   - 因此先 memcpy 到独立缓冲, 在副本上 inject, 再 ntag_emu_set_tag.
 *   - ntag_emu_set_tag 本身已经 memcpy 一份到自己的内部缓冲 (见 ntag_emu_v2.c
 *     第 305 行), 所以本缓冲生命周期只需要覆盖到 ntag_emu_set_tag 返回为止,
 *     不存在悬挂指针.
 *
 *   缓冲位置:
 *     初版 (fix2 v1) 用 `static ntag_t s_emu_aar_ntag;` 放 BSS, 结果在
 *     `ld: region RAM overflowed with stack` 上挂掉 —— ntag_t = 2184 字节,
 *     RAM 区 (0xCA00 = 51.7 KB) 减去 18 KB TLSF 堆和其它 BSS 后, 实在
 *     腾不出这 2.2 KB 留栈, 链接失败.
 *
 *     现版: 从 TLSF 堆 (mui_mem_*) 临时分配, 一次性使用, 用完立即 free.
 *     堆有 18 KB, 2.2 KB 占用 ~12%, 而且仅在 detail_emit_tag 调用期间存在,
 *     正常不会跟其它路径并发抢. 万一堆爆 (malloc 返回 NULL), 我们直接退
 *     回到 clean_ntag (无 AAR) 仿真 —— 体验跟非光遇徽章一致, 不至于无响应.
 *
 *     ⚠ 不放栈: ntag_t ~2.2 KB, 走栈会让 reload_ntag / button handler 栈帧
 *     直接超过 RTOS task stack (典型 2 KB), 触发 hard fault.
 */

/* 把仿真出去的 NTAG 内存里塞一条 Android Application Record (AAR),
 * 这样手机贴卡时就能根据徽章所属账号的渠道, 直接拉起对应的 Sky 光遇客户端,
 * 不再让系统弹"打开方式"让用户挑.
 *
 * 渠道识别: 从 app->current_folder 解析. 路径形如:
 *
 *     E:/star/<channel>/<account>
 *      ^^^^^^ STAR_ROOT_FOLDER
 *             ^^^^^^^^^ 我们要的渠道短码
 *
 * 没识别出来 / 不是 /star 下面的文件 → 返回 NULL, 调用方应当退回用原 ntag 仿真.
 * 这种兼容回退确保非光遇徽章 (amiibo 手办等) 完全不受影响.
 *
 * @param app    场景上下文 (用其 current_folder 解析渠道).
 * @param src    源 NTAG (不修改).
 * @param dst    调用方提供的目标缓冲 (mui_mem_malloc 出来的, 由调用方负责 free).
 * @return       成功注入返回 dst; 不需注入或注入失败返回 NULL.
 */
static ntag_t *detail_build_aar_ntag(app_amiibo_t *app, const ntag_t *src, ntag_t *dst) {
    if (app == NULL || src == NULL || dst == NULL) return NULL;
    const char *folder = string_get_cstr(app->current_folder);
    if (folder == NULL || folder[0] == '\0') return NULL;

    /* 找到 "/star/" 前缀. 可能是 "E:/star/..." 或 "/star/..." 形式. */
    const char *p = strstr(folder, STAR_ROOT_FOLDER "/");
    if (p == NULL) return NULL;
    p += strlen(STAR_ROOT_FOLDER "/");
    if (*p == '\0') return NULL;

    /* 截取一段直到下一个 '/' 当作渠道短码. */
    char ch_dir[16];
    size_t i = 0;
    while (i < sizeof(ch_dir) - 1 && p[i] != '\0' && p[i] != '/') {
        ch_dir[i] = p[i];
        i++;
    }
    ch_dir[i] = '\0';
    if (i == 0) return NULL;

    /* 查表 */
    const star_channel_t *ch = star_channels_find_by_dir(ch_dir);
    if (ch == NULL || ch->pkg_name == NULL || ch->pkg_name[0] == '\0') {
        NRF_LOG_DEBUG("detail_build_aar_ntag: channel '%s' has no pkg_name, skip",
                      nrf_log_push(ch_dir));
        return NULL;
    }

    /* 拷贝原始 ntag 到调用方缓冲, 在副本上 inject AAR */
    memcpy(dst, src, sizeof(*dst));
    bool ok = sky_aar_inject_into_ntag(dst, ch->pkg_name);
    if (!ok) {
        /* AAR inject 失败 (容量 / 解析): 返回 NULL, 由调用方退回原 ntag.
         * URL 仍能让手机弹"打开方式", 体验比无响应要好. */
        NRF_LOG_DEBUG("detail_build_aar_ntag: inject failed for pkg=%s",
                      nrf_log_push((char *)ch->pkg_name));
        return NULL;
    }
    return dst;
}

/* 统一仿真入口: 优先用 AAR 注入版本, 注入失败 / 内存不够 回退到原 ntag.
 *
 * 内存策略: 副本缓冲从 TLSF 堆 (mui_mem_malloc) 借, 用完立刻还.
 * - 不放 BSS: 长期占 2.2 KB, 会让 RAM 区放不下 stack (fix2 初版就是这么挂的).
 * - 不放栈:  ntag_t 2.2 KB, 走栈会爆 task stack.
 * - 堆借不到: 直接用 clean_ntag, 行为退化成无 AAR 仿真, 兼容老版.
 */
static void detail_emit_tag(app_amiibo_t *app, ntag_t *clean_ntag) {
    ntag_t *scratch = (ntag_t *)mui_mem_malloc(sizeof(ntag_t));
    if (scratch == NULL) {
        NRF_LOG_WARNING("detail_emit_tag: mui_mem_malloc(%u) failed, fallback to no-AAR emu",
                        (unsigned)sizeof(ntag_t));
        ntag_emu_set_tag(clean_ntag);
        return;
    }
    ntag_t *with_aar = detail_build_aar_ntag(app, clean_ntag, scratch);
    ntag_emu_set_tag(with_aar != NULL ? with_aar : clean_ntag);
    /* ntag_emu_set_tag 已 memcpy 到内部缓冲, 这里释放安全 */
    mui_mem_free(scratch);
}

/* v8.1-fix2 公开入口, 让 amiibo_scene_amiibo_detail_menu.c 也能走带 AAR 仿真.
 * 见 amiibo_scene.h 里的说明. */
void amiibo_scene_amiibo_detail_emit_tag(app_amiibo_t *app, ntag_t *clean_ntag) {
    detail_emit_tag(app, clean_ntag);
}

/* v8.2: 限制单次 reload 加载的徽章数, 避免账号 / 分类目录下有上百个 .bin
 * 时把 TLSF 堆 (~18KB) 撑爆 -> "点进去徽章直接闪退回主页" 的 hard fault.
 *
 * 测算:
 *   - 每个 string_array 条目 = string_t 头 + 文件名缓冲 (最长 ~50 字节)
 *     一共大概 70 字节.
 *   - amiibo_detail_view 用 uint8_t focus / max_ntags, 物理上 cap=255.
 *
 * 取 100 作为软上限, 既留给堆其它需求 (mui_list_view, vfs scratch 等),
 * 又给 max_ntags 留足 uint8_t 空间, 避免 256+ 时回绕到 0
 * (256 截断成 0 -> "max_ntags - 1" = 255 下溢 -> 焦点漂越界 -> 访问越界数组 -> 闪退). */
#define DETAIL_RELOAD_MAX_FILES   100

static void amiibo_scene_amiibo_detail_reload_files(app_amiibo_t *app) {
    vfs_dir_t dir;
    vfs_obj_t obj;
    string_t file_name;
    uint32_t focus = 0;
    uint32_t loaded = 0;

    // query amiibo list
    string_init(file_name);
    string_array_reset(app->amiibo_files);
    vfs_driver_t *p_vfs_driver = vfs_get_driver(app->current_drive);

    int32_t res = p_vfs_driver->open_dir(string_get_cstr(app->current_folder), &dir);
    if (res == VFS_OK) {
        while ((res = p_vfs_driver->read_dir(&dir, &obj)) == VFS_OK) {
            if (loaded >= DETAIL_RELOAD_MAX_FILES) {
                NRF_LOG_WARNING("detail reload: capped at %d files, more ignored",
                                DETAIL_RELOAD_MAX_FILES);
                break;
            }
            vfs_meta_t meta;
            memset(&meta, 0, sizeof(vfs_meta_t));
            vfs_meta_decode(obj.meta, sizeof(obj.meta), &meta);
            if (obj.type == VFS_TYPE_REG && is_valid_amiibo_ntag_by_size(obj.size)
                 && (!meta.has_flags || !(meta.flags & VFS_OBJ_FLAG_HIDDEN))) {
                string_set_str(file_name, obj.name);
                string_array_push_back(app->amiibo_files, file_name);
                loaded++;
            }
        }
        p_vfs_driver->close_dir(&dir);
    }
    /* 临时 string_t 必须 clear, 否则它持有的堆缓冲泄漏 */
    string_clear(file_name);

    string_array_special_sort(app->amiibo_files, amiibo_scene_amiibo_detail_list_item_cmp);

    // load amiibo detail
    string_array_it_t it;
    string_array_it(it, app->amiibo_files);
    while (!string_array_end_p(it)) {
        string_t *item = string_array_ref(it);
        if (string_cmp(*item, app->current_file) == 0) {
            amiibo_detail_view_set_focus(app->p_amiibo_detail_view, focus);
            break;
        }
        focus++;
        string_array_next(it);
    }

    if (focus >= string_array_size(app->amiibo_files)) {
        amiibo_detail_view_set_focus(app->p_amiibo_detail_view, 0);
    }

    /* v8.2: 强制 clamp 到 uint8_t 安全范围 (软上限 100 -> 不会撞到 255,
     * 但既然 set_max_ntags 接受 uint8_t, 形式上也补一次防御性 cast). */
    size_t cnt = string_array_size(app->amiibo_files);
    if (cnt > 255) cnt = 255;
    amiibo_detail_view_set_max_ntags(app->p_amiibo_detail_view, (uint8_t)cnt);
}

static void app_amiibo_detail_view_on_event(amiibo_detail_view_event_t event, amiibo_detail_view_t *p_view) {
    app_amiibo_t *app = p_view->user_data;
    if (event == AMIIBO_DETAIL_VIEW_EVENT_MENU) {
        mui_scene_dispatcher_next_scene(app->p_scene_dispatcher, AMIIBO_SCENE_AMIIBO_DETAIL_MENU);
    } else if (event == AMIIBO_DETAIL_VIEW_EVENT_UPDATE) {
        uint8_t focus = amiibo_detail_view_get_focus(app->p_amiibo_detail_view);
        amiibo_scene_amiibo_detail_reload_ntag(app, string_get_cstr(*string_array_get(app->amiibo_files, focus)));
    }
}

void amiibo_scene_amiibo_detail_on_enter(void *user_data) {
    app_amiibo_t *app = user_data;
    app->p_amiibo_detail_view->ntag = &app->ntag;
    app->p_amiibo_detail_view->event_cb = app_amiibo_detail_view_on_event;
    amiibo_detail_view_set_event_cb(app->p_amiibo_detail_view, app_amiibo_detail_view_on_event);

    if (app->reload_amiibo_files) {
        if (!amiibo_scene_amiibo_detail_reload_ntag(app, string_get_cstr(app->current_file))) {
            return;
        }
        amiibo_scene_amiibo_detail_reload_files(app);
        app->reload_amiibo_files = false;
    }

    ntag_emu_set_update_cb(ntag_update_cb, app);

    mui_view_dispatcher_switch_to_view(app->p_view_dispatcher, AMIIBO_VIEW_ID_DETAIL);

    int32_t err_code = app_timer_create(&m_amiibo_gen_delay_timer, APP_TIMER_MODE_SINGLE_SHOT, ntag_gen);
    APP_ERROR_CHECK(err_code);
}

void amiibo_scene_amiibo_detail_on_exit(void *user_data) {
    app_amiibo_t *app = user_data;
    ntag_emu_set_update_cb(NULL, NULL);
    app_timer_stop(m_amiibo_gen_delay_timer);
}

/* ============================================================ */
/*  v8.1 公开刷新接口                                            */
/*                                                              */
/*  让其他模块 (典型: badge_list 里的 BLE 上传 hook callback) 在 */
/*  文件名变化后请求 AMIIBO_DETAIL 重读新文件 + 重绘 UI.         */
/*                                                              */
/*  仅在用户当前仍停在 AMIIBO_DETAIL 时由调用方负责判断后再调.   */
/*  本函数内部复用上方的 static helpers, 不重复打开 vfs.        */
/* ============================================================ */
bool amiibo_scene_amiibo_detail_refresh(app_amiibo_t *app,
                                        const char *new_file_name) {
    if (!app || !new_file_name || new_file_name[0] == '\0') {
        return false;
    }
    if (!amiibo_scene_amiibo_detail_reload_ntag(app, new_file_name)) {
        return false;
    }
    amiibo_scene_amiibo_detail_reload_files(app);
    mui_update(mui());
    return true;
}
