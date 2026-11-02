#include "cjk_font.h"

#include "cjk_font_data.h"
#include "gbk_table.h"

static int utf8_one(const uint8_t *s, size_t n, uint32_t *cp)
{
    if (!n) return 0;
    uint8_t c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        if (n < 2 || (s[1] & 0xC0) != 0x80) return -1;
        *cp = ((uint32_t)(c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        if (n < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80)
            return -1;
        uint32_t v = ((uint32_t)(c & 0x0F) << 12) |
                     ((uint32_t)(s[1] & 0x3F) << 6) |
                     (s[2] & 0x3F);
        if (v < 0x800 || (v >= 0xD800 && v <= 0xDFFF)) return -1;
        *cp = v;
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (n < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 ||
            (s[3] & 0xC0) != 0x80) {
            return -1;
        }
        uint32_t v = ((uint32_t)(c & 0x07) << 18) |
                     ((uint32_t)(s[1] & 0x3F) << 12) |
                     ((uint32_t)(s[2] & 0x3F) << 6) |
                     (s[3] & 0x3F);
        if (v < 0x10000 || v > 0x10FFFF) return -1;
        *cp = v;
        return 4;
    }
    return -1;
}

static uint16_t gbk_lookup(uint8_t lead, uint8_t trail)
{
    if (lead < 0x81 || lead > 0xFE) return 0xFFFF;
    if (trail < 0x40 || trail > 0xFE || trail == 0x7F) return 0xFFFF;
    int ti = trail - 0x40 - (trail > 0x7F ? 1 : 0);
    return gbk_to_unicode[lead - 0x81][ti];
}

bool ssid_utf8_valid(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint32_t cp;
        int r = utf8_one(s + i, n - i, &cp);
        if (r < 0) return false;
        if (r == 0) break;
        i += (size_t)r;
    }
    return true;
}

bool ssid_gbk_valid(const uint8_t *s, size_t n)
{
    bool high = false;
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        if (i + 1 >= n) return false;
        if (gbk_lookup(c, s[i + 1]) == 0xFFFF) return false;
        high = true;
        i += 2;
    }
    return high;
}

static bool cp_is_cjk(uint32_t cp)
{
    return (cp >= 0x3000 && cp <= 0x303F) ||   /* CJK punctuation */
           (cp >= 0x3400 && cp <= 0x4DBF) ||   /* CJK ext A */
           (cp >= 0x4E00 && cp <= 0x9FFF) ||   /* CJK unified */
           (cp >= 0xF900 && cp <= 0xFAFF) ||   /* compatibility ideographs */
           (cp >= 0xFF00 && cp <= 0xFFEF);     /* fullwidth forms */
}

/* A GBK byte pair often happens to be valid UTF-8, but it then decodes to an
 * unrelated script (Hebrew, Greek, private use, ...).  Treat only the scripts
 * that actually show up in SSIDs as plausible. */
static bool cp_is_plausible(uint32_t cp)
{
    if (cp < 0x80) return true;
    if (cp >= 0x00A0 && cp <= 0x024F) return true; /* Latin-1 + extended */
    if (cp_is_cjk(cp)) return true;
    if (cp >= 0x2000 && cp <= 0x206F) return true; /* general punctuation */
    if (cp >= 0x20A0 && cp <= 0x20BF) return true; /* currency */
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return true; /* emoji */
    return false;
}

static bool ssid_plausible(const uint8_t *s, size_t n, ssid_enc_t enc)
{
    size_t pos = 0;
    while (pos < n) {
        uint32_t cp;
        size_t used = ssid_decode(s, n, pos, enc, &cp);
        if (!used) break;
        pos += used;
        if (!cp_is_plausible(cp)) return false;
    }
    return true;
}

ssid_enc_t ssid_detect(const uint8_t *s, size_t n)
{
    bool high = false;
    for (size_t i = 0; i < n; i++) {
        if (s[i] & 0x80) {
            high = true;
            break;
        }
    }
    if (!high) return SSID_ENC_ASCII;

    bool u8 = ssid_utf8_valid(s, n);
    bool gbk = ssid_gbk_valid(s, n);

    if (u8 && gbk) {
        /* Prefer UTF-8 unless it decodes to an implausible script while GBK
         * decodes cleanly.  This keeps GBK 中文 (D6 D0 CE C4), which is also
         * valid UTF-8, from being shown as Hebrew/Greek.  The SSID page still
         * lets the user switch encodings when the bytes are truly ambiguous. */
        if (ssid_plausible(s, n, SSID_ENC_UTF8)) return SSID_ENC_UTF8;
        if (ssid_plausible(s, n, SSID_ENC_GBK)) return SSID_ENC_GBK;
        return SSID_ENC_UTF8;
    }
    if (u8) return SSID_ENC_UTF8;
    if (gbk) return SSID_ENC_GBK;
    return SSID_ENC_UNKNOWN;
}

const char *ssid_enc_name(ssid_enc_t e)
{
    switch (e) {
    case SSID_ENC_ASCII: return "ASCII";
    case SSID_ENC_UTF8: return "UTF8";
    case SSID_ENC_GBK: return "GBK";
    default: return "?";
    }
}

size_t ssid_decode(const uint8_t *s, size_t n, size_t pos, ssid_enc_t enc,
                   uint32_t *cp)
{
    if (pos >= n) return 0;
    uint8_t c = s[pos];

    if (enc == SSID_ENC_ASCII || enc == SSID_ENC_UNKNOWN) {
        *cp = (c < 0x80) ? c : '?';
        return 1;
    }

    if (enc == SSID_ENC_UTF8) {
        int r = utf8_one(s + pos, n - pos, cp);
        if (r <= 0) {
            *cp = '?';
            return 1;
        }
        return (size_t)r;
    }

    /* GBK */
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if (pos + 1 >= n) {
        *cp = '?';
        return 1;
    }
    uint16_t u = gbk_lookup(c, s[pos + 1]);
    *cp = (u == 0xFFFF) ? '?' : u;
    return 2;
}

const uint8_t *cjk_glyph(uint32_t cp)
{
    if (cp >= CJK_HAN_FIRST && cp < CJK_HAN_FIRST + CJK_HAN_COUNT)
        return cjk_han[cp - CJK_HAN_FIRST];
    if (cp >= CJK_PUNCT_FIRST && cp < CJK_PUNCT_FIRST + CJK_PUNCT_COUNT)
        return cjk_punct[cp - CJK_PUNCT_FIRST];
    if (cp >= CJK_FULL_FIRST && cp < CJK_FULL_FIRST + CJK_FULL_COUNT)
        return cjk_full[cp - CJK_FULL_FIRST];
    return 0;
}
