/*
 * html_tags.c - tag classification: the Crawl4AI tag table,
 * frame/excluded/header predicates, weights, and boilerplate
 * class/id pattern matching.
 * Depends on: libc (ctype/string), html_internal.h for TagId.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "html_tags.h"
#include "html_internal.h"

/* Crawl4AI negative_patterns: class/id containing any of these is
 * boilerplate. */
const char *const NEG_PATTERNS[] = {
    "nav", "footer", "header", "sidebar", "ads", "comment",
    "promo", "advert", "social", "share",
};


TagId tag_lookup(const char *name, size_t n)
{
    if (n == 0 || n >= 32) return TAG_UNKNOWN;
    char buf[32];
    for (size_t i = 0; i < n; i++)
        buf[i] = (char)tolower((unsigned char)name[i]);
    buf[n] = '\0';
    for (size_t i = 0; i < sizeof(TAG_TABLE) / sizeof(TAG_TABLE[0]); i++)
    {
        if (strcmp(buf, TAG_TABLE[i].name) == 0) return TAG_TABLE[i].id;
    }
    return TAG_UNKNOWN;
}

int is_frame_tag(TagId tid)
{
    return tid >= TAG_FRAME_ARTICLE && tid <= TAG_FRAME_H6;
}

int is_excluded_tag(TagId tid)
{
    return tid >= TAG_EXCL_NAV && tid <= TAG_EXCL_SVG;
}

int is_header_tag(TagId tid)
{
    return tid >= TAG_FRAME_H1 && tid <= TAG_FRAME_H6;
}

double tag_weight_of(TagId tid)
{
    switch (tid)
    {
    case TAG_FRAME_ARTICLE: return 1.5;
    case TAG_FRAME_SECTION: return 1.0;
    case TAG_FRAME_P:       return 1.0;
    case TAG_FRAME_H1:      return 1.2;
    case TAG_FRAME_H2:      return 1.1;
    case TAG_FRAME_H3:      return 1.0;
    case TAG_FRAME_H4:      return 0.9;
    case TAG_FRAME_H5:      return 0.8;
    case TAG_FRAME_H6:      return 0.7;
    default:                return 0.5;
    }
}

int has_neg_pattern(const char *s, size_t n)
{
    for (size_t p = 0; p < sizeof(NEG_PATTERNS) / sizeof(NEG_PATTERNS[0]); p++)
    {
        size_t plen = strlen(NEG_PATTERNS[p]);
        if (n < plen) continue;
        for (size_t i = 0; i + plen <= n; i++)
        {
            size_t j = 0;
            while (j < plen &&
                   tolower((unsigned char)s[i + j]) == (unsigned char)NEG_PATTERNS[p][j])
                j++;
            if (j == plen) return 1;
        }
    }
    return 0;
}
