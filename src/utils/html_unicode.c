/*
 * html_unicode.c - Unicode/encoding helpers: UTF-8 validation,
 * scalar-to-UTF-8 encoding, Windows-1252 transcoding, and
 * cut-boundary backing.
 * Depends on: libc (string).
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "html_unicode.h"


size_t utf8_encode_cp(unsigned int cp, char out[4])
{
    if (cp <= 0x7F)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF)
    {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0; /* surrogate */
    if (cp <= 0xFFFF)
    {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF)
    {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

unsigned int cp1252_cp(unsigned char b)
{
    static const unsigned short map[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
    };
    if (b < 0x80) return b;
    if (b < 0xA0) return map[b - 0x80];
    return b; /* latin-1 range */
}

int utf8_valid(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n)
    {
        unsigned char c = (unsigned char)s[i];
        size_t need;
        unsigned char first_lo, first_hi;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
            first_lo = 0x80;
            first_hi = 0xBF;
        }
        else if (c == 0xE0) {
            need = 2;
            first_lo = 0xA0;
            first_hi = 0xBF;
        }
        else if (c >= 0xE1 && c <= 0xEC) {  // NOLINT(bugprone-branch-clone)
            need = 2;
            first_lo = 0x80;
            first_hi = 0xBF;
        }
        else if (c == 0xED) {
            need = 2;
            first_lo = 0x80;
            first_hi = 0x9F;
        }
        else if (c >= 0xEE && c <= 0xEF) {
            need = 2;
            first_lo = 0x80;
            first_hi = 0xBF;
        }
        else if (c == 0xF0) {
            need = 3;
            first_lo = 0x90;
            first_hi = 0xBF;
        }
        else if (c >= 0xF1 && c <= 0xF3) {
            need = 3;
            first_lo = 0x80;
            first_hi = 0xBF;
        }
        else if (c == 0xF4) {
            need = 3;
            first_lo = 0x80;
            first_hi = 0x8F;
        }
        else return 0; /* 0x80-0xC1, 0xF5-0xFF: never a lead byte */
        if (i + need >= n) return 0; /* sequence truncated */
        for (size_t k = 1; k <= need; k++)
        {
            unsigned char cc = (unsigned char)s[i + k];
            if (cc < 0x80 || cc > 0xBF) return 0;
        }
        unsigned char fc = (unsigned char)s[i + 1];
        if (fc < first_lo || fc > first_hi) return 0; /* overlong/surrogate */
        i += need + 1;
    }
    return 1;
}

size_t utf8_cut_boundary(const char *data, size_t cut)
{
    while (cut > 0 && ((unsigned char)data[cut] & 0xC0) == 0x80)
        cut--;
    return cut;
}

int prefix_ieq(const char *s, const char *pat)
{
    size_t plen = strlen(pat);
    for (size_t i = 0; i < plen; i++)
    {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)pat[i]))
            return 0;
    }
    return 1;
}
