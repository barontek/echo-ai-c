#ifndef ECHO_SAFETY_H
#define ECHO_SAFETY_H

#include <stddef.h>
#include <sys/socket.h>

typedef struct Conf Conf;

typedef struct {
    char *workspace;
    char **allowed_commands;
    int allowed_commands_count;
    char **blocked_commands;
    int blocked_commands_count;
    char **allowed_extensions;
    int allowed_extensions_count;
    char **blocked_extensions;
    int blocked_extensions_count;
    char **blocked_paths;
    int blocked_paths_count;
    int allow_network;
    char **allowed_domains;
    int allowed_domains_count;
    size_t max_file_size;
    int max_execution_time;
    char **require_approval_for;
    int require_approval_count;
    char *audit_log_path;
    int read_requires_approval;
    size_t read_size_threshold;
} SafetyConfig;

SafetyConfig *safety_config_create(void);
void safety_config_free(SafetyConfig *cfg);
void safety_load_from_conf(SafetyConfig *cfg, const Conf *conf);

int safety_check_path(const SafetyConfig *cfg, const char *path);
/*
 * Best-effort blocklist against obviously destructive commands.  Returns 0
 * (blocked) when the command matches a known dangerous pattern after
 * splitting on ';', '\n', '|', and '&'.  Returns 1 (allowed) for safe
 * commands.
 *
 * This is NOT a security boundary.  It cannot catch command substitution
 * ($(...)), variable expansion, encoded payloads, or scripting-language
 * invocations.  The real safety boundary is safety_needs_approval(), which
 * requires human approval for bash, write_file, replace_in_file, git,
 * python_execute, and delegate by default.  This function exists only to
 * catch the *obvious* destructive cases so the approver sees a prompt — it
 * offers no guarantee that every dangerous command is blocked.
 *
 * Caller must provide a non-NULL cfg (the command check ignores it, but
 * the signature is kept consistent with the rest of the safety API).
 */
int safety_check_command(const SafetyConfig *cfg, const char *command);
int safety_check_destructive(const char *command);
int safety_check_url(const SafetyConfig *cfg, const char *url);
int safety_check_socket_address(const struct sockaddr *address);
int safety_check_file_size(const SafetyConfig *cfg, size_t size);
int safety_needs_approval(const SafetyConfig *cfg, const char *tool_name);

int safety_audit_log(const SafetyConfig *cfg, const char *entry);

char *safety_resolve_path(const SafetyConfig *cfg, const char *path);
int safety_path_is_within_workspace(const SafetyConfig *cfg, const char *path);

#endif
