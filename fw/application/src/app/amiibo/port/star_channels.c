/*
 * star_channels.c - 12 个渠道的静态表.
 *
 * 顺序与用户给定列表一致, 即菜单从上到下:
 *   网易 / 九游 / vivo / 华为 / 4399 / 应用宝 / 荣耀 / 快手 /
 *   哔哩哔哩 / 国际服 / 华为国际服 / 测试服
 *
 * v8.1-fix2: 新增 pkg_name 列, 来源是 Star Android 项目
 * MainActivity.getAppNameByPackage() 里硬编码的一一对应. 这一列用于:
 *   - NDEF AAR (Android Application Record) 注入 ── 见 sky_aar_inject.c
 *     手机贴卡读到 AAR 后会直接打开对应渠道, 不再让系统去猜应用.
 *
 * v8.2-fix6: 增加快手渠道, 位置紧跟荣耀, 包名 com.netease.sky.kuaishou.
 */
#include "star_channels.h"
#include <string.h>

const star_channel_t star_channels[STAR_CH_COUNT] = {
    { STAR_CH_NETEASE,       "netease",       _L_STAR_CH_NETEASE,       "com.netease.sky"                  },
    { STAR_CH_9GAME,         "9game",         _L_STAR_CH_9GAME,         "com.netease.sky.aligames"         },
    { STAR_CH_VIVO,          "vivo",          _L_STAR_CH_VIVO,          "com.netease.sky.vivo"             },
    { STAR_CH_HUAWEI,        "huawei",        _L_STAR_CH_HUAWEI,        "com.netease.sky.huawei"           },
    { STAR_CH_4399,          "4399",          _L_STAR_CH_4399,          "com.netease.sky.m4399"            },
    { STAR_CH_YYB,           "yyb",           _L_STAR_CH_YYB,           "com.tencent.tmgp.eyou.eygy"       },
    { STAR_CH_HONOR,         "honor",         _L_STAR_CH_HONOR,         "com.netease.sky.honor"            },
    { STAR_CH_KUAISHOU,      "kuaishou",      _L_STAR_CH_KUAISHOU,      "com.netease.sky.kuaishou"         },
    { STAR_CH_BILIBILI,      "bilibili",      _L_STAR_CH_BILIBILI,      "com.netease.sky.bilibili"         },
    { STAR_CH_GLOBAL,        "global",        _L_STAR_CH_GLOBAL,        "com.tgc.sky.android"              },
    { STAR_CH_HUAWEI_GLOBAL, "huawei_global", _L_STAR_CH_HUAWEI_GLOBAL, "com.tgc.sky.android.huawei"       },
    { STAR_CH_TEST,          "test",          _L_STAR_CH_TEST,          "com.tgc.sky.android.test.gold"    },
};

const star_channel_t* star_channels_get(uint32_t idx) {
    if (idx >= STAR_CH_COUNT) return NULL;
    return &star_channels[idx];
}

const star_channel_t* star_channels_find_by_dir(const char *dir_name) {
    if (dir_name == NULL) return NULL;
    for (uint32_t i = 0; i < STAR_CH_COUNT; i++) {
        if (strcmp(star_channels[i].dir_name, dir_name) == 0) {
            return &star_channels[i];
        }
    }
    return NULL;
}
