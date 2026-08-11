/*
 * html_entities.c - HTML entity decoding: named and numeric
 * entities to UTF-8 bytes.
 * Depends on: html_unicode (encoding), libc (ctype).
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "html_entities.h"
#include "html_unicode.h"


typedef struct {
    const char *name;
    unsigned char ch;
} EntityEntry;

static const EntityEntry ENTITY_TABLE[] = {
    {"amp", '&'},    {"lt", '<'},     {"gt", '>'},
    {"quot", '"'},   {"apos", '\''},  {"nbsp", 0xA0},
    {"copy", 0xA9},  {"reg", 0xAE},   {"trade", 0x99},
    {"hellip", 0x85},{"mdash", 0x97}, {"ndash", 0x96},
    {"lsquo", 0x91}, {"rsquo", 0x92}, {"ldquo", 0x93},
    {"rdquo", 0x94}, {"middot", 0xB7},{"bull", 0x95},
    {"deg", 0xB0},   {"plusmn", 0xB1},{"times", 0xD7},
    {"divide", 0xF7},{"frac12", 0xBD},{"frac14", 0xBC},
    {"frac34", 0xBE},{"sup2", 0xB2},  {"sup3", 0xB3},
    {"para", 0xB6},  {"sect", 0xA7},  {"permil", 0x89},
    {"euro", 0x80},  {"pound", 0xA3}, {"cent", 0xA2},
    {"yen", 0xA5},   {"micro", 0xB5},
    {"eacute", 0xE9},{"agrave", 0xE0},{"ntilde", 0xF1},
    {"ccedil", 0xE7},{"szlig", 0xDF}, {"uuml", 0xFC},
    {"aacute", 0xE1},{"iacute", 0xED},{"oacute", 0xF3},
    {"uacute", 0xFA},{"auml", 0xE4},  {"igrave", 0xEC},
    {"ograve", 0xF2},{"ugrave", 0xF9},{"acirc", 0xE2},
    {"ecirc", 0xEA}, {"icirc", 0xEE}, {"ocirc", 0xF4},
    {"ucirc", 0xFB}, {"atilde", 0xE3},{"otilde", 0xF5},
};

static long entity_lookup(const char *name, size_t n)
{
    for (size_t i = 0; i < sizeof(ENTITY_TABLE) / sizeof(ENTITY_TABLE[0]); i++)
    {
        if (strlen(ENTITY_TABLE[i].name) == n &&
            memcmp(ENTITY_TABLE[i].name, name, n) == 0)
            return (long)ENTITY_TABLE[i].ch;
    }
    return -1;
}

int decode_entity(const char *s, size_t avail, char out[4],
                         size_t *out_len, size_t *consumed)
{
    if (avail < 2 || s[0] != '&') return 0;

    if (s[1] == '#')
    {
        size_t j = 2;
        int is_hex = 0;
        if (j < avail && (s[j] == 'x' || s[j] == 'X'))
        {
            is_hex = 1;
            j++;
        }
        unsigned long base = is_hex ? 16ul : 10ul;
        unsigned long v = 0;
        size_t digits = 0;
        while (j < avail && digits < 8)
        {
            unsigned char c = (unsigned char)s[j];
            unsigned d;
            if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
            else if (is_hex && c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
            else if (is_hex && c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
            else break;
            if (v > (0x10FFFFul - d) / base) return 0; /* overflow / too large */
            v = v * base + d;
            digits++;
            j++;
        }
        if (digits == 0) return 0;
        if (j < avail && s[j] == ';') j++;
        size_t elen = utf8_encode_cp((unsigned)v, out);
        if (elen == 0) return 0; /* surrogate or out of range: not an entity */
        *out_len = elen;
        *consumed = j;
        return 1;
    }

    size_t j = 1;
    while (j < avail && j < 12 && isalnum((unsigned char)s[j])) j++;
    if (j == 1) return 0;
    long ch = entity_lookup(s + 1, j - 1);
    if (ch < 0) return 0;
    if (j < avail && s[j] == ';') j++;
    *out_len = utf8_encode_cp(cp1252_cp((unsigned char)ch), out);
    *consumed = j;
    return 1;
}
