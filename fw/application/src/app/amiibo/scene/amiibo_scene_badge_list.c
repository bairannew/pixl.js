/*
 * amiibo_scene_badge_list.c   (v8.1 重写)
 *
 * 当前账号下的徽章列表 (/star/<channel>/<account>/*.bin).
 *
 *   - 短按某徽章      -> 复用 AMIIBO_DETAIL 场景, 加载到模拟器并显示详情.
 *   - 长按某徽章      -> 进入 BADGE_NAME_INPUT 改名.
 *   - 短按 "新建徽章" -> v8.1 新流程: 在账号目录下静默创建 new.bin 占位
 *                       (内容: 一份合法的随机 NTAG215 dump) + 注册 BLE 上传 hook,
 *                       然后 **直接进入** AMIIBO_DETAIL 视图查看这枚空徽章.
 *                       用户在此视图上发起 BLE 上传, 写入完成后:
 *                         a) 从 NTAG dump 里解析 NDEF URI Record -> URL.
 *                         b) URL ?s=BASE64 解码 -> 抽 sk=SKY-XX-XX-XX-XX.
 *                         c) 用 sk 查徽章数据库, 命中则把文件 rename 成
 *                            "<中文名>.bin" + 写 vfs_meta.notes 中文长名.
 *                         d) 同步刷新 AMIIBO_DETAIL 视图, 让用户立刻看到结果.
 *                       未识别 (没找到 sk / NDEF 不存在): 不强迫改名,
 *                       toast 提示 "未识别,长按可改名". 用户回到列表后长按可手动改.
 *
 *   - 长按 "新建徽章" -> 同上 (兼容旧肌肉记忆).
 *   - 短按 "返回"     -> 回到账号选择.
 *
 * 与 v8 的差别:
 *   v8 在点 "新建徽章" 后会进入一个独立场景 BADGE_WAIT_UPLOAD, 它弹一个
 *   msg_box: "等待蓝牙上传\n完成后自动识别" + "取消" 按钮.
 *   用户反馈这一步像是 "设备在叫用户去做一次额外的蓝牙上传",
 *   ("不要使用蓝牙上传空白BLE bin再进行感应接取BLE 啊 / 这样太麻烦了").
 *   v8.1 把这个中间场景整个去掉: 占位文件由设备自己悄悄建好, 同时挂好 hook,
 *   直接跳到 AMIIBO_DETAIL 让用户 "进入这个 bin". hook 是异步的, 写入到达时
 *   静默触发解析/改名/刷新, 全程不再弹出 "等待蓝牙上传" msg_box.
 *
 * 兼容性: 这个场景为了与 AMIIBO_DETAIL 兼容, 仍然把
 *   app->current_folder = "/star/<channel>/<account>"
 *   app->current_file   = "<selected>.bin"
 * 设好, 然后跳转到 AMIIBO_DETAIL.
 * AMIIBO_DETAIL 通过 current_folder + current_file 拼绝对路径加载.
 *
 *  ─ 光遇徽章定制版 (Sky Badge Edition) v8.1 - badge_list ─
 */
#include "amiibo_scene.h"
#include "app_amiibo.h"
#include "mini_app_launcher.h"
#include "mini_app_registry.h"
#include "mui_list_view.h"
#include "mui_msg_box.h"
#include "mui_toast_view.h"
#include "mui_core.h"
#include "ntag_def.h"
#include "ntag_store.h"
#include "nrf_log.h"
#include "vfs.h"
#include "vfs_meta.h"
#include "i18n/language.h"
#include "port/star_channels.h"
#include "star_upload_hook.h"
#include "sky_badge_parser.h"
#include "df_proto_vfs.h"   /* v8.2-fix11: 强制关 BLE 上传句柄, 解决迁移/扫描被句柄占用阻塞 */

#include <stdio.h>
#include <string.h>

#define ICON_BADGE 0xe1ed   /* file icon */
#define ICON_NEW   0xe1ed
#define ICON_BACK  0xe069

#define BADGE_ITEM_NEW   0xFFFFFFFEu
#define BADGE_ITEM_BACK  0xFFFFFFFFu
/* v9.0-fix1: 返回主菜单 (= channel_select) — 比反复按返回省事. */
#define BADGE_ITEM_MAIN  0xFFFFFFFDu
#define ICON_HOME        0xe1f0

/* v8.1: NTAG215 dump 540 字节是常规, 但 BLE 客户端在某些版本里会把
 * vfs_meta 也一并塞进文件 (~ +60 字节), NTAG216 dump 924 字节也可能出现.
 * 这里给到 1024, 让上限覆盖 NTAG216 + meta 仍有富裕. 之前 600 卡住
 * 任何 > 600 的上传都进 "bad uploaded size" 分支, 直接吃掉占位文件,
 * 表象就是 "BLE 写完了但自动改名没发生". */
#define UPLOAD_BUF_MAX  1024

/* 前向声明 (因为 hook callback 要复用 reload) */
static void amiibo_scene_badge_list_reload(app_amiibo_t *app);


/* ============================================================ */
/*  UTF-8 / 文件名清理 (共用 helpers, 与 v8 的 wait_upload 同源)   */
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
/*  路径辅助                                                     */
/* ============================================================ */

/* v8.2: badge_list 现在挂在 category 下面, 不再直接挂账号下面.
 *
 * 路径 = /star/<channel>/<account>/<category>
 * BLE 前缀 = E:/star/<channel>/<account>/<category>/
 *
 * 所有现有 "format_account_path" 名字保留, 但实际拼到 category 一层.
 * (改函数名会影响很多 callsite, 留个注释即可.) */

/* 当前徽章所在目录的绝对路径 (账号 + 分类, 不含末尾 '/'). */
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

/* 驱动器 label 字符 ('I' / 'E') -> 例 "I:" / "E:". */
static char drive_label_char(vfs_drive_t drv) {
    return (drv == VFS_DRIVE_INT) ? 'I' : 'E';
}

/* 当前 (account + category) 的 BLE 完整前缀,
 * 例 "E:/star/netease/default/_default/" (含末尾 '/'). */
static void format_account_full_prefix(app_amiibo_t *app, char *out, size_t out_cap) {
    const star_channel_t *ch = star_channels_get(app->star_channel_idx);
    const char *cat = string_get_cstr(app->star_category_dir);
    if (cat == NULL || cat[0] == '\0') cat = "_default";
    snprintf(out, out_cap, "%c:%s/%s/%s/%s/",
             drive_label_char(app->current_drive),
             STAR_ROOT_FOLDER,
             ch ? ch->dir_name : "unknown",
             string_get_cstr(app->star_account_dir),
             cat);
}

/* "E:/star/.../x.bin" -> "/star/.../x.bin" (去掉 "X:" 两字符前缀). */
static const char *strip_drive_prefix(const char *full_path) {
    if (full_path == NULL) return NULL;
    if (full_path[0] && full_path[1] == ':' && full_path[2] == '/') {
        return full_path + 2;
    }
    return full_path;
}


/* ============================================================ */
/*  占位文件: 在账号目录里选一个空闲的 new.bin / new_N.bin        */
/* ============================================================ */

static bool placeholder_candidate_name(int suffix, char *out_base, size_t out_cap) {
    int n;
    if (suffix == 0) {
        n = snprintf(out_base, out_cap, "new.bin");
    } else {
        n = snprintf(out_base, out_cap, "new_%d.bin", suffix);
    }
    return n > 0 && (size_t)n < out_cap;
}

/* 选择一个不与现有文件冲突的占位文件名, 写到 app->star_wait_placeholder.
 * 同时返回完整路径到 out_full_path (不含驱动器前缀). */
static bool pick_placeholder(app_amiibo_t *app,
                             char *out_full_path, size_t out_cap) {
    vfs_driver_t *p_drv = vfs_get_driver(app->current_drive);
    char dir[VFS_MAX_PATH_LEN];
    char base[VFS_MAX_NAME_LEN];

    format_account_path(app, dir, sizeof(dir));

    for (int suffix = 0; suffix < 100; suffix++) {
        if (!placeholder_candidate_name(suffix, base, sizeof(base))) {
            return false;
        }
        int n = snprintf(out_full_path, out_cap, "%s/%s", dir, base);
        if (n < 0 || (size_t)n >= out_cap) return false;

        vfs_obj_t obj;
        if (p_drv->stat_file(out_full_path, &obj) != VFS_OK) {
            /* 不存在, 可用 */
            string_set_str(app->star_wait_placeholder, base);
            return true;
        }
    }
    return false;
}

/* 写入占位 .bin (一份随机 NTAG215 dump). 失败返回 false. */
static bool write_placeholder(app_amiibo_t *app, const char *full_path) {
    vfs_driver_t *p_drv = vfs_get_driver(app->current_drive);
    ntag_t ntag;
    ntag_store_new_rand(&ntag);
    int32_t res = p_drv->write_file_data(full_path, ntag.data,
                                          _ntag_data_size(&ntag));
    return res > 0;
}

/* 删除占位文件 (如果它和 keep_path 不是同一个). keep_path 可为 NULL. */
static void purge_placeholder(app_amiibo_t *app, const char *keep_path) {
    if (string_size(app->star_wait_placeholder) == 0) return;
    vfs_driver_t *p_drv = vfs_get_driver(app->current_drive);
    char dir[VFS_MAX_PATH_LEN];
    char ph_path[VFS_MAX_PATH_LEN];
    format_account_path(app, dir, sizeof(dir));
    int n = snprintf(ph_path, sizeof(ph_path), "%s/%s",
                     dir, string_get_cstr(app->star_wait_placeholder));
    if (n < 0 || (size_t)n >= sizeof(ph_path)) return;

    if (keep_path && strcmp(keep_path, ph_path) == 0) {
        /* BLE 写入的就是这个占位文件 -> 它会被 rename 走或保留. 不删. */
        return;
    }
    (void)p_drv->remove_file(ph_path);
    NRF_LOG_INFO("badge_list: purged stale placeholder %s",
                 nrf_log_push(ph_path));
}


/* ============================================================ */
/*  v8.2-fix10: 孤儿 .bin 自动迁移到 _default 分类                */
/*                                                                */
/*  问题背景 (来自用户报障):                                       */
/*    "新建徽章后 连接蓝牙明明有徽章文件 但是在模拟器上面 一个也没  */
/*     有啊 只有新建徽章和返回啊 徽章列表呢 不显示啊"               */
/*                                                                */
/*  v8.2 在 account 和 badge 之间插了 "category" 一层, 路径从       */
/*    v8.1:  /star/<ch>/<acct>/<file>.bin                          */
/*  变成了                                                         */
/*    v8.2:  /star/<ch>/<acct>/<category>/<file>.bin               */
/*                                                                */
/*  badge_list 只扫描带 category 这一层的目录, 所以两类文件会       */
/*  "看不见":                                                      */
/*                                                                */
/*    A) 从 v8.1 升级上来的老徽章 — 都直接躺在账号根                */
/*       /star/<ch>/<acct>/*.bin 下, 没有 category 子目录.          */
/*                                                                */
/*    B) BLE 客户端 (网页 / 手机 app) 完全不知道 v8.2 多了一层      */
/*       category. 用户用文件浏览器上传到 E:/star/<ch>/<acct>/      */
/*       <file>.bin 是非常常见的操作 — 上传完文件就 "孤儿" 在账号   */
/*       根, 既触发不了 BLE 上传 hook (前缀含 <category>/, 对不上),  */
/*       也进不了 badge_list 的扫描范围.                            */
/*                                                                */
/*  解决方案:                                                       */
/*    在用户每次进入 "默认分类 (_default)" 的徽章列表时, 顺便扫一遍 */
/*    账号根目录, 把直接放在那里的 .bin 移到 _default 子目录里.    */
/*    用户分类是用户自己建的, 不动 (避免无意中把别处的徽章挪进去). */
/*                                                                */
/*  特点:                                                          */
/*    - 幂等: 没有孤儿文件就什么也不做.                             */
/*    - 静默: 不弹 toast / msg_box, 让用户直接看到迁过来的徽章.     */
/*    - 撞名安全: 目标已存在则加 "_N" 后缀.                         */
/*    - 单次最多搬 16 个 (LFS 目录迭代器期间最好不要 rename, 所以   */
/*      先收集名字再统一搬, 16 个能覆盖绝大多数升级用户; 多出来的   */
/*      下次进 badge_list 时再继续搬, 不会丢).                      */
/* ============================================================ */

/* v8.2-fix11: 把搬运批量从 16 扩到 64.
 *
 * fix10 用 16 是怕一次扫太多 .bin 在 LFS 目录迭代器期间反复 rename 影响
 * 稳定性. 但实测 16 太小: 用户在 BLE 客户端上一次性拖 30+ 张徽章到账号根
 * 是正常使用, 一次 reload 只搬 16 张, 剩下还在原地; 用户从 detail 返回
 * 再次触发 reload 才再搬 16 张, 之间状态被"丢"的徽章把用户搞迷糊 — 这
 * 也是用户报 "默认账号默认分类下徽章列表里看不到东西" 的高频原因之一.
 *
 * 现在 Pass1 (扫名字) + Pass2 (rename) 两遍法已经隔开了"迭代中改"的隐患,
 * 把上限提到 64 没有新风险, 但能覆盖几乎所有正常上传场景. */
#define MIGRATE_BATCH_MAX  64

static void migrate_orphan_bins_to_default(app_amiibo_t *app) {
    /* v9.0-fix2: 之前这里有个限制:
     *   if (cat == NULL || strcmp(cat, "_default") != 0) return;
     * 思路是 "用户分类不抢", 但用户报障实测下来这条限制反而是问题:
     *
     *   1) 用户在 BLE 客户端把 .bin 拖到 /star/<ch>/<acct>/ 账号根, 此时
     *      他可能正好在设备上进的是某个 "自己新建的分类" — fix10/11 不
     *      会跑迁移, 文件就这么挂在账号根, _default 看不到, 用户自建
     *      分类也看不到.
     *   2) 即使用户进了 _default, fix10/11 也只是把孤儿搬到 _default 一次,
     *      过几秒 BLE 客户端又上传了一份新的孤儿到账号根 — 用户已经从
     *      _default 切到自建分类了, 新孤儿永远不会被搬, 出现 "前面看得
     *      到, 后面新上传的看不到" 的诡异分布.
     *
     * 结论: 孤儿 .bin 的归属永远是 "_default 分类", 跟用户当前 in 哪个
     * 分类无关. 任何路径下进 badge_list 都顺便做一次清扫, 把孤儿挪到
     * _default. 这样不管用户在哪个分类下浏览, 进度都是同步的, 也避免
     * 上面 (2) 的 "上传时机差异" 造成可见性割裂.
     *
     * 静默 + 幂等仍然成立: 没有孤儿就什么也不做, 用户分类目录不被动. */

    /* v8.2-fix11: 修 "默认账号 + 默认分类下徽章列表为空" 的最常见根因.
     *
     * 用户痛点 (本次报障):
     *   "在默认账号和默认分类里面无法在模拟器显示徽章列表 有问题
     *    只有在自己新建的账号和自己新建的分类里面才能显示徽章数据列表"
     *
     * 现象拆解: BLE 客户端 (网页 / 手机 app) 把徽章 .bin 上传到 v8.1
     * 老路径 /star/<ch>/default/xxx.bin (没有 category 层). fix10 已经
     * 写了 migrate_orphan_bins_to_default 把这些孤儿搬到 _default 子目录.
     * 但实测 fix10 在用户场景下经常 silently 失败 — 原因是 BLE 客户端
     * 仍在线 (用户没断开), 还持有打开的文件句柄, LittleFS 的 rename_file
     * 因父目录有活句柄而拒绝 → 孤儿原地不动 → 用户进 _default 看不到.
     *
     * 自建账号 / 自建分类下用户都是先在设备 UI 创建目录再到 BLE 上传,
     * 客户端会把文件放在 "用户选中的目录" (即正确的 _default 或自建分类
     * 子目录) 里, 不需要迁移 → 看上去 "自建一切正常".
     *
     * 修法: 在 rename 之前 (实际上是整个迁移开始之前) 调
     * df_proto_vfs_close_active_upload() 把 BLE 残留句柄强制关掉,
     * 让 LFS rename_file 不再被拒. 幂等, 没打开就空操作.
     *
     * v9.0-fix2: 同时关 dir 句柄. fix11 漏覆盖了 BLE dir_read 分块协议
     * 留下的 dir 句柄 — 那是另一条让 LFS rename_file 失败的路径. */
    df_proto_vfs_close_active_upload();
    df_proto_vfs_close_active_dir();

    const star_channel_t *ch = star_channels_get(app->star_channel_idx);
    if (ch == NULL) return;

    const char *acct = string_get_cstr(app->star_account_dir);
    if (acct == NULL || acct[0] == '\0') return;

    vfs_driver_t *p_drv = vfs_get_driver(app->current_drive);

    /* 账号根目录: /star/<ch>/<acct> */
    char acc_dir[VFS_MAX_PATH_LEN];
    int n = snprintf(acc_dir, sizeof(acc_dir), "%s/%s/%s",
                     STAR_ROOT_FOLDER, ch->dir_name, acct);
    if (n < 0 || (size_t)n >= sizeof(acc_dir)) return;

    /* 目标分类目录: /star/<ch>/<acct>/_default */
    char def_dir[VFS_MAX_PATH_LEN];
    n = snprintf(def_dir, sizeof(def_dir), "%s/_default", acc_dir);
    if (n < 0 || (size_t)n >= sizeof(def_dir)) return;

    /* 防御性: 目标目录可能还不存在 (用户从 channel_select 一路跳过来,
     * 这里再保险一次, idempotent). category_select 也会建, 不冲突. */
    p_drv->create_dir(def_dir);

    /* Pass 1: 扫账号根, 收集所有直接放在那里的 .bin 名字.
     * 不在 read_dir 循环里直接 rename — LFS 目录迭代器与并发 rename 之间
     * 的行为没保证 (可能跳条目 / 重复 / 错乱). 先全部读完再统一搬. */
    char to_migrate[MIGRATE_BATCH_MAX][VFS_MAX_NAME_LEN];
    int count = 0;

    vfs_dir_t dir;
    if (p_drv->open_dir(acc_dir, &dir) != VFS_OK) return;

    vfs_obj_t obj;
    while (p_drv->read_dir(&dir, &obj) == VFS_OK) {
        if (count >= MIGRATE_BATCH_MAX) break;
        if (obj.type != VFS_TYPE_REG) continue;
        size_t nn = strlen(obj.name);
        if (nn < 4) continue;
        if (strcmp(obj.name + nn - 4, ".bin") != 0) continue;

        /* 名字按 vfs_obj_t.name 缓冲拷一份, name 本身就在 VFS_MAX_NAME_LEN
         * 大小, 直接 strncpy 不会越界. */
        strncpy(to_migrate[count], obj.name, VFS_MAX_NAME_LEN - 1);
        to_migrate[count][VFS_MAX_NAME_LEN - 1] = '\0';
        count++;
    }
    p_drv->close_dir(&dir);

    if (count == 0) return;

    /* Pass 2: 真正搬. 每个文件最多试 100 个 suffix 防止撞名死循环. */
    int moved = 0;
    for (int i = 0; i < count; i++) {
        char src[VFS_MAX_PATH_LEN];
        char dst[VFS_MAX_PATH_LEN];

        int sn = snprintf(src, sizeof(src), "%s/%s", acc_dir, to_migrate[i]);
        if (sn < 0 || (size_t)sn >= sizeof(src)) continue;

        bool placed = false;
        for (int suffix = 0; suffix < 100; suffix++) {
            int dn;
            if (suffix == 0) {
                dn = snprintf(dst, sizeof(dst), "%s/%s",
                              def_dir, to_migrate[i]);
            } else {
                /* 在 ".bin" 前面插 "_N" */
                size_t name_n = strlen(to_migrate[i]);
                if (name_n < 4) break;          /* 防御: 没有 ".bin" 后缀就跳过 */
                char base_no_ext[VFS_MAX_NAME_LEN];
                size_t base_len = name_n - 4;
                if (base_len >= sizeof(base_no_ext)) break;
                memcpy(base_no_ext, to_migrate[i], base_len);
                base_no_ext[base_len] = '\0';
                dn = snprintf(dst, sizeof(dst), "%s/%s_%d.bin",
                              def_dir, base_no_ext, suffix);
            }
            if (dn < 0 || (size_t)dn >= sizeof(dst)) break;

            vfs_obj_t exist;
            if (p_drv->stat_file(dst, &exist) != VFS_OK) {
                /* 目标不存在 -> rename.
                 *
                 * v9.0-fix1 关键修复: 修 "默认账号 + 默认分类下徽章列表为空"
                 * 的最常见根因.  fix11 在迁移开始前 close 了一次 active upload,
                 * 但 BLE 客户端 (网页 / 手机 app) 在迁移期间又会反复轮询账号目录,
                 * 把另外的 .bin 重新 open. 单次 rename_file 落在 "目录里有活句柄"
                 * 的窗口里, LittleFS 拒绝 → 孤儿原地不动 → 用户进 _default
                 * 看不到自动改名后的徽章 (但 new.bin / 用户分类下的 .bin 正常,
                 * 因为它们不走迁移路径).
                 *
                 * 修法: 关 → rename, 重复 3 次, 每次都先 close_active_upload 把
                 * 当前活的句柄清掉再尝试. 3 次足以覆盖 BLE 客户端任何现行
                 * 轮询节奏. 与 account/category rename 同策略.
                 *
                 * v9.0-fix2: 同时关 dir 句柄. fix1 漏了 dir_read 分块协议这条
                 * 路径, 客户端在 read_dir 半截时段持有的 dir 句柄一样会卡
                 * rename_file. 现在两个都关. */
                int32_t rr = VFS_ERR_FAIL;
                for (int attempt = 0; attempt < 3; attempt++) {
                    df_proto_vfs_close_active_upload();
                    df_proto_vfs_close_active_dir();
                    rr = p_drv->rename_file(src, dst);
                    if (rr == VFS_OK) break;
                }
                if (rr == VFS_OK) {
                    NRF_LOG_INFO("migrate orphan: %s -> %s",
                                 nrf_log_push(src), nrf_log_push(dst));
                    placed = true;
                } else {
                    NRF_LOG_WARNING("migrate orphan: rename failed for %s (after retries)",
                                    nrf_log_push(src));
                }
                break;
            }
            /* 目标存在, 试下一个 suffix. */
        }
        if (placed) moved++;
    }

    if (moved > 0) {
        NRF_LOG_INFO("badge_list: migrated %d orphan .bin file(s) to _default",
                     moved);
    }
}


/* ============================================================ */
/*  Autofill 命中后: 构造目标文件名 + rename + 写 meta             */
/* ============================================================ */

/* 计算 basename 可用字节预算 (受 VFS_MAX_PATH_LEN, VFS_MAX_NAME_LEN 双重约束).
 * ext 含 '.', 例 ".bin".
 *
 * v8.2: 路径多了一层 category, 所以 dir_len 把它算上.
 *   dir = /star/<ch>/<acct>/<cat>
 */
static size_t star_basename_budget(app_amiibo_t *app, const char *ext) {
    const star_channel_t *ch = star_channels_get(app->star_channel_idx);
    const char *chname = ch ? ch->dir_name : "unknown";
    const char *cat = string_get_cstr(app->star_category_dir);
    if (cat == NULL || cat[0] == '\0') cat = "_default";
    size_t dir_len = strlen(STAR_ROOT_FOLDER) + 1 +
                     strlen(chname) + 1 +
                     strlen(string_get_cstr(app->star_account_dir)) + 1 +
                     strlen(cat);
    size_t ext_len = ext ? strlen(ext) : 0;
    if (dir_len + 1 + ext_len + 1 >= VFS_MAX_PATH_LEN) return 0;
    size_t by_path = (VFS_MAX_PATH_LEN - 1) - (dir_len + 1) - ext_len;
    if (ext_len + 1 >= VFS_MAX_NAME_LEN) return 0;
    size_t by_name = (VFS_MAX_NAME_LEN - 1) - ext_len;
    return by_path < by_name ? by_path : by_name;
}

/* 构造 "<display>[_N]<ext>" 形态的 basename (含扩展名), 写到 out_base.
 * suffix>0 时追加 "_N". 失败返回 false (预算不足). */
static bool build_target_basename(app_amiibo_t *app,
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
    /* 加扩展名 */
    size_t cur = strlen(out_base);
    memcpy(out_base + cur, ext, ext_len);
    out_base[cur + ext_len] = '\0';
    return true;
}


/* ============================================================ */
/*  BLE 上传完成 hook: 解析 NDEF URL → 解码 sk → 改名 + 写 meta    */
/*                                                                */
/*  本 hook 由 star_upload_hook_set 注册, 在 df_proto_handler_vfs_  */
/*  file_close 走完之后异步派发 (实际是 main loop 同步派发).        */
/*                                                                */
/*  hook 是 "一次性": 触发后第一时间 star_upload_hook_clear() 自    */
/*  清, 避免接下来用户在同一目录做别的写入还被错误识别成 "新建徽章".*/
/* ============================================================ */

static void badge_list_upload_hook_cb(const char *full_path, void *user_data) {
    app_amiibo_t *app = (app_amiibo_t *)user_data;
    if (app == NULL) return;

    /* v8.1-fix2: 取消"一次性"语义 —— 用户希望连续刷多张 BLE 标签
     * 都能命中自动识别 + 改名。原版 v8.1 在第一次触发后就自清 hook,
     * 导致后续上传统统落到无名文件, 不再识别。
     *
     * 现在: hook 保持安装, 每个文件 close 都跑一次解析 + 改名。
     * 仍然遵守 star_upload_hook_set 注册的目录前缀约束 ——
     * 只对当前账号目录里的写入触发, 别的目录不抢。
     *
     * 退出 amiibo app 时 (app_amiibo_on_kill) 还会主动 clear, 不会泄漏。
     *
     * star_wait_hook_fired 标志保留, 只用于"首次触发"事件 (例如关掉
     * wait_upload 的小转场), 不再用来限制重复触发。 */
    if (!app->star_wait_hook_fired) {
        app->star_wait_hook_fired = true;
    }
    /* 不再调用 star_upload_hook_clear() — 让 hook 跨多次上传持续生效 */

    vfs_driver_t *p_drv = vfs_get_driver(app->current_drive);
    const char *local_path = strip_drive_prefix(full_path);
    if (local_path == NULL) {
        NRF_LOG_ERROR("badge_list_hook: bad full_path");
        mui_toast_view_show(app->p_toast_view,
                            getLangString(_L_STAR_BADGE_SAVE_FAILED));
        return;
    }

    /* 读这个文件全部内容 */
    vfs_obj_t obj;
    if (p_drv->stat_file(local_path, &obj) != VFS_OK ||
        obj.size == 0 || obj.size > UPLOAD_BUF_MAX) {
        NRF_LOG_ERROR("badge_list_hook: bad uploaded size or stat failed");
        mui_toast_view_show(app->p_toast_view,
                            getLangString(_L_STAR_BADGE_SAVE_FAILED));
        purge_placeholder(app, local_path);
        return;
    }

    static uint8_t buf[UPLOAD_BUF_MAX];
    int32_t r = p_drv->read_file_data(local_path, buf, obj.size);
    if (r != (int32_t)obj.size) {
        NRF_LOG_ERROR("badge_list_hook: short read want=%u got=%d",
                      (unsigned)obj.size, r);
        mui_toast_view_show(app->p_toast_view,
                            getLangString(_L_STAR_BADGE_SAVE_FAILED));
        purge_placeholder(app, local_path);
        return;
    }

    /* ============================================================ */
    /*  v8.2-fix5: 把 BLE 上传到的"另一个文件"合并到占位 new.bin 上.    */
    /*                                                                */
    /*  用户痛点: 点"新建徽章"后 detail 视图打开的是占位 new.bin, 但    */
    /*  BLE 客户端一般用自己的文件名写入 (例: device.bin), 结果磁盘上    */
    /*  并排出现两个文件 —— 当前 detail 显示的 new.bin 仍然是随机内容,   */
    /*  另一个 device.bin 才是真徽章. 即使后续 hook 把 device.bin       */
    /*  rename 成 <中文名>.bin、把 new.bin 删掉, 用户的心智模型           */
    /*  ("我现在就在这个 bin 里, 数据应该落到这个 bin") 已经被破坏了.    */
    /*                                                                */
    /*  解决方案: 一旦发现 BLE 写到了非占位路径, 立刻把它"挪"进占位:     */
    /*    1) 把刚刚读出来的数据写回 ph_path 覆盖随机内容                */
    /*    2) 删掉 BLE 那个临时源文件                                  */
    /*    3) 把 local_path 改指 ph_path, 后面正常 rename->中文名         */
    /*  这样从用户视角看, "这个 bin" 自己变成了新徽章, 而不是另起一个.  */
    /* ============================================================ */
    {
        const char *ph_name = string_get_cstr(app->star_wait_placeholder);
        if (ph_name != NULL && ph_name[0] != '\0') {
            char ph_dir[VFS_MAX_PATH_LEN];
            static char ph_full[VFS_MAX_PATH_LEN];  /* static: 给后续 local_path 续命 */
            format_account_path(app, ph_dir, sizeof(ph_dir));
            int pn = snprintf(ph_full, sizeof(ph_full), "%s/%s", ph_dir, ph_name);
            /* v8.2-fix8: 占位有可能已经被一次先到的 hook callback 合并 + 删掉,
             * 但 star_wait_placeholder 字段还没清空 (旧版根本不清). stat 一下,
             * 不在 disk 上就放弃合并 — local_path 维持 BLE 原路径走 rename,
             * 不至于把"已经不存在的占位"当成有效目标. */
            vfs_obj_t ph_stat;
            bool ph_exists = (pn > 0 && (size_t)pn < sizeof(ph_full) &&
                              p_drv->stat_file(ph_full, &ph_stat) == VFS_OK);
            if (!ph_exists) {
                /* placeholder 已不存在 — 顺手清掉残留状态, 后续不再走合并路径 */
                string_reset(app->star_wait_placeholder);
            } else if (strcmp(local_path, ph_full) != 0) {
                /* BLE 上传到的不是占位, 做一次原地合并 */
                int32_t wr = p_drv->write_file_data(ph_full, buf, obj.size);
                if (wr > 0) {
                    /* 占位现在装着真数据, 把 BLE 源文件删了 */
                    (void)p_drv->remove_file(local_path);
                    NRF_LOG_INFO("badge_list_hook: merged %s -> %s",
                                 nrf_log_push((char *)local_path),
                                 nrf_log_push(ph_full));
                    /* 后续 rename 走占位路径 */
                    local_path = ph_full;
                } else {
                    /* 写占位失败 (磁盘满 / 路径预算等) — 不致命, 继续按
                     * 原 BLE 源路径走 rename. 用户至少能拿到改完名的徽章. */
                    NRF_LOG_WARNING("badge_list_hook: merge to placeholder failed (%d), fallback to inline rename",
                                    (int)wr);
                }
            }
        }
    }

    /* 一站式解析: NTAG dump → NDEF URI → ?s=BASE64 → sk → 数据库条目 */
    const sky_badge_entry_t *e = sky_badge_try_autofill(buf, (size_t)obj.size);

    /* 提取上传文件的 basename (用于跟占位比较 / 显示) */
    const char *src_basename = strrchr(local_path, '/');
    src_basename = src_basename ? (src_basename + 1) : local_path;

    char dir[VFS_MAX_PATH_LEN];
    format_account_path(app, dir, sizeof(dir));

    /* 最终落地的 basename (可能 == 上传源名, 也可能是 rename 后的中文名) */
    char placed_base[VFS_MAX_NAME_LEN] = {0};
    bool placed_ok = false;

    if (e != NULL) {
        /* ============ 命中: rename 到 "<中文名>（<中文备注>）.bin" + 清空 meta ============
         *
         * v8.1-fix2 修改: 文件名同时包含中文名 + 中文备注 (用全角括号), vfs_meta.notes 留空。
         * 旧版本里把 "<name> (<note>)" 重复写到 meta.notes, 用户在 badge_list 里
         * 看到 "标题<name>.bin / 副标题<name>(<note>)", 名字出现两次太长。
         */
        char file_label[128];   /* 用作文件名 (不含扩展名), 包含 name + 全角括号备注 */
        const bool has_note_b = (e->note_zh && e->note_zh[0]);
        size_t name_len = strlen(e->name_zh);
        size_t note_len = has_note_b ? strlen(e->note_zh) : 0;
        if (has_note_b && name_len + note_len + 7 < sizeof(file_label)) {
            /* "<name>（<note>）" — UTF-8 下"（""）"各占 3 字节,
             *  所以最多多吃 6 字节, + 1 个 \0 = 7. */
            snprintf(file_label, sizeof(file_label), "%s（%s）",
                     e->name_zh, e->note_zh);
        } else {
            utf8_copy_bounded(file_label, sizeof(file_label), e->name_zh);
        }

        char target_base[VFS_MAX_NAME_LEN];
        char target_path[VFS_MAX_PATH_LEN];

        for (int suffix = 0; suffix < 100; suffix++) {
            if (!build_target_basename(app, file_label, suffix, ".bin",
                                       target_base, sizeof(target_base))) {
                break;
            }
            int tn = snprintf(target_path, sizeof(target_path), "%s/%s",
                              dir, target_base);
            if (tn < 0 || (size_t)tn >= sizeof(target_path)) break;
            if (strcmp(target_path, local_path) == 0) {
                /* 居然 BLE 已经用了这个名字, 直接当成已就位 */
                utf8_copy_bounded(placed_base, sizeof(placed_base), target_base);
                placed_ok = true;
                break;
            }
            vfs_obj_t existing;
            if (p_drv->stat_file(target_path, &existing) != VFS_OK) {
                /* v9.0-fix1: 与 migrate_orphan / account_rename 同策略,
                 * 关 + rename 重试 3 次, 防 BLE 客户端轮询窗口阻塞 LFS.
                 * v9.0-fix2: 同时关 dir 句柄, 覆盖 dir_read 分块协议这条路径. */
                int32_t rr = VFS_ERR_FAIL;
                for (int attempt = 0; attempt < 3; attempt++) {
                    df_proto_vfs_close_active_upload();
                    df_proto_vfs_close_active_dir();
                    rr = p_drv->rename_file(local_path, target_path);
                    if (rr == VFS_OK) break;
                }
                if (rr == VFS_OK) {
                    utf8_copy_bounded(placed_base, sizeof(placed_base), target_base);
                    placed_ok = true;
                    break;
                } else {
                    NRF_LOG_ERROR("badge_list_hook: rename failed");
                    break;
                }
            }
        }

        if (placed_ok) {
            /* v8.1-fix2: 清空 vfs_meta.notes —— 文件名里已经写了完整中文名(+备注),
             * 副标题再写一遍纯粹是重复。 */
            vfs_meta_t meta;
            memset(&meta, 0, sizeof(meta));
            meta.has_notes = false;
            uint8_t meta_buf[VFS_MAX_META_LEN];
            memset(meta_buf, 0, sizeof(meta_buf));
            vfs_meta_encode(meta_buf, sizeof(meta_buf), &meta);

            char target_full[VFS_MAX_PATH_LEN];
            snprintf(target_full, sizeof(target_full), "%s/%s",
                     dir, placed_base);
            (void)p_drv->update_file_meta(target_full, meta_buf, sizeof(meta_buf));

            /* 如果上传的不是占位文件, 把占位删掉 */
            purge_placeholder(app, local_path);

            char toast[80];
            snprintf(toast, sizeof(toast), "%s%s",
                     getLangString(_L_STAR_AUTOFILL_HIT), e->name_zh);
            mui_toast_view_show(app->p_toast_view, toast);
        } else {
            /* 名字预算用光了 (账号目录路径过长 / 名字都被占了), fallback:
             * 保留 BLE 原文件名, 提示 saved. */
            utf8_copy_bounded(placed_base, sizeof(placed_base), src_basename);
            placed_ok = true;
            mui_toast_view_show(app->p_toast_view, getLangString(_L_STAR_BADGE_SAVED));
            purge_placeholder(app, local_path);
        }
    } else {
        /* ============ 未命中: 保留原始文件, 提示 "未识别, 长按改名" ============ */
        NRF_LOG_INFO("badge_list_hook: autofill miss");
        utf8_copy_bounded(placed_base, sizeof(placed_base), src_basename);
        placed_ok = true;
        purge_placeholder(app, local_path);
        mui_toast_view_show(app->p_toast_view,
                            getLangString(_L_STAR_AUTOFILL_MISS));
    }

    /* ============================================================ */
    /*  UI 同步: 根据当前所在的场景, 决定怎么把变化反映到屏幕上.       */
    /*                                                                */
    /*    AMIIBO_DETAIL  -> 用户正在 "进入这个 bin" 等数据, 重新加载    */
    /*                      该文件 (现在已经是改名后的真徽章) 并刷新.   */
    /*                                                                */
    /*    BADGE_LIST     -> 用户已经从 detail 后退回到列表了 (或者根本  */
    /*                      没进过 detail, 直接列表上等). 重建列表项.   */
    /*                                                                */
    /*    其它           -> 用户翻出去了, 不强行抢焦. 文件还在磁盘上,   */
    /*                      下次回 badge_list 自然就能看到.            */
    /* ============================================================ */
    if (placed_ok && placed_base[0]) {
        /* v8.2-fix9 修复 1: "新建徽章 BLE 写入完成后不会自动返回到徽章列表".
         *
         * 用户复述:
         *   "新建徽章 空白BLE 自动进入 感应写入的BLE 自动判断 自动写入徽章
         *    写入后不会自动返回到徽章列表 你现在这个感应BLE后不会保存啊
         *    不会保存 导致徽章列表没有这个数据"
         *
         * 数据其实落盘了 (上面已经做完 rename + meta), 但 fix6 后行为是
         * "感应完就停在改完名的徽章详情里 (不自动跳 new.bin, 也不回列表)".
         * 用户在 detail 里看不到列表, 自然以为 "没保存".
         *
         * 修法: 把"是否新建徽章流程"这个上下文记下来 (用 star_wait_placeholder
         * 非空当判据 - 只有 start_new_badge_flow 才会 set 这个字段). 流程结束
         * 时如果用户还在 detail, 把 detail 这一层 pop 掉, 直接回 badge_list.
         * badge_list 的 on_enter 会重扫目录, 刚改名的徽章立刻出现在列表里. */
        bool was_new_badge_flow = (string_size(app->star_wait_placeholder) > 0);

        /* v8.2-fix8: rename + merge 已成功落盘, 把占位状态清空, 这样
         *   - 同一个新建流程不会再被后续 hook 误触发 (例: BLE 重传).
         *   - 用户立即又点 "新建徽章" 时 start_new_badge_flow 看到的是
         *     干净状态, pick_placeholder 不会撞到上次留下的 ghost 文件名.
         * star_wait_dir_full 仍保留, hook 还需要过滤前缀; 它在新建流程
         * 重入时也会被 string_set_str 覆盖, 不存在累积问题. */
        string_reset(app->star_wait_placeholder);

        uint32_t cur_scene =
            mui_scene_dispatcher_current_scene(app->p_scene_dispatcher);

        if (cur_scene == AMIIBO_SCENE_AMIIBO_DETAIL) {
            if (was_new_badge_flow) {
                /* v8.2-fix9: 新建徽章流程 BLE 写入完成 -> 自动回退到 badge_list.
                 *
                 * 之前的 amiibo_scene_amiibo_detail_refresh 是 "原地刷新当前
                 * detail 视图", 用户看到的是 "这个 bin 现在变成 <中文名>.bin",
                 * 但他们不一定意识到列表里已经有了这条数据. 用户期望: 新建走完
                 * 整个流程后回到列表看到结果. 这里 pop 一层就够 (栈是
                 * badge_list -> amiibo_detail).
                 *
                 * 用 previous_scene 而非 next_scene(BADGE_LIST), 保留 badge_list
                 * 在场景栈上的原始位置, 不会让栈无限增长.
                 *
                 * 副作用: badge_list 的 on_enter 自身会 reload 列表, 不需要在
                 * 这里再 reload. previous_scene 会调 detail 的 on_exit 清掉
                 * ntag_emu_set_update_cb / app_timer_stop, 跟 fix8 那条 back_scene
                 * 的 exit 路径走的是同一套清理. */
                mui_scene_dispatcher_previous_scene(app->p_scene_dispatcher);
            } else {
                /* 不是新建徽章流程 — 用户是在 detail 里看着某张已有徽章,
                 * BLE 同时往同一账号目录上传了别的东西 (例: 第三方同步).
                 * 不抢焦, 沿用 fix6 的 "原地刷新当前 detail" 行为, 让用户
                 * 决定要不要切到新东西. */
                string_set_str(app->current_file, placed_base);
                amiibo_scene_amiibo_detail_refresh(app, placed_base);
            }
        } else if (cur_scene == AMIIBO_SCENE_BADGE_LIST) {
            amiibo_scene_badge_list_reload(app);
            mui_update(mui());
        }
    }
}


/* ============================================================ */
/*  列表渲染                                                     */
/* ============================================================ */

static void amiibo_scene_badge_list_reload(app_amiibo_t *app) {
    vfs_driver_t *p_drv = vfs_get_driver(app->current_drive);
    vfs_dir_t dir;
    vfs_obj_t obj;
    char path[VFS_MAX_PATH_LEN];
    /* v8.2: 软上限. 一个分类下徽章太多 (>100), 全展示会把 mui_list_view
     * 的 string_t (text+sub_text) 撑爆 ~12KB 堆, 加上 amiibo_detail
     * 的 string_array (~7KB), 18KB 的 TLSF 堆基本就吃光了 -> 点徽章进
     * detail 时 mui_mem_malloc 返回 NULL -> 闪退. 上限 100 给堆留余地. */
    const uint32_t BADGE_LIST_DISPLAY_CAP = 100;
    uint32_t shown = 0;

    mui_list_view_clear_items(app->p_list_view);

    format_account_path(app, path, sizeof(path));

    /* 防御性: 默认分类目录在 category_select 选中时已建过, 这里再保险一次. */
    p_drv->create_dir(path);

    /* v8.2-fix10: 修 "BLE 上传了徽章, 模拟器列表却空空如也" 这个高频报障.
     * 在扫当前分类目录之前, 先把账号根目录里 "裸放" 的 .bin (v8.1 老格式
     * 或 BLE 客户端不知道 category 层 时上传到的) 自动搬进 _default 分类.
     * 只在用户当前进入的是 _default 时跑, 别的用户分类不抢. 见函数头大段注释. */
    migrate_orphan_bins_to_default(app);

    int32_t res = p_drv->open_dir(path, &dir);
    if (res == VFS_OK) {
        while (p_drv->read_dir(&dir, &obj) == VFS_OK) {
            if (obj.type != VFS_TYPE_REG) continue;
            size_t n = strlen(obj.name);
            if (n < 4) continue;
            if (strcmp(obj.name + n - 4, ".bin") != 0) continue;

            if (shown >= BADGE_LIST_DISPLAY_CAP) {
                NRF_LOG_WARNING("badge_list: capped at %u entries", BADGE_LIST_DISPLAY_CAP);
                break;
            }

            /* text = 文件名 (用于拼路径).
             * sub_text = vfs_meta.notes 中文长名 (有的话, 当作副标题展示). */
            vfs_meta_t meta;
            memset(&meta, 0, sizeof(meta));
            vfs_meta_decode(obj.meta, sizeof(obj.meta), &meta);
            const char *sub = (meta.has_notes && meta.notes[0] != '\0')
                                  ? meta.notes : NULL;
            mui_list_view_add_item_ext(app->p_list_view, ICON_BADGE,
                                       obj.name, sub,
                                       (void *)(uintptr_t)1);
            shown++;
        }
        p_drv->close_dir(&dir);
    }

    /* "新建徽章" */
    mui_list_view_add_item(app->p_list_view, ICON_NEW,
                           getLangString(_L_STAR_NEW_BADGE),
                           (void *)(uintptr_t)BADGE_ITEM_NEW);
    /* 返回 */
    mui_list_view_add_item(app->p_list_view, ICON_BACK,
                           getLangString(_L_BACK),
                           (void *)(uintptr_t)BADGE_ITEM_BACK);
    /* v9.0-fix1: 返回主菜单 — 一次性回到渠道选择界面 (整个 app 的"主菜单").
     * 比反复按返回省事, 也省得用户等模拟器息屏再唤醒. */
    mui_list_view_add_item(app->p_list_view, ICON_HOME,
                           getLangString(_L_BACK_TO_MAIN_MENU),
                           (void *)(uintptr_t)BADGE_ITEM_MAIN);
}


/* ============================================================ */
/*  "新建徽章" 一站式入口 (v8.1):                                 */
/*    1) 在账号目录里创建 new.bin (随机 NTAG215 dump 占位)         */
/*    2) 给整个账号目录前缀挂 BLE 上传 hook                        */
/*    3) 立刻跳到 AMIIBO_DETAIL 让用户 "进入这个 bin"               */
/*                                                                */
/*  失败时静默回滚 (清 hook + 清 placeholder), 不进入 detail.       */
/* ============================================================ */

static bool start_new_badge_flow(app_amiibo_t *app) {
    /* 重置 hook 状态 */
    app->star_wait_hook_fired = false;
    string_reset(app->star_wait_placeholder);
    string_reset(app->star_wait_dir_full);

    /* 确保账号目录存在 */
    vfs_driver_t *p_drv = vfs_get_driver(app->current_drive);
    char dir[VFS_MAX_PATH_LEN];
    format_account_path(app, dir, sizeof(dir));
    p_drv->create_dir(dir);   /* 幂等 */

    /* 选一个不冲突的占位文件名 + 写入 */
    char ph_path[VFS_MAX_PATH_LEN];
    if (!pick_placeholder(app, ph_path, sizeof(ph_path))) {
        mui_toast_view_show(app->p_toast_view,
                            getLangString(_L_STAR_BADGE_SAVE_FAILED));
        return false;
    }
    if (!write_placeholder(app, ph_path)) {
        mui_toast_view_show(app->p_toast_view,
                            getLangString(_L_STAR_BADGE_SAVE_FAILED));
        return false;
    }

    /* 注册 BLE 上传完成 hook, 监视当前账号目录前缀.
     * 前缀字符串放进 app->star_wait_dir_full 长持有, hook 内部不复制. */
    char prefix[VFS_MAX_FULL_PATH_LEN];
    format_account_full_prefix(app, prefix, sizeof(prefix));
    string_set_str(app->star_wait_dir_full, prefix);
    star_upload_hook_set(string_get_cstr(app->star_wait_dir_full),
                         badge_list_upload_hook_cb, app);

    /* 准备 AMIIBO_DETAIL 跳转参数:
     *   current_folder = "/star/<channel>/<account>"
     *   current_file   = "new.bin" (或 new_N.bin)
     *   reload_amiibo_files = true   (让 detail 在 on_enter 时刷新文件列表) */
    string_set_str(app->current_folder, dir);
    string_set_str(app->current_file,
                   string_get_cstr(app->star_wait_placeholder));
    app->reload_amiibo_files = true;

    /* 这里不是 "改名模式" — 是新建. 让 badge_name_input 知道 (虽然新流程
     * 一般用不到, 但长按 detail 里的徽章去改名时会重设这个标志). */
    app->star_wait_rename_mode = false;

    NRF_LOG_INFO("badge_list: armed new_badge_flow placeholder=%s prefix=%s",
                 nrf_log_push(string_get_cstr(app->star_wait_placeholder)),
                 nrf_log_push(prefix));

    /* 一切就绪, 跳入 detail. hook 是异步的, 写入到达时它会回头来刷新 detail. */
    mui_scene_dispatcher_next_scene(app->p_scene_dispatcher,
                                    AMIIBO_SCENE_AMIIBO_DETAIL);
    return true;
}


/* ============================================================ */
/*  列表选择回调                                                 */
/* ============================================================ */

static void amiibo_scene_badge_list_on_selected(mui_list_view_event_t event,
                                                 mui_list_view_t *p_list_view,
                                                 mui_list_item_t *p_item) {
    app_amiibo_t *app = p_list_view->user_data;
    uintptr_t tag = (uintptr_t)p_item->user_data;

    if (tag == BADGE_ITEM_BACK) {
        if (event == MUI_LIST_VIEW_EVENT_SELECTED) {
            mui_scene_dispatcher_previous_scene(app->p_scene_dispatcher);
        }
        return;
    }
    if (tag == BADGE_ITEM_MAIN) {
        /* v9.0-fix1: 一键回主菜单. 栈深度 = [channel, account, category, badge_list]
         * = 4, 我们要回到 channel_select (栈底), 所以 pop 3 层. */
        if (event == MUI_LIST_VIEW_EVENT_SELECTED ||
            event == MUI_LIST_VIEW_EVENT_LONG_SELECTED) {
            mui_scene_dispatcher_back_scene(app->p_scene_dispatcher, 3);
        }
        return;
    }
    if (tag == BADGE_ITEM_NEW) {
        /* v8.1: 短按 / 长按 "新建徽章" 都直接进入 "自动接收 BLE" 流程.
         * 不再去 BADGE_WAIT_UPLOAD 弹 msg_box, 而是直接进入 AMIIBO_DETAIL,
         * 让用户 "进入空白 bin" 等数据自动流入 -> 解析 -> 改名. */
        if (event == MUI_LIST_VIEW_EVENT_SELECTED ||
            event == MUI_LIST_VIEW_EVENT_LONG_SELECTED) {
            (void)start_new_badge_flow(app);
        }
        return;
    }

    /* 一个已有的徽章 */
    if (event == MUI_LIST_VIEW_EVENT_SELECTED) {
        /* 短按 -> 复用 AMIIBO_DETAIL: 把 current_folder/current_file 设好.
         * 注意: 走的是 "查看已有徽章" 路径, 不挂 BLE 上传 hook. */
        char folder[VFS_MAX_PATH_LEN];
        format_account_path(app, folder, sizeof(folder));
        string_set_str(app->current_folder, folder);
        string_set(app->current_file, p_item->text);
        app->reload_amiibo_files = true;
        mui_scene_dispatcher_next_scene(app->p_scene_dispatcher, AMIIBO_SCENE_AMIIBO_DETAIL);
        return;
    }

    /* 长按某个已有徽章 -> v8.2-fix5: 弹 action_menu (重命名 / 删除 / 取消).
     * 之前 (v8 ~ v8.2) 是直接跳到 BADGE_NAME_INPUT 改名, 用户反馈这少了
     * 一个"删除单个徽章"的入口 —— 真要删一张就得进 detail 再开操作菜单.
     * 现在跟账号/分类的长按统一: 弹 action_menu, 选什么就做什么.
     *
     * 入参准备 (与原来给 BADGE_NAME_INPUT 的一样, 如果用户最终选 "重命名"
     * 那条路径, action_menu 会 next_scene -> BADGE_NAME_INPUT, 它读这几个字段):
     *   - star_badge_pending     = 当前文件名 (原 basename, 含 .bin)
     *   - star_badge_autofill    = vfs_meta.notes (如果有), 否则用文件名去 .bin
     *   - star_wait_rename_mode  = true  (告诉 name_input "这是改名, 不是新建")
     *   - star_rename_target_kind = BADGE  (告诉 action_menu 目标是单文件) */
    if (event == MUI_LIST_VIEW_EVENT_LONG_SELECTED) {
        char folder[VFS_MAX_PATH_LEN];
        char full[VFS_MAX_PATH_LEN];
        format_account_path(app, folder, sizeof(folder));
        snprintf(full, sizeof(full), "%s/%s", folder,
                 string_get_cstr(p_item->text));

        vfs_driver_t *p_drv = vfs_get_driver(app->current_drive);
        vfs_obj_t obj;
        if (p_drv->stat_file(full, &obj) != VFS_OK) {
            mui_toast_view_show(app->p_toast_view,
                                getLangString(_L_APP_CHAMELEON_CARD_DATA_LOAD_NOT_FOUND));
            return;
        }
        vfs_meta_t meta;
        memset(&meta, 0, sizeof(meta));
        vfs_meta_decode(obj.meta, sizeof(obj.meta), &meta);

        /* 写入预填: 优先 notes, 否则文件名去 .bin */
        char prefill[64];
        if (meta.has_notes && meta.notes[0] != '\0') {
            strncpy(prefill, meta.notes, sizeof(prefill) - 1);
            prefill[sizeof(prefill) - 1] = '\0';
        } else {
            const char *fn = string_get_cstr(p_item->text);
            strncpy(prefill, fn, sizeof(prefill) - 1);
            prefill[sizeof(prefill) - 1] = '\0';
        }
        /* v8.2-fix9 修复 2 (a): 不管 prefill 来自 notes 还是 filename, 都把
         * 末尾 ".bin" 剥掉. 原版只对 filename 这条路径剥, 如果 notes 里因为
         * 历史原因 (旧版 / 别的路径) 留了 ".bin", prefill 就带 ".bin" 进 text
         * input. 用户 confirm 后 display_name 含 ".bin", 又被原样写回 notes,
         * 同时 star_build_basename 在 ".bin" 后再追加 ".bin", 最终 filename =
         * "<name>.bin.bin", notes = "<name>.bin". badge_list 渲染 text(filename)
         * + sub_text(notes) 时两个字符串都带 ".bin" 且本身就像 "<name>.bin"
         * 的形式 -> 用户看到 "<name>.bin<name>.bin" 这种叠加变长的怪相.
         *
         * 这里在写进 star_badge_autofill 之前统一剥尾, 后续 rename 路径不会再
         * 接到 ".bin" 末尾的 display_name. 配合 fix9 修复 2(b) (badge_name_input.c
         * 里同样的防御性剥尾), 即使用户错按 / 历史脏数据也不会再叠加. */
        {
            size_t pn = strlen(prefill);
            if (pn > 4 && strcmp(prefill + pn - 4, ".bin") == 0) {
                prefill[pn - 4] = '\0';
            }
        }

        string_set(app->star_badge_pending, p_item->text);
        string_set_str(app->star_badge_autofill, prefill);
        app->star_wait_rename_mode = true;
        app->star_rename_target_kind = STAR_RENAME_KIND_BADGE;

        mui_scene_dispatcher_next_scene(app->p_scene_dispatcher,
                                        AMIIBO_SCENE_ACTION_MENU);
        return;
    }
}


/* ============================================================ */
/*  场景生命周期                                                 */
/* ============================================================ */

void amiibo_scene_badge_list_on_enter(void *user_data) {
    app_amiibo_t *app = user_data;
    amiibo_scene_badge_list_reload(app);
    mui_list_view_set_selected_cb(app->p_list_view, amiibo_scene_badge_list_on_selected);
    mui_list_view_set_user_data(app->p_list_view, app);

    /* v8.1-fix2: 每次进入徽章列表都重新挂 BLE 上传 hook, 监视当前账号目录前缀.
     * 这一步保证用户可以连续刷多张 BLE 标签 —— 不必每张都重新点"新建徽章".
     *
     * 之所以在 on_enter 里挂: 用户进徽章列表的所有路径
     * (开机 → 选账号 → 列表 / 详情页 "返回徽章大全" / wait_upload 转场)
     * 最后都会落到这里, 而 wait_upload 是一次性场景, 不能把 hook 责任压在那里.
     *
     * 防呆: 用 string_get_cstr(app->star_wait_dir_full) 这次还活着,
     *      就直接复用; 没有的话就用当前账号路径现拼一个.
     */
    {
        char prefix[VFS_MAX_FULL_PATH_LEN];
        format_account_full_prefix(app, prefix, sizeof(prefix));
        string_set_str(app->star_wait_dir_full, prefix);
        app->star_wait_hook_fired = false;
        star_upload_hook_set(string_get_cstr(app->star_wait_dir_full),
                             badge_list_upload_hook_cb, app);
    }

    mui_view_dispatcher_switch_to_view(app->p_view_dispatcher, AMIIBO_VIEW_ID_LIST);
}

void amiibo_scene_badge_list_on_exit(void *user_data) {
    app_amiibo_t *app = user_data;
    mui_list_view_set_selected_cb(app->p_list_view, NULL);
    mui_list_view_clear_items(app->p_list_view);
    /* 注意: 这里不 clear hook. hook 应该跨场景存活, 让用户在 AMIIBO_DETAIL
     * 里也能等到上传到达. hook 只在以下几个时机被清:
     *   - 用户再次 "新建徽章" 时 star_upload_hook_set 覆盖
     *   - 用户切换账号时 account_select 会清掉重设
     *   - 整个 app 退出时 app_amiibo_on_kill 兜底清掉
     *
     * v8.1-fix2: hook 不再"触发一次就自清" —— 改成跨多次上传持续生效,
     *           支持用户连续刷多张 BLE 标签都能命中自动识别 + 改名.
     */
}
