/*
 * star_channels.h
 *
 * 光遇徽章定制版: 固定的 12 个渠道定义 (v8.2-fix6 新增 快手, 在荣耀下面).
 *
 * 渠道列表来自用户需求 (光遇手游各发行渠道 / 国际服 / 测试服).
 * 渠道是不可编辑/不可增减的, 因此用枚举 + 静态表写死.
 *
 * 磁盘上每个渠道对应一个英文短码目录:
 *   /star/<dir_name>/<account>/<badge>.bin
 *
 * 显示名走 i18n key (zh_Hans 中文 / 其它语言英文).
 */
#ifndef STAR_CHANNELS_H
#define STAR_CHANNELS_H

#include <stdint.h>
#include <stddef.h>
#include "i18n/string_id.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 磁盘上的根目录, 所有渠道都挂在这下面. */
#define STAR_ROOT_FOLDER          "/star"

/** 新徽章 inbox: 用户通过 BLE 上传的 .bin 临时放在这里, 用作"新建徽章"的来源. */
#define STAR_INBOX_FOLDER         "/star/_inbox"

/** 默认账号的磁盘目录名 (固定 ASCII). */
#define STAR_DEFAULT_ACCOUNT_DIR  "default"

typedef enum {
    STAR_CH_NETEASE = 0,    /* 网易 */
    STAR_CH_9GAME,          /* 九游 */
    STAR_CH_VIVO,           /* vivo */
    STAR_CH_HUAWEI,         /* 华为 */
    STAR_CH_4399,           /* 4399 */
    STAR_CH_YYB,            /* 应用宝 */
    STAR_CH_HONOR,          /* 荣耀 */
    STAR_CH_KUAISHOU,       /* 快手 (v8.2-fix6 新增, 在荣耀下面) */
    STAR_CH_BILIBILI,       /* 哔哩哔哩 */
    STAR_CH_GLOBAL,         /* 国际服 */
    STAR_CH_HUAWEI_GLOBAL,  /* 华为国际服 */
    STAR_CH_TEST,           /* 测试服 */
    STAR_CH_COUNT
} star_channel_id_t;

typedef struct {
    star_channel_id_t id;
    const char *dir_name;          /* 磁盘短码 (ASCII), 例: "netease" */
    L_StringID    name_i18n_key;   /* 显示用 i18n key */
    /* v8.1-fix2: 该渠道对应的 Android 包名 (ASCII), 用作 NDEF AAR
     * (Android Application Record) 的 payload. 手机贴到 Pixl.js
     * 读到这条 AAR 后会直接打开对应渠道的 Sky 光遇客户端,
     * 不再让系统 NDEF 调度器去猜.
     *
     * 取值来自 Star Android 项目 MainActivity.getAppNameByPackage(),
     * 一一对应. 见 star_channels.c 表. */
    const char *pkg_name;
} star_channel_t;

extern const star_channel_t star_channels[STAR_CH_COUNT];

/** 取第 idx 个渠道 (0..STAR_CH_COUNT-1); 越界返回 NULL. */
const star_channel_t* star_channels_get(uint32_t idx);

/** 用磁盘短码反查渠道; 找不到返回 NULL. */
const star_channel_t* star_channels_find_by_dir(const char *dir_name);

#ifdef __cplusplus
}
#endif

#endif /* STAR_CHANNELS_H */
