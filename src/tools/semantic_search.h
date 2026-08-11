/*
 * semantic_search.h - Process-wide TF-IDF document index and the
 * semantic_search tool that queries it. Depends on: tool.h, safety.h.
 */

#ifndef ECHO_SEMANTIC_SEARCH_H
#define ECHO_SEMANTIC_SEARCH_H

#include "tool.h"
#include "../safety/safety.h"

/**
 * semantic_search_index_document - add a document to the in-process index
 * @content: document text; stored verbatim for snippet extraction and
 *   tokenized (lowercased, 3-char minimum) for scoring. NULL is
 *   tolerated and indexes nothing.
 *
 * The index is a process-wide singleton with fixed capacity (MAX_DOCS
 * documents, MAX_TOKENS distinct terms); overflow and allocation
 * failures are reported through the return value. The index is only
 * committed (doc_count incremented) when the whole document fit.
 *
 * Return: 0 on success (or NULL content, which indexes nothing); -1
 * when the index is full or an allocation failed. Thread-safety: not
 * thread-safe; the caller serializes access. Overflow and allocation
 * failures are silently dropped, and the arrays are allocated lazily on
 * first use. A document whose indexing fails partway is not counted, so
 * the index stays consistent. The index lives for the whole process and
 * is shared with the semantic_search tool's execute callback; there is
 * no way to remove a single document — release the whole index with
 * semantic_search_free_index() when the process is done with it.
 */
int semantic_search_index_document(const char *content);

/**
 * semantic_search_free_index - tear down the process-wide index
 *
 * Frees every document, term, frequency row, and the backing arrays,
 * leaving the index empty and reusable for a fresh indexing pass. The
 * tool created by tool_semantic_search_create() calls this from its
 * destroy() callback, so applications that register the tool never need
 * to call it directly.
 *
 * Return: nothing. Not thread-safe: must not run concurrently with
 * semantic_search_index_document(), semantic_search_execute(), or
 * semantic_search_test_reset(), which share the same static index.
 */
void semantic_search_free_index(void);

/**
 * tool_semantic_search_create - construct the semantic_search tool
 * @safety: unused — the tool performs no safety checks; accepted only
 *   for signature uniformity with the other tool factories.
 *
 * The tool's execute callback runs a TF-IDF query against the global
 * index built by semantic_search_index_document() and returns up to
 * top_k scored snippets in descending order. The tool keeps no
 * per-instance state.
 *
 * Return: heap-allocated Tool, or NULL on allocation failure. The caller
 * owns the Tool and must release it via its destroy() callback, which
 * also frees the entire global index — destroying one semantic_search
 * tool empties the index for every other user of it. The safety pointer
 * is borrowed, never freed by the tool.
 */
Tool *tool_semantic_search_create(SafetyConfig *safety);

#ifdef SEMANTIC_SEARCH_TEST
/**
 * semantic_search_test_set_alloc_fail - make the Nth str_dup fail here
 * @nth_allocation: 1-based index of the str_dup call to fail; -1
 *   disables fault injection.
 *
 * Test-only hook. Resets the call counter, fails the Nth str_dup (only
 * that call), and leaves every other allocation to behave normally.
 *
 * Return: nothing. Single-threaded tests only.
 */
void semantic_search_test_set_alloc_fail(int nth_allocation);

/**
 * semantic_search_test_set_realloc_fail - make the Nth realloc fail here
 * @nth_realloc: 1-based index of the realloc call to fail; -1 disables
 *   fault injection.
 *
 * Test-only hook. Resets the call counter, fails the Nth realloc (only
 * that call), and leaves every other allocation to behave normally.
 *
 * Return: nothing. Single-threaded tests only.
 */
void semantic_search_test_set_realloc_fail(int nth_realloc);

/**
 * semantic_search_test_reset - tear down the global index
 *
 * Test-only hook. Frees every document, term, frequency row, and the
 * backing arrays, leaving the index empty and reusable.
 *
 * Return: nothing. Single-threaded tests only.
 */
void semantic_search_test_reset(void);
#endif

#endif
