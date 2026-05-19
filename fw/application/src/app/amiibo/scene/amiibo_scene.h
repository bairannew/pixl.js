#ifndef AMIIBO_SCENE_H
#define AMIIBO_SCENE_H

#include "mui_scene_dispatcher.h"
#include "app_amiibo.h"

// Generate scene id and total number
#define ADD_SCENE(prefix, name, id) AMIIBO_SCENE_##id,
typedef enum {
#include "amiibo_scene_config.h"
    AMIIBO_SCENE_MAX,
} amiibo_scene_id_t;
#undef ADD_SCENE

extern const mui_scene_t amiibo_scene_defines[];

/* ============================================================ */
/*  v8.1 跨场景刷新接口                                          */
/*                                                              */
/*  badge_list 上点 "新建徽章" -> 占位 + 挂 BLE hook + 跳转       */
/*  AMIIBO_DETAIL. 当 BLE 写入到达, hook callback 把文件 rename  */
/*  之后, 如果当前还停在 AMIIBO_DETAIL, 就调用下面这个函数让      */
/*  AMIIBO_DETAIL 重新加载新文件名, 并触发 UI 重绘.              */
/*  返回值: true=刷新成功, false=参数无效或读盘失败.             */
/* ============================================================ */
bool amiibo_scene_amiibo_detail_refresh(app_amiibo_t *app,
                                        const char *new_file_name);

/* ============================================================ */
/*  v8.1-fix2 共用仿真入口                                       */
/*                                                              */
/*  在 ntag_emu_set_tag 之前根据 app->current_folder 解析渠道,    */
/*  在副本上注入 Android Application Record (AAR), 让手机贴卡    */
/*  时直接拉起对应渠道的 Sky 光遇客户端.                          */
/*                                                              */
/*  - 不动 clean_ntag 本身, 拷贝到模块内部静态缓冲再注入,         */
/*    所以 set UID / rand UID / read-only toggle 路径写回磁盘     */
/*    的内容都是干净的.                                          */
/*  - 渠道识别失败 / AAR 注入失败 => 回退到无 AAR 的仿真,         */
/*    URL 仍然能被系统 NDEF dispatcher 接住. 行为完全兼容.        */
/*                                                              */
/*  请凡是 "把当前 ntag 投到仿真器" 的地方一律走这个函数,         */
/*  不要直接 ntag_emu_set_tag —— 否则 AAR 不会被注入.            */
/* ============================================================ */
void amiibo_scene_amiibo_detail_emit_tag(app_amiibo_t *app, ntag_t *clean_ntag);

#endif
