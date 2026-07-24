#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#include "src/config/config.h"
#include "src/utils/string_utils.h"

static char *resp_buf = NULL;
static size_t resp_len = 0;
static size_t resp_cap = 1;

static size_t write_cb(void *ptr, size_t sz, size_t nm, void *ud)
{
    (void)ud;
    size_t total = sz * nm;
    while (resp_len + total + 1 > resp_cap) {
        resp_cap *= 2;
        resp_buf = realloc(resp_buf, resp_cap);
    }
    memcpy(resp_buf + resp_len, ptr, total);
    resp_len += total;
    resp_buf[resp_len] = '\0';
    return total;
}

#if defined(__linux__)
#define SYSTEM_OS "Linux"
#elif defined(__APPLE__)
#define SYSTEM_OS "macOS"
#elif defined(_WIN32)
#define SYSTEM_OS "Windows"
#else
#define SYSTEM_OS "Unknown"
#endif

static const char *TOOLS_JSON =
    "["
    "{\"type\":\"function\",\"function\":{\"name\":\"list_dir\","
    "\"description\":\"List files and directories in a given path\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"Directory path\"}},"
    "\"required\":[]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"read_file\","
    "\"description\":\"Read contents of a file\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"file_path\":{\"type\":\"string\",\"description\":\"Path to the file\"}},"
    "\"required\":[\"file_path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"bash\","
    "\"description\":\"Execute shell commands\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\",\"description\":\"Shell command to run\"}},"
    "\"required\":[\"command\"]}}}"
    "]";

static char *escape_json(const char *s)
{
    char *esc = malloc(strlen(s) * 2 + 1);
    if (!esc) return NULL;
    size_t e = 0;
    for (size_t i = 0; s[i]; i++) {
        switch (s[i]) {
        case '"':  esc[e++] = '\\'; esc[e++] = '"';  break;
        case '\\': esc[e++] = '\\'; esc[e++] = '\\'; break;
        case '\n': esc[e++] = '\\'; esc[e++] = 'n';  break;
        case '\r': esc[e++] = '\\'; esc[e++] = 'r';  break;
        case '\t': esc[e++] = '\\'; esc[e++] = 't';  break;
        default:   esc[e++] = s[i]; break;
        }
    }
    esc[e] = '\0';
    return esc;
}

int main(void)
{
    Conf *conf = conf_load("config.conf");
    if (!conf) { fprintf(stderr, "failed to load config\n"); return 1; }

    const char *raw_sp = conf_get(conf, "agent.system_prompt");
    const char *model  = conf_get(conf, "agent.model");
    int num_ctx       = conf_get_int(conf, "ollama.num_ctx", 32768);
    int keep_alive    = conf_get_int(conf, "ollama.keep_alive_secs", 120);
    double temperature = conf_get_int(conf, "agent.temperature", 30) / 100.0;
    const char *burl  = conf_get(conf, "ollama.base_url");

    if (!model) model = "gemma4-32k";
    if (!burl) burl = "http://localhost:11434";

    /* Build system prompt with context (mirrors build_system_prompt in agent.c) */
    char cwd_buf[4096];
    const char *cwd = getcwd(cwd_buf, sizeof(cwd_buf));
    if (!cwd) cwd = ".";

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    char *sys_prompt = NULL;
    asprintf(&sys_prompt, "%s\n\n[System Context]\nOS: " SYSTEM_OS "\n"
             "Current Working Directory: %s\nCurrent Time: %s\n",
             raw_sp ? raw_sp : "You are a helpful AI assistant.",
             cwd, time_buf);

    char *esp = escape_json(sys_prompt);
    printf("=== SYSTEM PROMPT (first 200 chars) ===\n%.200s...\n\n", sys_prompt);

    char *body = NULL;
    asprintf(&body,
        "{\"model\":\"%s\","
        "\"messages\":["
            "{\"role\":\"system\",\"content\":\"%s\"},"
            "{\"role\":\"user\",\"content\":\"hi\"}"
        "],"
        "\"stream\":true,"
        "\"options\":{\"temperature\":%.2f,\"num_ctx\":%d},"
        "\"keep_alive\":%d,"
        "\"tools\":%s}",
        model, esp, temperature, num_ctx, keep_alive, TOOLS_JSON);

    char url[512];
    snprintf(url, sizeof(url), "%s/api/chat", burl);

    resp_buf = malloc(1);
    resp_buf[0] = '\0';
    resp_len = 0;
    resp_cap = 1;

    CURL *curl = curl_easy_init();
    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, NULL);

    CURLcode rc = curl_easy_perform(curl);
    printf("Status: %s\n\n", rc == CURLE_OK ? "OK" : "FAILED");

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    cJSON *resp_json = NULL;
    char *final_thinking = NULL;
    char *final_content = NULL;
    int has_tool_calls = 0;

    /* Parse streaming response — accumulate thinking/content across lines */
    char *line = strtok(resp_buf, "\n");
    while (line) {
        cJSON *chunk = cJSON_Parse(line);
        if (chunk) {
            cJSON *msg = cJSON_GetObjectItem(chunk, "message");
            if (msg) {
                cJSON *t = cJSON_GetObjectItem(msg, "thinking");
                cJSON *c = cJSON_GetObjectItem(msg, "content");
                cJSON *tc = cJSON_GetObjectItem(msg, "tool_calls");
                if (t && cJSON_IsString(t)) {
                    size_t old = final_thinking ? strlen(final_thinking) : 0;
                    size_t add = strlen(cJSON_GetStringValue(t));
                    char *n = realloc(final_thinking, old + add + 1);
                    if (n) { memcpy(n + old, cJSON_GetStringValue(t), add + 1); final_thinking = n; }
                }
                if (c && cJSON_IsString(c) && strlen(cJSON_GetStringValue(c)) > 0) {
                    size_t old = final_content ? strlen(final_content) : 0;
                    size_t add = strlen(cJSON_GetStringValue(c));
                    char *n = realloc(final_content, old + add + 1);
                    if (n) { memcpy(n + old, cJSON_GetStringValue(c), add + 1); final_content = n; }
                }
                if (tc && cJSON_IsArray(tc) && cJSON_GetArraySize(tc) > 0)
                    has_tool_calls = 1;
            }
            cJSON_Delete(chunk);
        }
        line = strtok(NULL, "\n");
    }

    printf("thinking   = %s\n", final_thinking ? final_thinking : "(none)");
    printf("content    = %s\n", final_content ? final_content : "(none)");
    printf("tool_calls = %s\n", has_tool_calls ? "YES" : "none");

    free(body);
    free(resp_buf);
    free(sys_prompt);
    free(esp);
    conf_free(conf);
    return 0;
}
