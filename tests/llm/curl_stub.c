/*
 * curl_stub.c - test-only curl stubs shared by the ollama test targets
 * (test_ollama, test_ollama_tool_calls): shadow the real libcurl at
 * link time so the provider's transfers can be captured and failed on
 * demand. Compiled only under OLLAMA_TEST; the captured values live in
 * ollama.c. Split out of ollama.c (2026-08 file-length compliance).
 * Depends on: libcurl types (types only, no linking).
 */

#ifdef OLLAMA_TEST

#include <stdarg.h>
#include <stdlib.h>

#include <curl/curl.h>
#include "../../src/utils/string_utils.h"

/* curl >= 8.2 ships curl_easy_setopt as a function-like macro; the
 * stub must undef it to define the function under that name. */
#undef curl_easy_setopt
#undef curl_easy_init
#undef curl_easy_perform
#undef curl_easy_cleanup
#undef curl_easy_strerror
#undef curl_slist_append
#undef curl_slist_free_all

extern char *ollama_test_captured_url;
extern char *ollama_test_captured_body;
extern int ollama_test_curl_init_fails;
extern CURLcode ollama_test_curl_result;

CURL *curl_easy_init(void)
{
    if (ollama_test_curl_init_fails) return NULL;
    static int dummy;
    return (CURL *)&dummy;
}

CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...)
{
    (void)curl;
    va_list args;
    va_start(args, option);
    if (option == CURLOPT_URL)
        ollama_test_captured_url = va_arg(args, char *);
    else if (option == CURLOPT_POSTFIELDS)
    {
        free(ollama_test_captured_body);
        ollama_test_captured_body = str_dup(va_arg(args, char *));
    }
    else if (option == CURLOPT_POST)
        (void)va_arg(args, long);
    else if (option == CURLOPT_HTTPHEADER)
        (void)va_arg(args, struct curl_slist *);
    else if (option == CURLOPT_WRITEFUNCTION)
        (void)va_arg(args, void *);
    else if (option == CURLOPT_WRITEDATA)
        (void)va_arg(args, void *);
    else if (option == CURLOPT_TIMEOUT)
        (void)va_arg(args, long);
    else if (option == CURLOPT_LOW_SPEED_LIMIT)
        (void)va_arg(args, long);
    else if (option == CURLOPT_LOW_SPEED_TIME)
        (void)va_arg(args, long);
    else
        (void)va_arg(args, void *);
    va_end(args);
    return CURLE_OK;
}

CURLcode curl_easy_perform(CURL *curl)
{
    (void)curl;
    return ollama_test_curl_result;
}

void curl_easy_cleanup(CURL *curl)
{
    (void)curl;
}

const char *curl_easy_strerror(CURLcode code)
{
    (void)code;
    return "stub error";
}

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *data)
{
    (void)list;
    (void)data;
    static struct curl_slist dummy;
    return &dummy;
}

void curl_slist_free_all(struct curl_slist *list)
{
    (void)list;
}

#endif /* OLLAMA_TEST */
