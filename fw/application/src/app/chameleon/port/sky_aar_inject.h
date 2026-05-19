/*
 * sky_aar_inject.h  (v8.1-fix2 新增)
 *
 * "按账号自动打开应用" —— 在被仿真的 NTAG 内存里, 给 NDEF 消息追加一条
 * Android Application Record (AAR), 让手机贴卡时直接拉起对应渠道的
 * Sky 光遇客户端, 不再依赖系统 NDEF dispatcher 去问用户挑应用.
 *
 * 一句话: 把
 *
 *     [URI: "https://.../?s=..."]
 *
 * 改写成
 *
 *     [URI: "https://.../?s=..."]   ── ME=0
 *     [AAR: "com.netease.sky"]      ── ME=1
 *
 * 整个 NDEF Message 仍然在 TLV 里 (T=03), 长度字段也会改对.
 *
 * 模块只**在内存里**改写传入的 ntag_t* (用于 ntag_emu_set_tag 之前),
 * **不**碰磁盘上的 .bin 文件. 这样既能让仿真出去的卡带上 AAR,
 * 又不会污染用户备份的原始 dump.
 *
 * 设计参考:
 *   - NFC Forum NDEF Record Format spec
 *   - NFC Forum AAR (External Type "android.com:pkg")
 *   - Star Android 项目 NfcReaderWriter.writeToTag() 写卡时怎么塞 AAR
 *
 *  ─ 光遇徽章定制版 (Sky Badge Edition) v8.1-fix2 - sky_aar_inject ─
 */
#ifndef SKY_AAR_INJECT_H
#define SKY_AAR_INJECT_H

#include "ntag_def.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 在 ntag 的 NDEF Message 里追加一条 AAR 记录.
 *
 * 行为:
 *   1) 在 ntag->data[] 的用户区 (page 4 起) 找到 TLV T=03 (NDEF Message);
 *   2) 解析消息里第一条 record, 拿到它的 length;
 *   3) 在它后面追加一条 AAR record (TNF=External, type="android.com:pkg",
 *      payload = pkg_name);
 *   4) 第一条 record 的 ME (Message End) bit 清 0, 新追加的 AAR 的 ME 置 1;
 *   5) TLV 长度字段重写, terminator 0xFE 后移.
 *
 * 失败回退:
 *   - 没找到 NDEF TLV / 没有 URI record / 容量不够 → 直接返回 false,
 *     ntag->data[] 不动. 调用方应当继续用原始 ntag 仿真, 至少 URL
 *     还能让手机弹"打开方式".
 *
 * 不会 free 也不会 alloc, 全部就地改写.
 *
 * @param p_ntag      NTAG dump (ntag->data 必须是已加载的 NTAG 内存映像).
 *                    本函数会原地改写 ntag->data[] 的用户区.
 * @param pkg_name    要 inject 的 Android 包名 (ASCII \0 结尾).
 *                    传 NULL 或空串 → 函数立即返回 false, 不改写.
 * @return            true = 成功追加 AAR 且重写了 TLV;
 *                    false = 任意一步失败, ntag 未改动.
 */
bool sky_aar_inject_into_ntag(ntag_t *p_ntag, const char *pkg_name);

#ifdef __cplusplus
}
#endif

#endif /* SKY_AAR_INJECT_H */
