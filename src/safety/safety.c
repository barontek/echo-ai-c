#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "safety.h"
#include "../utils/string_utils.h"

SafetyConfig *safety_config_create(void)
{
    SafetyConfig *cfg = calloc(1, sizeof(SafetyConfig));
    if (!cfg) return NULL;

    cfg->max_file_size = 10485760;
    cfg->max_execution_time = 300;
    cfg->allow_network = 1;

    return cfg;
}

void safety_config_free(SafetyConfig *cfg)
{
    if (!cfg) return;
    free(cfg->workspace);
    for (int i = 0; i < cfg->allowed_commands_count; i++) free(cfg->allowed_commands[i]);
    for (int i = 0; i < cfg->blocked_commands_count; i++) free(cfg->blocked_commands[i]);
    for (int i = 0; i < cfg->blocked_extensions_count; i++) free(cfg->blocked_extensions[i]);
    for (int i = 0; i < cfg->blocked_paths_count; i++) free(cfg->blocked_paths[i]);
    for (int i = 0; i < cfg->allowed_domains_count; i++) free(cfg->allowed_domains[i]);
    free(cfg->allowed_commands);
    free(cfg->blocked_commands);
    free(cfg->blocked_extensions);
    free(cfg->blocked_paths);
    free(cfg->allowed_domains);
    free(cfg);
}

int safety_check_path(const SafetyConfig *cfg, const char *path)
{
    (void)cfg;
    if (!path) return 0;

    if (strstr(path, "..") != NULL) return 0;

    static const char *blocked_exts[] = {
        ".key", ".pem", ".env", ".token", ".password",
        ".aws", ".netrc", ".htpasswd", ".crt", ".p12", NULL
    };

    for (int i = 0; blocked_exts[i]; i++)
    {
        if (str_ends_with(path, blocked_exts[i])) return 0;
    }

    static const char *blocked_paths[] = {
        "/etc/passwd", "/etc/shadow", "/etc/sudoers",
        ".git/config", NULL
    };

    for (int i = 0; blocked_paths[i]; i++)
    {
        if (strstr(path, blocked_paths[i]) != NULL) return 0;
    }

    return 1;
}

int safety_check_command(const SafetyConfig *cfg, const char *command)
{
    (void)cfg;

    static const char *dangerous[] = {
        "rm -rf /", "rm -rf /*", "mkfs.", "dd if=",
        "chmod 777", "> /dev/", "sudo rm", NULL
    };

    for (int i = 0; dangerous[i]; i++)
    {
        if (strstr(command, dangerous[i]) != NULL) return 0;
    }

    return 1;
}

int safety_check_url(const SafetyConfig *cfg, const char *url)
{
    if (!cfg->allow_network) return 0;

    if (cfg->allowed_domains_count > 0)
    {
        for (int i = 0; i < cfg->allowed_domains_count; i++)
        {
            if (strstr(url, cfg->allowed_domains[i]) != NULL) return 1;
        }
        return 0;
    }

    return 1;
}

int safety_check_file_size(const SafetyConfig *cfg, size_t size)
{
    return size <= cfg->max_file_size;
}

char *safety_resolve_path(const SafetyConfig *cfg, const char *path)
{
    if (!cfg->workspace || !path) return NULL;

    char *resolved = NULL;
    if (path[0] == '/')
        resolved = str_dup(path);
    else if (asprintf(&resolved, "%s/%s", cfg->workspace, path) < 0)
        resolved = NULL;

    return resolved;
}
