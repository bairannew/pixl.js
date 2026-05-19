/*
 * sky_badge_parser.c
 *
 * 实现见 sky_badge_parser.h.
 *
 * NDEF TLV / URI Record 结构来自 NFC Forum NDEF spec, 这里只做最小可用解析:
 *
 *   NTAG 用户区 (page 4 起) 一般是
 *       03 LL  D1 01 LL2 55  PROTO_PREFIX  url_body...  FE
 *       ──    ─────────────────────────────────────  ──
 *       TLV   一条 NDEF Record (Short / MB|ME|SR)     Terminator
 *       T=03 = NDEF Message
 *       LL    = NDEF Message 长度 (短形式 1 字节; 0xFF 时跟 2 字节大端)
 *
 *   Record header byte 0xD1 = MB=1 ME=1 CF=0 SR=1 IL=0 TNF=001 (Well-Known)
 *   后面 01 = Type Length, LL2 = Payload Length, 55 = 'U' (URI)
 *   URI payload 第 1 字节是 protocol prefix code (0x04 = https://, etc.)
 *
 * 简化策略:
 *   不严格校验 record header 的各 bit, 只要找到 TLV T=03, 跳过 LL,
 *   再扫到一个 type byte 'U' 0x55, 取它前一字节当 type length,
 *   再读 payload length, 再读 1 字节 protocol prefix, 后续就是 url body.
 *   这样能兼容多 record / 长形式 TLV.
 *
 *  ─ 光遇徽章定制版 (Sky Badge Edition) - sky_badge_parser ─
 */
#include "sky_badge_parser.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

/* ============================================================
 *  NDEF URI prefix table (NFC Forum URI RTD spec, 36 个).
 *  payload[0] 取这一字节, 拼到 url_out 前面.
 * ============================================================ */
static const char *const k_uri_prefix[] = {
    "",                                 /* 0x00 */
    "http://www.",                      /* 0x01 */
    "https://www.",                     /* 0x02 */
    "http://",                          /* 0x03 */
    "https://",                         /* 0x04 */
    "tel:",                             /* 0x05 */
    "mailto:",                          /* 0x06 */
    "ftp://anonymous:anonymous@",       /* 0x07 */
    "ftp://ftp.",                       /* 0x08 */
    "ftps://",                          /* 0x09 */
    "sftp://",                          /* 0x0A */
    "smb://",                           /* 0x0B */
    "nfs://",                           /* 0x0C */
    "ftp://",                           /* 0x0D */
    "dav://",                           /* 0x0E */
    "news:",                            /* 0x0F */
    "telnet://",                        /* 0x10 */
    "imap:",                            /* 0x11 */
    "rtsp://",                          /* 0x12 */
    "urn:",                             /* 0x13 */
    "pop:",                             /* 0x14 */
    "sip:",                             /* 0x15 */
    "sips:",                            /* 0x16 */
    "tftp:",                            /* 0x17 */
    "btspp://",                         /* 0x18 */
    "btl2cap://",                       /* 0x19 */
    "btgoep://",                        /* 0x1A */
    "tcpobex://",                       /* 0x1B */
    "irdaobex://",                      /* 0x1C */
    "file://",                          /* 0x1D */
    "urn:epc:id:",                      /* 0x1E */
    "urn:epc:tag:",                     /* 0x1F */
    "urn:epc:pat:",                     /* 0x20 */
    "urn:epc:raw:",                     /* 0x21 */
    "urn:epc:",                         /* 0x22 */
    "urn:nfc:",                         /* 0x23 */
};
#define K_URI_PREFIX_COUNT (sizeof(k_uri_prefix) / sizeof(k_uri_prefix[0]))

/* NTAG: page 0..3 = UID/lock/CC. 用户区 NDEF TLV 从 byte 16 (page 4) 起. */
#define NTAG_USER_AREA_OFFSET 16

/* ============================================================
 *  PART 1 : NTAG memory → URL
 * ============================================================ */
bool sky_badge_parse_url_from_ntag(const uint8_t *mem, size_t size,
                                   char *url_out, size_t url_max) {
    if (mem == NULL || url_out == NULL || url_max < 2) {
        return false;
    }
    url_out[0] = '\0';

    if (size <= NTAG_USER_AREA_OFFSET + 4) {
        return false;
    }

    const uint8_t *p   = mem + NTAG_USER_AREA_OFFSET;
    const uint8_t *end = mem + size;

    /* --- 找 TLV T=0x03 (NDEF Message) -------------------------- */
    while (p < end) {
        uint8_t t = *p;
        if (t == 0xFE) {                /* Terminator TLV: 没找到 */
            return false;
        }
        if (t == 0x00) {                /* Null TLV: 单字节, 跳过 */
            p++;
            continue;
        }
        if (p + 1 >= end) return false;
        /* TLV length: 1 字节; 若 == 0xFF, 后接 2 字节大端长形式 */
        uint32_t tlv_len;
        const uint8_t *value;
        if (p[1] != 0xFF) {
            tlv_len = p[1];
            value   = p + 2;
        } else {
            if (p + 3 >= end) return false;
            tlv_len = ((uint32_t)p[2] << 8) | p[3];
            value   = p + 4;
        }
        if (value + tlv_len > end) return false;

        if (t == 0x03) {
            /* 命中 NDEF Message. 在 [value, value+tlv_len) 里找 URI Record. */
            const uint8_t *rp = value;
            const uint8_t *rend = value + tlv_len;
            /*
             * 在 Record header (1 byte) + Type Length (1 byte) + Payload Length
             * (1 or 4 bytes, SR bit 决定) + [ID Length (IL bit)] + Type bytes
             * + [ID bytes] + Payload bytes 这样的 record 序列里, 找 type == 'U'.
             */
            while (rp + 3 < rend) {
                uint8_t hdr        = rp[0];
                uint8_t type_len   = rp[1];
                bool    sr         = (hdr & 0x10) != 0;
                bool    il         = (hdr & 0x08) != 0;
                uint32_t payload_len;
                const uint8_t *pp  = rp + 2;
                if (sr) {
                    payload_len = *pp;
                    pp += 1;
                } else {
                    if (pp + 4 > rend) return false;
                    payload_len = ((uint32_t)pp[0] << 24) | ((uint32_t)pp[1] << 16) |
                                  ((uint32_t)pp[2] << 8)  |  (uint32_t)pp[3];
                    pp += 4;
                }
                uint8_t id_len = 0;
                if (il) {
                    if (pp >= rend) return false;
                    id_len = *pp;
                    pp += 1;
                }
                if (pp + type_len + id_len + payload_len > rend) return false;

                const uint8_t *type_bytes    = pp;
                const uint8_t *payload_bytes = pp + type_len + id_len;

                /* 看是不是 type == 'U' && TNF == 0x01 (Well-Known) */
                uint8_t tnf = hdr & 0x07;
                if (tnf == 0x01 && type_len == 1 && type_bytes[0] == 0x55 && payload_len >= 1) {
                    /* 取 prefix + 剩余 body 拼成完整 URL */
                    uint8_t code = payload_bytes[0];
                    const char *prefix = (code < K_URI_PREFIX_COUNT) ? k_uri_prefix[code] : "";
                    size_t plen = strlen(prefix);
                    size_t blen = payload_len - 1;
                    if (plen + blen + 1 > url_max) {
                        /* 缓冲区不够, 截断保存可见部分 */
                        if (plen >= url_max) return false;
                        blen = url_max - 1 - plen;
                    }
                    memcpy(url_out, prefix, plen);
                    memcpy(url_out + plen, payload_bytes + 1, blen);
                    url_out[plen + blen] = '\0';

                    /* 同时把 URL 体里非 ASCII 字节清掉, 防止越界字符干扰后续匹配 */
                    for (size_t i = 0; i < plen + blen; i++) {
                        if ((unsigned char)url_out[i] < 0x20 || (unsigned char)url_out[i] > 0x7E) {
                            url_out[i] = '\0';
                            break;
                        }
                    }
                    return url_out[0] != '\0';
                }

                /* 进入下一条 record */
                rp = pp + type_len + id_len + payload_len;

                /* ME bit (0x40) 表示这是 Message 的最后一条; 命中即停 */
                if (hdr & 0x40) {
                    break;
                }
            }
            /* NDEF Message 里没找到 URI Record */
            return false;
        }
        /* 不是 0x03, 跳过这个 TLV 继续找 */
        p = value + tlv_len;
    }
    return false;
}

/* ============================================================
 *  PART 2 : Base64 解码 (RFC 4648 标准字母表, 兼容 url-safe).
 *  小巧实现, 不依赖外部库, 不带 alloc.
 * ============================================================ */
static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* dst_cap 是目标缓冲区容量(含 \0); 返回真实写入的字节数(不含 \0), 失败返回 -1. */
static int b64_decode(const char *src, size_t src_len, char *dst, size_t dst_cap) {
    if (dst_cap == 0) return -1;
    size_t out = 0;
    int buf = 0;
    int bits = 0;
    for (size_t i = 0; i < src_len; i++) {
        char c = src[i];
        if (c == '=' || c == '\0') break;
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        int v = b64_val(c);
        if (v < 0) return -1;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out + 1 >= dst_cap) return -1;
            dst[out++] = (char)((buf >> bits) & 0xFF);
        }
    }
    dst[out] = '\0';
    return (int)out;
}

/* ============================================================
 *  小工具: 在字符串里找 "key=" 后取到下一个 '&' 或末尾的子串.
 *  与 Java 的正则 [?&]key=([^&]+) 等价.
 *  返回写入 out 的字节数 (不含 \0), 没找到返回 -1.
 * ============================================================ */
static int extract_query_value(const char *src, const char *key,
                               char *out, size_t out_cap) {
    if (src == NULL || key == NULL || out == NULL || out_cap < 2) return -1;
    size_t klen = strlen(key);
    const char *p = src;
    while ((p = strstr(p, key)) != NULL) {
        /* 前面必须是 ? 或 & 或字符串开头, 防止部分匹配 (例如 ssk=) */
        if (p == src || *(p - 1) == '?' || *(p - 1) == '&') {
            if (p[klen] == '=') {
                const char *vstart = p + klen + 1;
                const char *vend = vstart;
                while (*vend && *vend != '&' && *vend != '#') vend++;
                size_t vlen = (size_t)(vend - vstart);
                if (vlen + 1 > out_cap) vlen = out_cap - 1;
                memcpy(out, vstart, vlen);
                out[vlen] = '\0';
                return (int)vlen;
            }
        }
        p += klen;
    }
    return -1;
}

/* ============================================================
 *  PART 3 : URL → sk
 * ============================================================ */
bool sky_badge_parse_sk_from_url(const char *url, char *sk_out, size_t sk_max) {
    if (url == NULL || sk_out == NULL || sk_max < 2) return false;
    sk_out[0] = '\0';

    /* 第一步: 抓 s=BASE64 */
    char s_param[384];
    int slen = extract_query_value(url, "s", s_param, sizeof(s_param));
    if (slen <= 0) return false;

    /* 第二步: base64 解码 */
    char decoded[288];
    int dlen = b64_decode(s_param, (size_t)slen, decoded, sizeof(decoded));
    if (dlen <= 0) return false;

    /*
     * 解码后的串不一定以 ? 开头. 例子: 可能是 "abc?sk=SKY-...&...".
     * 为了让 extract_query_value 能匹配开头, 给它加一个虚 '?' 前缀.
     */
    char with_q[300];
    with_q[0] = '?';
    size_t cp = (size_t)dlen;
    if (cp > sizeof(with_q) - 2) cp = sizeof(with_q) - 2;
    memcpy(with_q + 1, decoded, cp);
    with_q[1 + cp] = '\0';

    int klen = extract_query_value(with_q, "sk", sk_out, sk_max);
    return klen > 0;
}

/* ============================================================
 *  PART 4 : 一站式封装
 * ============================================================ */
const sky_badge_entry_t *sky_badge_try_autofill(const uint8_t *mem, size_t size) {
    char url[SKY_BADGE_URL_MAX];
    if (!sky_badge_parse_url_from_ntag(mem, size, url, sizeof(url))) {
        return NULL;
    }
    char sk[SKY_BADGE_SK_MAX];
    if (!sky_badge_parse_sk_from_url(url, sk, sizeof(sk))) {
        return NULL;
    }
    return sky_badge_db_find(sk);
}
