#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "safety.h"
#include "../config/config.h"
#include "../utils/string_utils.h"

SafetyConfig *safety_config_create(void)
{
    SafetyConfig *cfg = calloc(1, sizeof(SafetyConfig));
    if (!cfg) return NULL;

    cfg->max_file_size = 10485760;
    cfg->max_execution_time = 300;
    cfg->allow_network = 1;
    cfg->read_size_threshold = 1048576;

    return cfg;
}

static char **parse_csv(const char *val, int *count)
{
    *count = 0;
    if (!val || !val[0]) return NULL;

    StrArray arr = str_split(val, ',');
    if (arr.count == 0) return NULL;

    char **result = malloc(sizeof(char *) * arr.count);
    if (!result) { str_array_free(&arr); return NULL; }

    for (int i = 0; i < arr.count; i++)
    {
        char *trimmed = str_trim(arr.items[i]);
        result[i] = str_dup(trimmed ? trimmed : arr.items[i]);
    }
    *count = arr.count;
    str_array_free(&arr);
    return result;
}

void safety_load_from_conf(SafetyConfig *cfg, const Conf *conf)
{
    if (!cfg || !conf) return;

    const char *v = conf_get(conf, "safety.workspace");
    if (v) { free(cfg->workspace); cfg->workspace = str_dup(v); }

    v = conf_get(conf, "safety.allow_network");
    cfg->allow_network = v ? (strcmp(v, "true") == 0 || strcmp(v, "1") == 0) : 1;

    cfg->max_file_size = (size_t)conf_get_int(conf, "safety.max_file_size", 10485760);
    cfg->max_execution_time = conf_get_int(conf, "safety.max_execution_time", 300);

    v = conf_get(conf, "safety.allowed_commands");
    if (v) { free(cfg->allowed_commands); cfg->allowed_commands = parse_csv(v, &cfg->allowed_commands_count); }

    v = conf_get(conf, "safety.blocked_commands");
    if (v) { free(cfg->blocked_commands); cfg->blocked_commands = parse_csv(v, &cfg->blocked_commands_count); }

    v = conf_get(conf, "safety.allowed_extensions");
    if (v) { free(cfg->allowed_extensions); cfg->allowed_extensions = parse_csv(v, &cfg->allowed_extensions_count); }

    v = conf_get(conf, "safety.blocked_extensions");
    if (v) { free(cfg->blocked_extensions); cfg->blocked_extensions = parse_csv(v, &cfg->blocked_extensions_count); }

    v = conf_get(conf, "safety.allowed_domains");
    if (v) { free(cfg->allowed_domains); cfg->allowed_domains = parse_csv(v, &cfg->allowed_domains_count); }

    v = conf_get(conf, "safety.require_approval_for");
    if (v) {
        free(cfg->require_approval_for);
        cfg->require_approval_for = parse_csv(v, &cfg->require_approval_count);
    }
    /* Sensible defaults: tools that modify files or execute code */
    if (cfg->require_approval_count == 0)
    {
        const char *defaults[] = {
            "bash", "write_file", "replace_in_file", "git",
            "python_execute", "delegate",
        };
        int ndef = sizeof(defaults) / sizeof(defaults[0]);
        cfg->require_approval_for = calloc(ndef, sizeof(char *));
        if (cfg->require_approval_for)
        {
            for (int i = 0; i < ndef; i++)
                cfg->require_approval_for[i] = str_dup(defaults[i]);
            cfg->require_approval_count = ndef;
        }
    }

    v = conf_get(conf, "safety.audit_log_path");
    if (v) { free(cfg->audit_log_path); cfg->audit_log_path = str_dup(v); }

    cfg->read_requires_approval = conf_get_int(conf, "safety.read_requires_approval", 0);
    cfg->read_size_threshold = (size_t)conf_get_int(conf, "safety.read_size_threshold", 1048576);
}

static void free_str_array(char ***arr, int *count)
{
    if (!arr || !*arr) return;
    for (int i = 0; i < *count; i++) free((*arr)[i]);
    free(*arr);
    *arr = NULL;
    *count = 0;
}

void safety_config_free(SafetyConfig *cfg)
{
    if (!cfg) return;
    free(cfg->workspace);
    free_str_array(&cfg->allowed_commands, &cfg->allowed_commands_count);
    free_str_array(&cfg->blocked_commands, &cfg->blocked_commands_count);
    free_str_array(&cfg->allowed_extensions, &cfg->allowed_extensions_count);
    free_str_array(&cfg->blocked_extensions, &cfg->blocked_extensions_count);
    free_str_array(&cfg->blocked_paths, &cfg->blocked_paths_count);
    free_str_array(&cfg->allowed_domains, &cfg->allowed_domains_count);
    free_str_array(&cfg->require_approval_for, &cfg->require_approval_count);
    free(cfg->audit_log_path);
    free(cfg);
}

int safety_check_path(const SafetyConfig *cfg, const char *path)
{
    if (!path) return 0;

    if (strstr(path, "..") != NULL) return 0;

    if (cfg->allowed_extensions_count > 0)
    {
        int allowed = 0;
        for (int i = 0; i < cfg->allowed_extensions_count; i++)
        {
            if (str_ends_with(path, cfg->allowed_extensions[i])) { allowed = 1; break; }
        }
        if (!allowed) return 0;
    }

    static const char *default_blocked_exts[] = {
        ".key", ".pem", ".env", ".token", ".password",
        ".aws", ".netrc", ".htpasswd", ".crt", ".p12", NULL
    };

    for (int i = 0; default_blocked_exts[i]; i++)
    {
        if (str_ends_with(path, default_blocked_exts[i])) return 0;
    }

    for (int i = 0; i < cfg->blocked_extensions_count; i++)
    {
        if (str_ends_with(path, cfg->blocked_extensions[i])) return 0;
    }

    static const char *default_blocked_paths[] = {
        "/etc/passwd", "/etc/shadow", "/etc/sudoers",
        ".git/config", NULL
    };

    for (int i = 0; default_blocked_paths[i]; i++)
    {
        if (strstr(path, default_blocked_paths[i]) != NULL) return 0;
    }

    for (int i = 0; i < cfg->blocked_paths_count; i++)
    {
        if (strstr(path, cfg->blocked_paths[i]) != NULL) return 0;
    }

    return 1;
}

static const char *dangerous_patterns[] = {
    "rm -rf /", "rm -rf /*",
    ":(){ :|:& };:", "fork()",
    "wget", "curl",
    "mkfs.", "mkswap",
    "dd if=", "dd if=/dev/zero", "dd if=/dev/urandom",
    "shred",
    "> /dev/sda", "> /dev/sdb", "> /dev/nvme",
    "pv < /dev/sda", "pv < /dev/sdb",
    "debugfs", "hdparm",
    "mount -o loop", "losetup",
    "parted", "fdisk", "cfdisk", "sfdisk",
    "chmod 777", "chmod -R 777",
    "sudo rm", "sudo rm -rf",
    NULL
};

int safety_check_command(const SafetyConfig *cfg, const char *command)
{
    (void)cfg;
    if (!command) return 1;

    StrArray parts = str_split(command, ';');
    for (int i = 0; i < parts.count; i++)
    {
        char *trimmed = str_trim(parts.items[i]);
        if (!trimmed || trimmed[0] == '\0') continue;

        StrArray sub = str_split(trimmed, '|');
        for (int j = 0; j < sub.count; j++)
        {
            char *st = str_trim(sub.items[j]);
            if (!st || st[0] == '\0') continue;

            for (int k = 0; dangerous_patterns[k]; k++)
            {
                if (strstr(st, dangerous_patterns[k]) != NULL)
                {
                    str_array_free(&sub);
                    str_array_free(&parts);
                    return 0;
                }
            }
        }
        str_array_free(&sub);

        StrArray and_sub = str_split(trimmed, '&');
        for (int j = 0; j < and_sub.count; j++)
        {
            if (and_sub.items[j][0] == '&') continue;
            char *st = str_trim(and_sub.items[j]);
            if (!st || st[0] == '\0') continue;

            for (int k = 0; dangerous_patterns[k]; k++)
            {
                if (strstr(st, dangerous_patterns[k]) != NULL)
                {
                    str_array_free(&and_sub);
                    str_array_free(&parts);
                    return 0;
                }
            }
        }
        str_array_free(&and_sub);
    }

    str_array_free(&parts);
    return 1;
}

int safety_check_destructive(const char *command)
{
    static const char *keywords[] = {
        "delete", "destroy", "format", "drop", "truncate",
        "shred", "wipe", "erase", "purge", "reset",
        NULL
    };
    if (!command) return 0;
    for (int i = 0; keywords[i]; i++)
    {
        if (strstr(command, keywords[i]) != NULL) return 1;
    }
    return 0;
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

int safety_needs_approval(const SafetyConfig *cfg, const char *tool_name)
{
    if (!cfg || !tool_name) return 1;
    for (int i = 0; i < cfg->require_approval_count; i++)
    {
        if (strcmp(cfg->require_approval_for[i], tool_name) == 0) return 1;
    }
    return 0;
}

int safety_audit_log(const SafetyConfig *cfg, const char *entry)
{
    if (!cfg || !cfg->audit_log_path || !entry) return -1;

    FILE *f = fopen(cfg->audit_log_path, "a");
    if (!f) return -1;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm);

    fprintf(f, "{\"timestamp\":\"%s\",\"entry\":%s}\n", ts, entry);
    fclose(f);
    return 0;
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
