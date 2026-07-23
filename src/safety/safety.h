#ifndef ECHO_SAFETY_H
#define ECHO_SAFETY_H

#include <stddef.h>

typedef struct {
    char *workspace;
    char **allowed_commands;
    int allowed_commands_count;
    char **blocked_commands;
    int blocked_commands_count;
    char **blocked_extensions;
    int blocked_extensions_count;
    char **blocked_paths;
    int blocked_paths_count;
    int allow_network;
    char **allowed_domains;
    int allowed_domains_count;
    size_t max_file_size;
    int max_execution_time;
} SafetyConfig;

SafetyConfig *safety_config_create(void);
void safety_config_free(SafetyConfig *cfg);

int safety_check_path(const SafetyConfig *cfg, const char *path);
int safety_check_command(const SafetyConfig *cfg, const char *command);
int safety_check_url(const SafetyConfig *cfg, const char *url);
int safety_check_file_size(const SafetyConfig *cfg, size_t size);

char *safety_resolve_path(const SafetyConfig *cfg, const char *path);

#endif
