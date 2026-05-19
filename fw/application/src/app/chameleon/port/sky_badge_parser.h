/*
 * sky_badge_parser.h
 *
 * 把一段刚加载到当前卡槽内存里的 NTAG dump 数据,
 * 解析成 (URL → base64 解码 → sk → 徽章条目) 链路,
 * 用来自动识别这是哪一枚光遇实体徽章.
 *
 * 设计参考 Star Android 项目 (brr.star.sky.app.tool.BadgeIdentifier):
 *
 *   URL 形如   https://xxx/?s=BASE64
 *   base64 解码后字符串里含  ?sk=SKY-XX-XX-XX-XX&...
 *
 * 本模块负责:
 *   1) sky_badge_parse_url_from_ntag : NTAG 内存 → NDEF URI record → URL 字符串
 *   2) sky_badge_parse_sk_from_url   : URL → base64 → sk
 *   3) sky_badge_try_autofill        : 一站式封装, 返回命中的 sky_badge_entry_t*
 *
 *  ─ 光遇徽章定制版 (Sky Badge Edition) - sky_badge_parser ─
 */
#ifndef SKY_BADGE_PARSER_H
#define SKY_BADGE_PARSER_H

#include "sky_badge_db.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 解析出来的 URL 最长长度 (含 \0). 光遇实体徽章实测 ~70 字节. 留 256 富裕. */
#define SKY_BADGE_URL_MAX   256

/** sk 字符串最长长度 (含 \0). 现存最长 sk 22 字符, 留 64 富裕. */
#define SKY_BADGE_SK_MAX    64

/**
 * 在一段 NTAG 内存里搜 NDEF TLV (block 4 起), 找到第一条 URI Record,
 * 拼出完整 URL (带协议前缀) 写到 url_out.
 *
 * @param mem        NTAG memory[] 起始指针, 调用方应当传 memory[0] 而不是 [4],
 *                   因为 page 0..3 是 UID/lock/CC; 实际 NDEF TLV 从 page 4 起.
 *                   我们内部跳过前 16 字节 = 4 页.
 * @param size       mem 实际可读字节数 (页数 * 4).
 * @param url_out    输出缓冲区.
 * @param url_max    url_out 容量 (>= SKY_BADGE_URL_MAX 推荐).
 * @return           true = 找到一条 URI Record 且写入了 url_out;
 *                   false = 没找到 / 数据残缺 / 缓冲区太小.
 */
bool sky_badge_parse_url_from_ntag(const uint8_t *mem, size_t size,
                                   char *url_out, size_t url_max);

/**
 * 从一条 URL 里抽 ?s=BASE64, 解码, 再从结果里抽 sk=XXX.
 *
 * 与 Java BadgeIdentifier.parseSk 行为一致:
 *   - 缺 s= → 返回 false
 *   - base64 解码失败 → 返回 false
 *   - 解码后缺 sk= → 返回 false
 *
 * @param url        输入 URL 字符串 (\0 结尾).
 * @param sk_out     输出 sk 字符串缓冲区.
 * @param sk_max     sk_out 容量 (>= SKY_BADGE_SK_MAX 推荐).
 * @return           true = 成功提取 sk; false = 任意一步失败.
 */
bool sky_badge_parse_sk_from_url(const char *url,
                                 char *sk_out, size_t sk_max);

/**
 * 一站式: NTAG dump → URL → sk → 数据库条目.
 *
 * @param mem        NTAG memory[][] 起始指针.
 * @param size       NTAG memory 总大小 (字节).
 * @return           命中: 返回 sky_badge_db_find() 给的指针; 未命中: NULL.
 *                   返回非 NULL 时, entry->name_zh / note_zh 可直接拿去写 nickname.
 */
const sky_badge_entry_t *sky_badge_try_autofill(const uint8_t *mem, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* SKY_BADGE_PARSER_H */
