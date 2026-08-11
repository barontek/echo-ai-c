/*
 * html_assembly.h - final output assembly contracts.
 * Depends on: html_internal.h (Extract), html_outbuf.h.
 */

#ifndef ECHO_HTML_ASSEMBLY_H
#define ECHO_HTML_ASSEMBLY_H

#include <stddef.h>

#include "html_internal.h"
#include "html_outbuf.h"

/**
 * assemble - build the final extraction result string
 * @x: extraction context (title, body, links already populated).
 *
 * Composes "Title: <t>\n\n<body><marker><footer>" within max_chars,
 * preferring paragraph then word boundaries for truncation and backing
 * cuts off UTF-8 continuation bytes. Drops the footer, then the title,
 * when the budget is too tight.
 *
 * Return: caller-owned NUL-terminated string (free with free()), or
 * NULL on allocation failure.
 */
char *assemble(Extract *x);

#endif /* ECHO_HTML_ASSEMBLY_H */
