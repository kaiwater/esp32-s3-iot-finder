#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SSID_ENC_ASCII = 0,
    SSID_ENC_UTF8,
    SSID_ENC_GBK,
    SSID_ENC_UNKNOWN,
} ssid_enc_t;

bool ssid_utf8_valid(const uint8_t *s, size_t n);
bool ssid_gbk_valid(const uint8_t *s, size_t n);
ssid_enc_t ssid_detect(const uint8_t *s, size_t n);
const char *ssid_enc_name(ssid_enc_t e);

/* Returns the number of source bytes consumed, or 0 at end. */
size_t ssid_decode(const uint8_t *s, size_t n, size_t pos, ssid_enc_t enc,
                   uint32_t *cp);

/* 12x12, 24 bytes, MSB is the leftmost pixel of each row. */
const uint8_t *cjk_glyph(uint32_t cp);
