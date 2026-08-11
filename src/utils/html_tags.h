/*
 * html_tags.h - tag classification contracts.
 * Depends on: html_internal.h (TagId).
 */

#ifndef ECHO_HTML_TAGS_H
#define ECHO_HTML_TAGS_H

#include <stddef.h>

#include "html_internal.h"

/**
 * tag_lookup - classify a tag name
 * @name: tag name bytes (not NUL-terminated).
 * @n: byte count; 0 and >= 32 fail.
 *
 * Return: the matching TagId, or TAG_UNKNOWN.
 */
TagId tag_lookup(const char *name, size_t n);

/**
 * is_frame_tag - test whether a tag opens a scored frame
 * @tid: tag id.
 *
 * Return: 1 for frame tags (article..h6), 0 otherwise.
 */
int is_frame_tag(TagId tid);

/**
 * is_excluded_tag - test whether a tag is hard-excluded
 * @tid: tag id.
 *
 * Return: 1 for excluded tags (nav..svg), 0 otherwise.
 */
int is_excluded_tag(TagId tid);

/**
 * is_header_tag - test whether a tag is a heading
 * @tid: tag id.
 *
 * Return: 1 for h1..h6, 0 otherwise.
 */
int is_header_tag(TagId tid);

/**
 * tag_weight_of - Crawl4AI frame weight for scoring
 * @tid: tag id.
 *
 * Return: the weight (1.5 for article, ...), 0.5 default.
 */
double tag_weight_of(TagId tid);

/**
 * has_neg_pattern - test class/id bytes for boilerplate patterns
 * @s: class or id attribute bytes.
 * @n: byte count.
 *
 * Return: 1 when any negative pattern ("nav", "footer", ...) appears
 * case-insensitively in @s[0..n), 0 otherwise.
 */
int has_neg_pattern(const char *s, size_t n);

#endif /* ECHO_HTML_TAGS_H */
