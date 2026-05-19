/*
 * sky_aar_inject.c  (v8.1-fix2 新增)
 *
 * 实现: 把一条 Android Application Record 追加到 NTAG 用户区的 NDEF 消息后面.
 *
 * NDEF 在 NTAG 用户区的物理布局 (NFC Forum NDEF v1 / Type 2 Tag Operation):
 *
 *   page 4..N: NDEF TLV
 *     03 LL  <message bytes...>  FE
 *     ──────────────────────────────
 *     T=03   LL=short_len 或 0xFF + 2 字节大端长形式
 *
 *   Message 里面就是一条接一条的 record. 每条 record 头一个字节是 flags:
 *
 *     bit7 MB  (Message Begin)
 *     bit6 ME  (Message End)
 *     bit5 CF  (Chunk)             —— 本函数永远 = 0
 *     bit4 SR  (Short Record)      —— 本函数生成的 AAR 一定 SR=1
 *     bit3 IL  (ID Length present) —— 本函数永远 = 0
 *     bit2..0 TNF (Type Name Format)
 *       0x01 = NFC Well-Known   ("U" URI record 走这个)
 *       0x04 = External         (AAR 走这个, type 串 "android.com:pkg")
 *
 *   一条标准 SR record 的字节序:
 *     [header(1)] [type_len(1)] [payload_len(1)] [type...] [payload...]
 *
 *   AAR 的 type 永远是固定的 15 字节: "android.com:pkg"
 *
 * 本模块只动用户区, 不动 page 0..3 (UID/lock/CC), 也不动 NTAG 尾部
 * 的 dynamic lock / cfg pages. NTAG215 用户区 36 字节起 (page 4) 到
 * 130 字节结束 (page 129), 共 504 字节 (126 页). 留 50 字节给原 URL,
 * 还能塞 ~30 字节 AAR + terminator + meta —— 够用.
 *
 *  ─ 光遇徽章定制版 (Sky Badge Edition) v8.1-fix2 - sky_aar_inject ─
 */
#include "sky_aar_inject.h"

#include "nrf_log.h"
#include <string.h>

/* NTAG 用户区起点 = page 4, 即 byte offset 16. */
#define USER_AREA_OFFSET   16

/* 用户区结束 offset. NTAG215 走 .data[540], 但实际尾部 page 是 cfg /
 * dynamic lock, 不应该写. 保守取到 page 129 = byte 520. */
#define USER_AREA_END      520

/* AAR record 固定 type. 这是 Android NDEF 规约里硬编码的, 不能改. */
static const char k_aar_type[] = "android.com:pkg";
#define AAR_TYPE_LEN       15   /* strlen(k_aar_type) */

/* Header bit definitions */
#define NDEF_FLAG_MB       0x80
#define NDEF_FLAG_ME       0x40
#define NDEF_FLAG_SR       0x10
#define NDEF_TNF_EXTERNAL  0x04

bool sky_aar_inject_into_ntag(ntag_t *p_ntag, const char *pkg_name) {
    if (p_ntag == NULL || pkg_name == NULL || pkg_name[0] == '\0') {
        return false;
    }
    size_t pkg_len = strlen(pkg_name);
    if (pkg_len > 255) {
        /* SR (short record) payload_len 是 1 字节, 包名一定 < 256. */
        NRF_LOG_WARNING("sky_aar_inject: pkg_name too long: %u", (unsigned)pkg_len);
        return false;
    }

    uint8_t *mem = p_ntag->data;
    size_t total = _ntag_data_size(p_ntag);
    if (total <= USER_AREA_OFFSET) return false;
    if (total > USER_AREA_END) total = USER_AREA_END;

    /* ── 1. 找 NDEF TLV (T=0x03) ────────────────────────────────────────
     * NTAG 用户区第一个有意义的 TLV 不一定就是 03; 可能前面有
     * Lock Control TLV (0x01) 或 Memory Control TLV (0x02), 长度都是 1
     * 字节(总是 03 + LL=03 + 三字节 payload). 我们跳过这些, 找到 03 即停.
     */
    size_t tlv_off = USER_AREA_OFFSET;
    while (tlv_off < total - 2) {
        uint8_t tag = mem[tlv_off];
        if (tag == 0x03) break;       /* 找到 NDEF Message TLV */
        if (tag == 0x00) { tlv_off++; continue; }    /* NULL TLV, 跳 */
        if (tag == 0xFE) {            /* Terminator, 后面没东西了 */
            NRF_LOG_WARNING("sky_aar_inject: no NDEF TLV before terminator");
            return false;
        }
        /* 其它 TLV (0x01 / 0x02): 头 1 + 长度 1 + payload */
        uint8_t ll = mem[tlv_off + 1];
        size_t skip = (ll == 0xFF)
            ? (4 + (((size_t)mem[tlv_off + 2] << 8) | mem[tlv_off + 3]))
            : (2 + ll);
        tlv_off += skip;
    }
    if (tlv_off >= total - 2 || mem[tlv_off] != 0x03) {
        NRF_LOG_WARNING("sky_aar_inject: NDEF TLV not found");
        return false;
    }

    /* ── 2. 读 TLV 长度 ─────────────────────────────────────────────── */
    size_t tlv_hdr_len;       /* 03 + LL[1 or 3] */
    size_t msg_len;
    if (mem[tlv_off + 1] != 0xFF) {
        tlv_hdr_len = 2;
        msg_len = mem[tlv_off + 1];
    } else {
        tlv_hdr_len = 4;
        msg_len = ((size_t)mem[tlv_off + 2] << 8) | mem[tlv_off + 3];
    }
    size_t msg_off = tlv_off + tlv_hdr_len;
    if (msg_off + msg_len > total) {
        NRF_LOG_WARNING("sky_aar_inject: bad TLV length %u (msg_off=%u, total=%u)",
                        (unsigned)msg_len, (unsigned)msg_off, (unsigned)total);
        return false;
    }
    if (msg_len < 1) {
        return false;     /* 空消息, 没东西可改 */
    }

    /* ── 3. 走 record 链表, 找最后一条 (ME=1 那个), 准备把它的 ME 改成 0,
     *     然后在它后面塞 AAR. 同时全程校验 chunk 标志: 本模块只支持
     *     无 chunk 的简单消息(光遇徽章 URL 就是这种). ─────────────── */
    size_t cur = msg_off;
    size_t end = msg_off + msg_len;
    size_t last_hdr_off = 0;
    bool   last_hdr_seen = false;
    while (cur < end) {
        uint8_t hdr = mem[cur];
        if (hdr & 0x20) {   /* CF chunked → 不支持 */
            NRF_LOG_WARNING("sky_aar_inject: chunked record, skip");
            return false;
        }
        bool sr = (hdr & NDEF_FLAG_SR) != 0;
        bool il = (hdr & 0x08) != 0;
        if (cur + 1 >= end) break;
        size_t type_len = mem[cur + 1];
        size_t payload_len;
        size_t hdr_total;
        if (sr) {
            if (cur + 2 >= end) break;
            payload_len = mem[cur + 2];
            hdr_total = 3 + (il ? 1 : 0);
        } else {
            if (cur + 5 >= end) break;
            payload_len = ((size_t)mem[cur + 2] << 24)
                        | ((size_t)mem[cur + 3] << 16)
                        | ((size_t)mem[cur + 4] << 8)
                        |  (size_t)mem[cur + 5];
            hdr_total = 6 + (il ? 1 : 0);
        }
        size_t id_len = il ? mem[cur + (sr ? 3 : 6)] : 0;
        size_t rec_total = hdr_total + type_len + id_len + payload_len;
        last_hdr_off = cur;
        last_hdr_seen = true;
        if (hdr & NDEF_FLAG_ME) {
            /* 这就是末条 record */
            break;
        }
        cur += rec_total;
    }
    if (!last_hdr_seen) {
        NRF_LOG_WARNING("sky_aar_inject: no records parsed");
        return false;
    }

    /* ── 4. 计算 AAR record 的实际字节长度 ────────────────────────────
     *   header(1) + type_len(1) + payload_len(1) + type(15) + payload(pkg_len)
     */
    size_t aar_len = 1 + 1 + 1 + AAR_TYPE_LEN + pkg_len;

    /* 新的 message 长度 = 原长度 + aar_len. TLV 头形式可能从 short
     * (LL=1 byte) 变成 long (0xFF + 2 byte). 老的还是 short 的情况下,
     * 新长度 >= 0xFF 也必须变 long. 我们这里只在 short → short 时
     * 直接改 LL 字节; short → long 需要把整个 TLV 后续往后挤 2 字节,
     * 这是个略复杂的边缘情形, 而光遇徽章 URL + AAR 加起来一般 ~70 字节,
     * 走不到 long form. 真到 long form 直接返回 false 让上层放弃 inject. */
    size_t new_msg_len = msg_len + aar_len;
    if (tlv_hdr_len == 2 && new_msg_len >= 0xFF) {
        NRF_LOG_WARNING("sky_aar_inject: TLV would overflow short form, abort");
        return false;
    }
    /* 边界: TLV 末尾 (msg_off + new_msg_len) + 1 byte terminator
     *       <= total (用户区上限).   */
    if (msg_off + new_msg_len + 1 > total) {
        NRF_LOG_WARNING("sky_aar_inject: not enough room for AAR (need %u, have %u)",
                        (unsigned)(msg_off + new_msg_len + 1), (unsigned)total);
        return false;
    }

    /* ── 5. 改写最后一条 record 的 ME 位为 0 ──────────────────────── */
    mem[last_hdr_off] &= (uint8_t)~NDEF_FLAG_ME;

    /* ── 6. 在原 message 末尾 (msg_off + msg_len) 处写 AAR record ──── */
    uint8_t *p = mem + msg_off + msg_len;
    /* header: ME=1, SR=1, TNF=External(0x04). 第一条 record 仍然保留 MB=1, 不动它. */
    *p++ = (uint8_t)(NDEF_FLAG_ME | NDEF_FLAG_SR | NDEF_TNF_EXTERNAL);
    *p++ = (uint8_t)AAR_TYPE_LEN;
    *p++ = (uint8_t)pkg_len;
    memcpy(p, k_aar_type, AAR_TYPE_LEN);   p += AAR_TYPE_LEN;
    memcpy(p, pkg_name,   pkg_len);        p += pkg_len;

    /* ── 7. 写 terminator 0xFE ────────────────────────────────────── */
    *p = 0xFE;

    /* ── 8. 改写 TLV 长度字段 (short form 已经在第 4 步排除了溢出) ── */
    mem[tlv_off + 1] = (uint8_t)new_msg_len;

    NRF_LOG_INFO("sky_aar_inject: appended AAR pkg=%s (msg_len %u -> %u)",
                 nrf_log_push((char *)pkg_name),
                 (unsigned)msg_len, (unsigned)new_msg_len);
    return true;
}
