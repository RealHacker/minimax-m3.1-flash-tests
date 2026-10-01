#include "entities.h"

#include <string.h>

/* Common named references, ASCII-sorted by name. */
typedef struct {
    const char *name;
    const char *utf8;
} md_entity_named;

static const md_entity_named md_entities[] = {
    { "Dagger", "\xE2\x80\xA1" },   /* U+2021 */
    { "Prime",  "\xE2\x80\xB2" },   /* U+2032 */
    { "amp",    "&" },
    { "apos",   "'" },
    { "bull",   "\xE2\x80\xA2" },   /* U+2022 */
    { "cent",   "\xC2\xA2" },       /* U+00A2 */
    { "copy",   "\xC2\xA9" },       /* U+00A9 */
    { "dagger", "\xE2\x80\xA0" },   /* U+2020 */
    { "deg",    "\xC2\xB0" },       /* U+00B0 */
    { "divide", "\xC3\xB7" },       /* U+00F7 */
    { "euro",   "\xE2\x82\xAC" },   /* U+20AC */
    { "frac12", "\xC2\xBD" },       /* U+00BD */
    { "frac14", "\xC2\xBC" },       /* U+00BC */
    { "frac34", "\xC2\xBE" },       /* U+00BE */
    { "gt",     ">" },
    { "hArr",   "\xE2\x86\x94" },   /* U+2194 */
    { "hearts", "\xE2\x99\xA5" },   /* U+2665 */
    { "hellip", "\xE2\x80\xA6" },   /* U+2026 */
    { "iexcl",  "\xC2\xA1" },       /* U+00A1 */
    { "iquest", "\xC2\xBF" },       /* U+00BF */
    { "lArr",   "\xE2\x87\x90" },   /* U+21D0 */
    { "laquo",  "\xC2\xAB" },       /* U+00AB */
    { "larr",   "\xE2\x86\x90" },   /* U+2190 */
    { "ldquo",  "\xE2\x80\x9C" },   /* U+201C */
    { "lsquo",  "\xE2\x80\x98" },   /* U+2018 */
    { "lt",     "<" },
    { "macr",   "\xC2\xAF" },       /* U+00AF */
    { "mdash",  "\xE2\x80\x94" },   /* U+2014 */
    { "micro",  "\xC2\xB5" },       /* U+00B5 */
    { "middot", "\xC2\xB7" },       /* U+00B7 */
    { "minus",  "\xE2\x88\x92" },   /* U+2212 */
    { "nbsp",   "\xC2\xA0" },       /* U+00A0 */
    { "ndash",  "\xE2\x80\x93" },   /* U+2013 */
    { "ne",     "\xE2\x89\xA0" },   /* U+2260 */
    { "para",   "\xC2\xB6" },       /* U+00B6 */
    { "permil", "\xE2\x80\xB0" },   /* U+2030 */
    { "plusmn", "\xC2\xB1" },       /* U+00B1 */
    { "pound",  "\xC2\xA3" },       /* U+00A3 */
    { "prime",  "\xE2\x80\xB2" },   /* U+2032 */
    { "quot",   "\"" },
    { "raquo",  "\xC2\xBB" },       /* U+00BB */
    { "rarr",   "\xE2\x86\x92" },   /* U+2192 */
    { "rdquo",  "\xE2\x80\x9D" },   /* U+201D */
    { "reg",    "\xC2\xAE" },       /* U+00AE */
    { "rsquo",  "\xE2\x80\x99" },   /* U+2019 */
    { "sect",   "\xC2\xA7" },       /* U+00A7 */
    { "shy",    "\xC2\xAD" },       /* U+00AD */
    { "sup2",   "\xC2\xB2" },       /* U+00B2 */
    { "sup3",   "\xC2\xB3" },       /* U+00B3 */
    { "times",  "\xC3\x97" },       /* U+00D7 */
    { "trade",  "\xE2\x84\xA2" },   /* U+2122 */
    { "uarr",   "\xE2\x86\x91" },   /* U+2191 */
    { "yen",    "\xC2\xA5" }        /* U+00A5 */
};

#define MD_ENTITY_COUNT (sizeof md_entities / sizeof md_entities[0])

/* Encode one code point as UTF-8; returns the byte count (1..4). */
static size_t encode_utf8(unsigned long cp, char *out, size_t out_cap)
{
    if (cp == 0 || cp > 0x10FFFFul || (cp >= 0xD800ul && cp <= 0xDFFFul)) {
        cp = 0xFFFDul; /* unencodable code point */
    }
    if (cp < 0x80ul) {
        if (out_cap < 1u) {
            return 0;
        }
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800ul) {
        if (out_cap < 2u) {
            return 0;
        }
        out[0] = (char)(0xC0ul | (cp >> 6));
        out[1] = (char)(0x80ul | (cp & 0x3Ful));
        return 2;
    }
    if (cp < 0x10000ul) {
        if (out_cap < 3u) {
            return 0;
        }
        out[0] = (char)(0xE0ul | (cp >> 12));
        out[1] = (char)(0x80ul | ((cp >> 6) & 0x3Ful));
        out[2] = (char)(0x80ul | (cp & 0x3Ful));
        return 3;
    }
    if (out_cap < 4u) {
        return 0;
    }
    out[0] = (char)(0xF0ul | (cp >> 18));
    out[1] = (char)(0x80ul | ((cp >> 12) & 0x3Ful));
    out[2] = (char)(0x80ul | ((cp >> 6) & 0x3Ful));
    out[3] = (char)(0x80ul | (cp & 0x3Ful));
    return 4;
}

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return (int)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (int)(c - 'a') + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return (int)(c - 'A') + 10;
    }
    return -1;
}

static const md_entity_named *lookup_named(const char *src, size_t len,
                                           size_t pos, size_t *consumed)
{
    size_t i;

    for (i = 0; i < MD_ENTITY_COUNT; i++) {
        size_t name_len = strlen(md_entities[i].name);

        if (name_len + 2u > len - pos) {
            continue;
        }
        if (memcmp(src + pos + 1u, md_entities[i].name, name_len) != 0) {
            continue;
        }
        if (src[pos + 1u + name_len] != ';') {
            continue;
        }
        *consumed = name_len + 2u;
        return &md_entities[i];
    }
    return NULL;
}

size_t md_entity_decode(const char *src, size_t len, size_t pos, char *out,
                        size_t out_cap, size_t *out_len)
{
    size_t consumed = 0;

    if (src == NULL || out == NULL || out_len == NULL || pos >= len ||
        src[pos] != '&' || out_cap < 4u) {
        return 0;
    }
    if (pos + 2u < len && src[pos + 1u] == '#') {
        size_t p = pos + 2u;
        unsigned long cp = 0;
        int digits = 0;

        if (p < len && (src[p] == 'x' || src[p] == 'X')) {
            p++;
            while (p < len && hex_digit((unsigned char)src[p]) >= 0) {
                if (cp < 0x110000ul) {
                    cp = cp * 16ul +
                         (unsigned long)hex_digit((unsigned char)src[p]);
                }
                p++;
                digits++;
            }
        } else {
            while (p < len && src[p] >= '0' && src[p] <= '9') {
                if (cp < 0x110000ul) {
                    cp = cp * 10ul + (unsigned long)(src[p] - '0');
                }
                p++;
                digits++;
            }
        }
        if (digits == 0 || p >= len || src[p] != ';') {
            return 0;
        }
        *out_len = encode_utf8(cp, out, out_cap);
        return p + 1u - pos;
    }
    {
        const md_entity_named *named = lookup_named(src, len, pos, &consumed);
        size_t n;

        if (named == NULL) {
            return 0;
        }
        n = strlen(named->utf8);
        if (n > out_cap) {
            return 0;
        }
        memcpy(out, named->utf8, n);
        *out_len = n;
        return consumed;
    }
}
