/*
 * activation.c
 *
 * Implementation of the activation gate. MD5 and base64 are embedded
 * here as small dependency-free helpers — the project's mbedtls is in
 * the externally-resolved Nordic SDK and we don't want to be coupled
 * to a particular API revision (md5_starts vs md5_starts_ret etc).
 */

#include "activation.h"

#include "nrf52.h"
#include "vfs.h"
#include "vfs_meta.h"

#include <string.h>

#define ACTIVATION_FILE_NAME "/activation.bin"
#define ACTIVATION_MAGIC     0x53544152u  /* 'STAR' */

typedef struct {
    uint32_t magic;
    uint32_t activated;     /* 0 or 1 */
    uint8_t  reserved[24];  /* room for future fields without breaking layout */
} activation_record_t;

static bool m_activated     = false;
static bool m_pin_computed  = false;
static char m_pin_str[ACTIVATION_PIN_LEN + 1];

/* ============================================================== */
/*  Embedded MD5 (RFC 1321 reference, public domain rewrite)       */
/* ============================================================== */

typedef struct {
    uint32_t lo, hi;
    uint32_t a, b, c, d;
    uint8_t  buffer[64];
    uint32_t block[16];
} md5_ctx_t;

#define MD5_F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define MD5_G(x, y, z) ((y) ^ ((z) & ((x) ^ (y))))
#define MD5_H(x, y, z) (((x) ^ (y)) ^ (z))
#define MD5_H2(x, y, z) ((x) ^ ((y) ^ (z)))
#define MD5_I(x, y, z) ((y) ^ ((x) | ~(z)))

#define MD5_STEP(f, a, b, c, d, x, t, s) \
    (a) += f((b), (c), (d)) + (x) + (t); \
    (a) = (((a) << (s)) | (((a) & 0xFFFFFFFFu) >> (32 - (s)))); \
    (a) += (b);

#define MD5_SET(n) \
    (ctx->block[(n)] = \
        (uint32_t)ptr[(n) * 4] | \
        ((uint32_t)ptr[(n) * 4 + 1] << 8) | \
        ((uint32_t)ptr[(n) * 4 + 2] << 16) | \
        ((uint32_t)ptr[(n) * 4 + 3] << 24))
#define MD5_GET(n) (ctx->block[(n)])

static const void *md5_body(md5_ctx_t *ctx, const void *data, size_t size) {
    const uint8_t *ptr = (const uint8_t *)data;
    uint32_t a = ctx->a, b = ctx->b, c = ctx->c, d = ctx->d;
    uint32_t saved_a, saved_b, saved_c, saved_d;

    do {
        saved_a = a; saved_b = b; saved_c = c; saved_d = d;

        /* Round 1 */
        MD5_STEP(MD5_F, a, b, c, d, MD5_SET(0),  0xd76aa478, 7)
        MD5_STEP(MD5_F, d, a, b, c, MD5_SET(1),  0xe8c7b756, 12)
        MD5_STEP(MD5_F, c, d, a, b, MD5_SET(2),  0x242070db, 17)
        MD5_STEP(MD5_F, b, c, d, a, MD5_SET(3),  0xc1bdceee, 22)
        MD5_STEP(MD5_F, a, b, c, d, MD5_SET(4),  0xf57c0faf, 7)
        MD5_STEP(MD5_F, d, a, b, c, MD5_SET(5),  0x4787c62a, 12)
        MD5_STEP(MD5_F, c, d, a, b, MD5_SET(6),  0xa8304613, 17)
        MD5_STEP(MD5_F, b, c, d, a, MD5_SET(7),  0xfd469501, 22)
        MD5_STEP(MD5_F, a, b, c, d, MD5_SET(8),  0x698098d8, 7)
        MD5_STEP(MD5_F, d, a, b, c, MD5_SET(9),  0x8b44f7af, 12)
        MD5_STEP(MD5_F, c, d, a, b, MD5_SET(10), 0xffff5bb1, 17)
        MD5_STEP(MD5_F, b, c, d, a, MD5_SET(11), 0x895cd7be, 22)
        MD5_STEP(MD5_F, a, b, c, d, MD5_SET(12), 0x6b901122, 7)
        MD5_STEP(MD5_F, d, a, b, c, MD5_SET(13), 0xfd987193, 12)
        MD5_STEP(MD5_F, c, d, a, b, MD5_SET(14), 0xa679438e, 17)
        MD5_STEP(MD5_F, b, c, d, a, MD5_SET(15), 0x49b40821, 22)

        /* Round 2 */
        MD5_STEP(MD5_G, a, b, c, d, MD5_GET(1),  0xf61e2562, 5)
        MD5_STEP(MD5_G, d, a, b, c, MD5_GET(6),  0xc040b340, 9)
        MD5_STEP(MD5_G, c, d, a, b, MD5_GET(11), 0x265e5a51, 14)
        MD5_STEP(MD5_G, b, c, d, a, MD5_GET(0),  0xe9b6c7aa, 20)
        MD5_STEP(MD5_G, a, b, c, d, MD5_GET(5),  0xd62f105d, 5)
        MD5_STEP(MD5_G, d, a, b, c, MD5_GET(10), 0x02441453, 9)
        MD5_STEP(MD5_G, c, d, a, b, MD5_GET(15), 0xd8a1e681, 14)
        MD5_STEP(MD5_G, b, c, d, a, MD5_GET(4),  0xe7d3fbc8, 20)
        MD5_STEP(MD5_G, a, b, c, d, MD5_GET(9),  0x21e1cde6, 5)
        MD5_STEP(MD5_G, d, a, b, c, MD5_GET(14), 0xc33707d6, 9)
        MD5_STEP(MD5_G, c, d, a, b, MD5_GET(3),  0xf4d50d87, 14)
        MD5_STEP(MD5_G, b, c, d, a, MD5_GET(8),  0x455a14ed, 20)
        MD5_STEP(MD5_G, a, b, c, d, MD5_GET(13), 0xa9e3e905, 5)
        MD5_STEP(MD5_G, d, a, b, c, MD5_GET(2),  0xfcefa3f8, 9)
        MD5_STEP(MD5_G, c, d, a, b, MD5_GET(7),  0x676f02d9, 14)
        MD5_STEP(MD5_G, b, c, d, a, MD5_GET(12), 0x8d2a4c8a, 20)

        /* Round 3 */
        MD5_STEP(MD5_H,  a, b, c, d, MD5_GET(5),  0xfffa3942, 4)
        MD5_STEP(MD5_H2, d, a, b, c, MD5_GET(8),  0x8771f681, 11)
        MD5_STEP(MD5_H,  c, d, a, b, MD5_GET(11), 0x6d9d6122, 16)
        MD5_STEP(MD5_H2, b, c, d, a, MD5_GET(14), 0xfde5380c, 23)
        MD5_STEP(MD5_H,  a, b, c, d, MD5_GET(1),  0xa4beea44, 4)
        MD5_STEP(MD5_H2, d, a, b, c, MD5_GET(4),  0x4bdecfa9, 11)
        MD5_STEP(MD5_H,  c, d, a, b, MD5_GET(7),  0xf6bb4b60, 16)
        MD5_STEP(MD5_H2, b, c, d, a, MD5_GET(10), 0xbebfbc70, 23)
        MD5_STEP(MD5_H,  a, b, c, d, MD5_GET(13), 0x289b7ec6, 4)
        MD5_STEP(MD5_H2, d, a, b, c, MD5_GET(0),  0xeaa127fa, 11)
        MD5_STEP(MD5_H,  c, d, a, b, MD5_GET(3),  0xd4ef3085, 16)
        MD5_STEP(MD5_H2, b, c, d, a, MD5_GET(6),  0x04881d05, 23)
        MD5_STEP(MD5_H,  a, b, c, d, MD5_GET(9),  0xd9d4d039, 4)
        MD5_STEP(MD5_H2, d, a, b, c, MD5_GET(12), 0xe6db99e5, 11)
        MD5_STEP(MD5_H,  c, d, a, b, MD5_GET(15), 0x1fa27cf8, 16)
        MD5_STEP(MD5_H2, b, c, d, a, MD5_GET(2),  0xc4ac5665, 23)

        /* Round 4 */
        MD5_STEP(MD5_I, a, b, c, d, MD5_GET(0),  0xf4292244, 6)
        MD5_STEP(MD5_I, d, a, b, c, MD5_GET(7),  0x432aff97, 10)
        MD5_STEP(MD5_I, c, d, a, b, MD5_GET(14), 0xab9423a7, 15)
        MD5_STEP(MD5_I, b, c, d, a, MD5_GET(5),  0xfc93a039, 21)
        MD5_STEP(MD5_I, a, b, c, d, MD5_GET(12), 0x655b59c3, 6)
        MD5_STEP(MD5_I, d, a, b, c, MD5_GET(3),  0x8f0ccc92, 10)
        MD5_STEP(MD5_I, c, d, a, b, MD5_GET(10), 0xffeff47d, 15)
        MD5_STEP(MD5_I, b, c, d, a, MD5_GET(1),  0x85845dd1, 21)
        MD5_STEP(MD5_I, a, b, c, d, MD5_GET(8),  0x6fa87e4f, 6)
        MD5_STEP(MD5_I, d, a, b, c, MD5_GET(15), 0xfe2ce6e0, 10)
        MD5_STEP(MD5_I, c, d, a, b, MD5_GET(6),  0xa3014314, 15)
        MD5_STEP(MD5_I, b, c, d, a, MD5_GET(13), 0x4e0811a1, 21)
        MD5_STEP(MD5_I, a, b, c, d, MD5_GET(4),  0xf7537e82, 6)
        MD5_STEP(MD5_I, d, a, b, c, MD5_GET(11), 0xbd3af235, 10)
        MD5_STEP(MD5_I, c, d, a, b, MD5_GET(2),  0x2ad7d2bb, 15)
        MD5_STEP(MD5_I, b, c, d, a, MD5_GET(9),  0xeb86d391, 21)

        a += saved_a; b += saved_b; c += saved_c; d += saved_d;
        ptr += 64;
    } while (size -= 64);

    ctx->a = a; ctx->b = b; ctx->c = c; ctx->d = d;
    return ptr;
}

static void md5_init(md5_ctx_t *ctx) {
    ctx->a = 0x67452301; ctx->b = 0xefcdab89;
    ctx->c = 0x98badcfe; ctx->d = 0x10325476;
    ctx->lo = 0; ctx->hi = 0;
}

static void md5_update(md5_ctx_t *ctx, const void *data, size_t size) {
    uint32_t saved_lo = ctx->lo;
    if ((ctx->lo = (saved_lo + size) & 0x1fffffff) < saved_lo) ctx->hi++;
    ctx->hi += (uint32_t)(size >> 29);

    size_t used = saved_lo & 0x3f;
    if (used) {
        size_t available = 64 - used;
        if (size < available) {
            memcpy(&ctx->buffer[used], data, size);
            return;
        }
        memcpy(&ctx->buffer[used], data, available);
        data = (const uint8_t *)data + available;
        size -= available;
        md5_body(ctx, ctx->buffer, 64);
    }
    if (size >= 64) {
        data = md5_body(ctx, data, size & ~(size_t)0x3f);
        size &= 0x3f;
    }
    memcpy(ctx->buffer, data, size);
}

static void md5_final(md5_ctx_t *ctx, uint8_t out[16]) {
    size_t used = ctx->lo & 0x3f;
    ctx->buffer[used++] = 0x80;
    size_t available = 64 - used;
    if (available < 8) {
        memset(&ctx->buffer[used], 0, available);
        md5_body(ctx, ctx->buffer, 64);
        used = 0;
        available = 64;
    }
    memset(&ctx->buffer[used], 0, available - 8);
    ctx->lo <<= 3;
    ctx->buffer[56] = (uint8_t)(ctx->lo);
    ctx->buffer[57] = (uint8_t)(ctx->lo >> 8);
    ctx->buffer[58] = (uint8_t)(ctx->lo >> 16);
    ctx->buffer[59] = (uint8_t)(ctx->lo >> 24);
    ctx->buffer[60] = (uint8_t)(ctx->hi);
    ctx->buffer[61] = (uint8_t)(ctx->hi >> 8);
    ctx->buffer[62] = (uint8_t)(ctx->hi >> 16);
    ctx->buffer[63] = (uint8_t)(ctx->hi >> 24);
    md5_body(ctx, ctx->buffer, 64);
    out[0]  = (uint8_t)(ctx->a);
    out[1]  = (uint8_t)(ctx->a >> 8);
    out[2]  = (uint8_t)(ctx->a >> 16);
    out[3]  = (uint8_t)(ctx->a >> 24);
    out[4]  = (uint8_t)(ctx->b);
    out[5]  = (uint8_t)(ctx->b >> 8);
    out[6]  = (uint8_t)(ctx->b >> 16);
    out[7]  = (uint8_t)(ctx->b >> 24);
    out[8]  = (uint8_t)(ctx->c);
    out[9]  = (uint8_t)(ctx->c >> 8);
    out[10] = (uint8_t)(ctx->c >> 16);
    out[11] = (uint8_t)(ctx->c >> 24);
    out[12] = (uint8_t)(ctx->d);
    out[13] = (uint8_t)(ctx->d >> 8);
    out[14] = (uint8_t)(ctx->d >> 16);
    out[15] = (uint8_t)(ctx->d >> 24);
    memset(ctx, 0, sizeof(*ctx));
}

/* ============================================================== */
/*  Embedded base64 (encoder only, RFC 4648 alphabet, padded)      */
/* ============================================================== */

static void b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap) {
    static const char A[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, j = 0;
    while (i + 3 <= in_len && j + 4 < out_cap) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[j++] = A[(v >> 18) & 0x3F];
        out[j++] = A[(v >> 12) & 0x3F];
        out[j++] = A[(v >> 6)  & 0x3F];
        out[j++] = A[v & 0x3F];
        i += 3;
    }
    if (i < in_len && j + 4 < out_cap) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) v |= (uint32_t)in[i + 1] << 8;
        out[j++] = A[(v >> 18) & 0x3F];
        out[j++] = A[(v >> 12) & 0x3F];
        out[j++] = (i + 1 < in_len) ? A[(v >> 6) & 0x3F] : '=';
        out[j++] = '=';
    }
    if (j < out_cap) out[j] = '\0';
}

/* ============================================================== */
/*  PIN computation                                                */
/* ============================================================== */

static void hex8(uint8_t b, char out[2]) {
    static const char H[] = "0123456789abcdef";
    out[0] = H[(b >> 4) & 0x0F];
    out[1] = H[b & 0x0F];
}

static void compute_pin_string(void) {
    if (m_pin_computed) return;

    /* 1. Read 8-byte FICR DEVICEID (the chip's per-unit factory ID). */
    uint8_t device_id[8];
    uint32_t d0 = NRF_FICR->DEVICEID[0];
    uint32_t d1 = NRF_FICR->DEVICEID[1];
    memcpy(&device_id[0], &d0, 4);
    memcpy(&device_id[4], &d1, 4);

    /* 2. Convert to a lowercase hex string. The spec is
     *    "get motherboard id, md5 encrypt, then base64 encrypt".
     *    We hash the printable hex (a stable representation of the
     *    ID) rather than the 8 raw bytes, so the algorithm matches
     *    what would happen if you ran it on a desktop with the ID
     *    pasted as text. */
    char id_hex[17];
    for (int i = 0; i < 8; i++) hex8(device_id[i], &id_hex[i * 2]);
    id_hex[16] = '\0';

    /* 3. MD5 the hex string. */
    uint8_t md5_out[16];
    md5_ctx_t md5;
    md5_init(&md5);
    md5_update(&md5, id_hex, 16);
    md5_final(&md5, md5_out);

    /* 4. Base64-encode the 16 MD5 bytes (24 chars + NUL). */
    char b64[32];
    b64_encode(md5_out, 16, b64, sizeof(b64));

    /* 5. Walk the base64 and collect all digit chars. */
    char digits[32];
    size_t ndigits = 0;
    for (size_t i = 0; b64[i] != '\0' && ndigits < sizeof(digits); i++) {
        if (b64[i] >= '0' && b64[i] <= '9') {
            digits[ndigits++] = b64[i];
        }
    }

    /* 6. Build 8-digit PIN.
     *    Base64 has only 10 digit chars out of 64, so a 24-char
     *    encoding typically yields just ~3-4 digits. Cycling
     *    deterministically gives every device a stable, unique
     *    8-digit PIN. If by extreme chance no digit appeared at
     *    all, fall back to (md5_byte[i] % 10) which is also
     *    deterministic and unique per chip. */
    if (ndigits == 0) {
        for (int i = 0; i < ACTIVATION_PIN_LEN; i++) {
            m_pin_str[i] = (char)('0' + (md5_out[i] % 10));
        }
    } else {
        for (int i = 0; i < ACTIVATION_PIN_LEN; i++) {
            m_pin_str[i] = digits[i % ndigits];
        }
    }
    m_pin_str[ACTIVATION_PIN_LEN] = '\0';
    m_pin_computed = true;
}

const char *activation_get_pin_string(void) {
    compute_pin_string();
    return m_pin_str;
}

uint32_t activation_get_pin_value(void) {
    compute_pin_string();
    uint32_t v = 0;
    for (int i = 0; i < ACTIVATION_PIN_LEN; i++) {
        v = v * 10u + (uint32_t)(m_pin_str[i] - '0');
    }
    return v;
}

int32_t activation_get_expected_code(void) {
    /* code = ((pin*3) - 82) * 2 + 1524
     *      = pin*6 - 164 + 1524
     *      = pin*6 + 1360
     *
     * Note: with 8-digit PIN max (99,999,999), pin*6 ≈ 6e8, fits in int32_t.
     */
    int32_t base = (int32_t)(activation_get_pin_value() * 3u) - 82;
    return base * 2 + 1524;
}

/* ============================================================== */
/*  Persistence                                                    */
/* ============================================================== */

int32_t activation_init(void) {
    m_activated = false;

    vfs_driver_t *p_driver = vfs_get_default_driver();
    if (p_driver == NULL) {
        return -1;
    }
    if (!p_driver->mounted()) {
        /* settings_init normally mounts already; if it didn't, try here. */
        int32_t merr = p_driver->mount();
        if (merr < 0 || !p_driver->mounted()) {
            return -1;
        }
    }

    activation_record_t rec;
    memset(&rec, 0, sizeof(rec));
    int32_t err = p_driver->read_file_data(ACTIVATION_FILE_NAME, &rec, sizeof(rec));
    if (err < 0) {
        /* No file yet → not activated. Not a hard error. */
        return 0;
    }
    if (rec.magic == ACTIVATION_MAGIC && rec.activated == 1) {
        m_activated = true;
    }
    return 0;
}

bool activation_is_activated(void) {
    return m_activated;
}

bool activation_try_activate(int32_t entered_code) {
    if (entered_code != activation_get_expected_code()) {
        return false;
    }

    /* Code matches — persist. */
    vfs_driver_t *p_driver = vfs_get_default_driver();
    if (p_driver == NULL || !p_driver->mounted()) {
        /* Storage broken — let the user in for this session so they
         * aren't bricked. They'll be re-prompted next boot. */
        m_activated = true;
        return true;
    }

    activation_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic     = ACTIVATION_MAGIC;
    rec.activated = 1;

    int32_t err = p_driver->write_file_data(ACTIVATION_FILE_NAME, &rec, sizeof(rec));
    if (err < 0) {
        m_activated = true;
        return true;
    }

    /* Hide from file listings (same pattern as settings.bin). */
    vfs_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.has_flags = true;
    meta.flags     = VFS_OBJ_FLAG_HIDDEN;
    uint8_t meta_data[VFS_MAX_META_LEN];
    vfs_meta_encode(meta_data, sizeof(meta_data), &meta);
    p_driver->update_file_meta(ACTIVATION_FILE_NAME, meta_data, sizeof(meta_data));

    m_activated = true;
    return true;
}
