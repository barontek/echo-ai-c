#ifndef ECHO_SEMANTIC_SEARCH_H
#define ECHO_SEMANTIC_SEARCH_H

#include "tool.h"
#include "../safety/safety.h"

void semantic_search_index_document(const char *content);
Tool *tool_semantic_search_create(SafetyConfig *safety);

#ifdef SEMANTIC_SEARCH_TEST
void semantic_search_test_set_alloc_fail(int nth_allocation);
void semantic_search_test_reset(void);
#endif

#endif
