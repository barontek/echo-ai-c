/*
 * safety.h - path, URL, command, and network-address safety checks, plus
 * approval gating and audit logging for tools.
 * Depends on: <stddef.h>, <sys/socket.h>, config (Conf).
 */

#ifndef ECHO_SAFETY_H
#define ECHO_SAFETY_H

#include <stddef.h>
#include <sys/socket.h>

typedef struct Conf Conf;

/*
 * SafetyMode - global policy strictness dial, loaded from safety.mode.
 *
 * SAFETY_MODE_RESTRICTED: policy checks (paths, commands, URLs, sockets,
 * size) and the require_approval_for approval list — the historical
 * behavior, and the default.
 *
 * SAFETY_MODE_APPROVE_ALL: policy checks stay on, but every tool call
 * goes through the human approval gate, ignoring require_approval_for.
 *
 * SAFETY_MODE_UNRESTRICTED: all policy checks and approval gates are
 * bypassed (paths, commands, URLs, sockets, file size, workspace
 * pinning). Process-protecting resource guards still apply: bash
 * max_execution_time, tool-result truncation, web_fetch_max_chars, and
 * the audit log still run.
 */
typedef enum {
    SAFETY_MODE_RESTRICTED = 0,
    SAFETY_MODE_APPROVE_ALL,
    SAFETY_MODE_UNRESTRICTED
} SafetyMode;

typedef struct {
    char *workspace;
    SafetyMode mode;
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
    size_t web_fetch_max_chars;
} SafetyConfig;

/**
 * safety_config_create - allocate a safety config with built-in defaults
 *
 * Defaults: max_file_size 10 MiB, max_execution_time 300 s, allow_network
 * on, read_size_threshold 1 MiB, web_fetch_max_chars 25000, mode
 * SAFETY_MODE_RESTRICTED; every string list starts empty. Populate it
 * with safety_load_from_conf() before use.
 *
 * Return: caller-owned SafetyConfig (free with safety_config_free()), or
 * NULL on allocation failure. Thread-safe; no shared state.
 */
SafetyConfig *safety_config_create(void);

/**
 * safety_config_free - release a safety config and every owned list
 * @cfg: config to release, or NULL (no-op).
 *
 * Frees workspace, the command/extension/domain/path/approval string
 * lists, audit_log_path, and cfg itself.
 *
 * Return: void. Thread-safe; no shared state.
 */
void safety_config_free(SafetyConfig *cfg);

/**
 * safety_load_from_conf - (re)load a safety config from the Conf
 * @cfg: config to populate; must be non-NULL.
 * @conf: config source, borrowed for the duration of the call; must be
 *   non-NULL.
 *
 * Reads the safety.* keys (workspace, mode, allow_network, max_file_size,
 * max_execution_time, allowed/blocked_commands, allowed/blocked_extensions,
 * blocked_paths, allowed_domains, require_approval_for, audit_log_path,
 * read_requires_approval, read_size_threshold, web_fetch_max_chars) and
 * replaces previously loaded values. Keys absent from conf keep their
 * current values. safety.mode accepts "restricted", "approve_all", or
 * "unrestricted" (any other value is ignored and keeps the current mode).
 * When no require_approval_for list is configured, a default list (bash,
 * write_file, edit, git, python_execute, delegate) is
 * installed.
 *
 * Return: 0 on success; -1 when any policy field could not be allocated
 * (workspace/audit path dup or list parse) — the config is then
 * incomplete and the caller should abort with context rather than run
 * with a silently weakened safety policy. Not thread-safe with respect
 * to concurrent use of the same cfg.
 */
int safety_load_from_conf(SafetyConfig *cfg, const Conf *conf);

/**
 * safety_check_path - reject unsafe paths before file access
 * @cfg: safety config; must be non-NULL (dereferenced without a NULL
 *   check).
 * @path: path to check, borrowed; NULL is rejected.
 *
 * A syntactic, not filesystem, check: rejects any path containing "..",
 * paths not ending in an allowed extension (when allowed_extensions is
 * configured), paths ending in a blocked extension, and paths containing a
 * blocked path. Absolute paths are allowed — the workspace pinning in
 * safety_resolve_path_alloc()/safety_path_is_within_workspace() is the
 * real security boundary, and every file tool calls it after this check.
 *
 * Blocked-extension/path defaults (.key .pem .env .token .password .aws
 * .netrc .htpasswd .crt .p12; /etc/passwd /etc/shadow /etc/sudoers
 * .git/config) apply only while the corresponding config list is empty:
 * configuring cfg->blocked_extensions or cfg->blocked_paths replaces the
 * defaults instead of extending them. In SAFETY_MODE_UNRESTRICTED every
 * path passes. Use
 * safety_resolve_path_alloc()/safety_path_is_within_workspace() when
 * realpath-resolved checks are needed.
 *
 * Return: 1 if allowed, 0 if blocked. Never fails; thread-safe.
 */
int safety_check_path(const SafetyConfig *cfg, const char *path);

/**
 * safety_check_command - best-effort blocklist check for destructive commands
 * @cfg: ignored by the check; must be non-NULL to keep the signature
 *   consistent with the rest of the safety API.
 * @command: shell command string to check; NULL is treated as allowed.
 *
 * Returns 0 (blocked) when a segment — the command split on ';', '\n',
 * '|', and '&' — matches a known dangerous pattern; 1 (allowed)
 * otherwise. In SAFETY_MODE_UNRESTRICTED every command passes.
 *
 * This is NOT a security boundary. It cannot catch command substitution
 * ($(...)), variable expansion, encoded payloads, or scripting-language
 * invocations. The real safety boundary is safety_needs_approval(), which
 * requires human approval for bash, write_file, edit, git,
 * python_execute, and delegate by default. This function exists only to
 * catch the *obvious* destructive cases so the approver sees a prompt — it
 * offers no guarantee that every dangerous command is blocked.
 *
 * Return: 1 if allowed, 0 if blocked. Never fails; thread-safe.
 */
int safety_check_command(const SafetyConfig *cfg, const char *command);

/**
 * safety_check_destructive - detect destructive keywords in a command
 * @command: text to scan, or NULL (treated as not destructive).
 *
 * Case-sensitive substring scan for "delete", "destroy", "format", "drop",
 * "truncate", "shred", "wipe", "erase", "purge", "reset".
 *
 * Return: 1 if any keyword occurs, 0 otherwise. Never fails; thread-safe.
 */
int safety_check_destructive(const char *command);

/**
 * safety_check_url - allow or block an http(s) URL by host
 * @cfg: safety config; NULL (or allow_network off) blocks everything.
 * @url: URL to check, borrowed; NULL is blocked.
 *
 * Only http:// and https:// schemes are accepted. Rejects userinfo
 * ("user@host"), empty hosts, localhost and *.localhost, IPv6 loopback,
 * link-local (fe80:) and unique-local (fc/fd prefix) hosts, and private
 * IPv4 ranges (10/8, 127/8, 0/8, 169.254/16, 192.168/16, 172.16/12) —
 * the numeric range checks apply only when the whole host parses as a
 * dotted quad. When allowed_domains is configured the host must match one
 * entry exactly or as a subdomain; otherwise every remaining host is
 * allowed.
 *
 * The host is matched literally; no DNS resolution is performed, so a
 * hostname that resolves to a private address is not caught here. ULA
 * detection is approximated by the literal fc/fd prefix, so hostnames
 * starting with "fc" or "fd" are also rejected. In SAFETY_MODE_UNRESTRICTED
 * every URL is accepted.
 *
 * Return: 1 if allowed, 0 if blocked. Never fails; thread-safe.
 */
int safety_check_url(const SafetyConfig *cfg, const char *url);

/**
 * safety_check_socket_address - reject non-public network addresses
 * @cfg: safety config; NULL dereferenced only in SAFETY_MODE_UNRESTRICTED
 *   — pass a live config.
 * @address: sockaddr to inspect, borrowed; NULL returns 0. Only AF_INET
 *   and AF_INET6 are handled.
 *
 * Returns 1 for globally routable addresses, 0 for private, loopback,
 * link-local, unique-local, multicast, or unspecified addresses, and for
 * any other family. IPv4-mapped IPv6 addresses are evaluated as IPv4. In
 * SAFETY_MODE_UNRESTRICTED every address is accepted.
 *
 * Return: 1 if public, 0 otherwise. Never fails; thread-safe.
 */
int safety_check_socket_address(const SafetyConfig *cfg,
                                const struct sockaddr *address);

/**
 * safety_check_file_size - compare a size against cfg->max_file_size
 * @cfg: safety config; must be non-NULL (dereferenced without a NULL
 *   check).
 * @size: size in bytes to check.
 *
 * Return: 1 when size <= max_file_size, 0 otherwise. In
 * SAFETY_MODE_UNRESTRICTED every size passes. Never fails; thread-safe.
 */
int safety_check_file_size(const SafetyConfig *cfg, size_t size);

/**
 * safety_needs_approval - does a tool require human approval?
 * @cfg: safety config; NULL returns 1 (fail closed).
 * @tool_name: tool name to look up in require_approval_for; NULL returns 1.
 *
 * In SAFETY_MODE_APPROVE_ALL every tool name returns 1; in
 * SAFETY_MODE_UNRESTRICTED every tool name returns 0.
 *
 * Return: 1 when the tool is in the approval list or when either argument
 * is NULL, 0 otherwise. Never fails; thread-safe.
 */
int safety_needs_approval(const SafetyConfig *cfg, const char *tool_name);

/**
 * safety_allow_tool_always - stop asking for approval for a tool
 * @cfg: safety config; non-NULL.
 * @tool_name: tool name to drop from require_approval_for; non-NULL.
 *
 * Runtime-only, matching the "allow until restarted" semantics: removes
 * the tool from the in-memory approval list so future calls skip the
 * approval gate. The on-disk config is untouched. No-op in
 * SAFETY_MODE_APPROVE_ALL (every tool still prompts) and
 * SAFETY_MODE_UNRESTRICTED (nothing prompts anyway).
 *
 * Return: 1 when the tool was removed (previously approval-required),
 * 0 when it was not in the list, -1 on NULL arguments.
 */
int safety_allow_tool_always(SafetyConfig *cfg, const char *tool_name);

/**
 * safety_audit_log - append a JSON line to the audit log file
 * @cfg: safety config; must be non-NULL.
 * @entry: JSON text embedded verbatim as the "entry" field — callers must
 *   pre-escape it (the function does no JSON escaping). NULL returns -1.
 *
 * Appends {"timestamp":"<ISO-8601>","entry":<entry>} to cfg->audit_log_path
 * (created if missing). Timestamps come from localtime(), which is
 * non-reentrant — do not call this from multiple threads.
 *
 * Return: 0 on success, -1 when cfg or entry is NULL, audit_log_path is
 * not set, or the file cannot be opened for appending. The failure is not
 * logged.
 */
int safety_audit_log(const SafetyConfig *cfg, const char *entry);

/**
 * safety_resolve_path_alloc - canonicalize a path and pin it inside the workspace
 * @cfg: safety config; must be non-NULL and have workspace set.
 * @path: path to resolve (absolute, or relative to the workspace root),
 *   borrowed; NULL, or containing "..", returns NULL.
 *
 * For existing files the canonical (symlink-resolved) path must lie inside
 * the workspace. For not-yet-created files the parent directory is
 * resolved instead and must be inside the workspace; the basename is then
 * re-attached, so writes to new files still validate.
 *
 * In SAFETY_MODE_UNRESTRICTED the path is duplicated verbatim — no
 * realpath, no workspace pinning — so anything reachable by the process
 * can be addressed.
 *
 * Return: caller-owned null-terminated path (free with free()), or NULL on
 * any failure: bad arguments, realpath failure, outside the workspace, or
 * allocation failure. Thread-safe; no shared state.
 */
char *safety_resolve_path_alloc(const SafetyConfig *cfg, const char *path);

/**
 * safety_path_is_within_workspace - is the canonical path under the workspace?
 * @cfg: safety config; must be non-NULL and have workspace set.
 * @path: path to check, borrowed; NULL returns 0.
 *
 * Both the workspace and the path are canonicalized with realpath(), so
 * symlinks resolve. The path counts as inside when it equals the workspace
 * root or starts with root + '/'. In SAFETY_MODE_UNRESTRICTED every
 * non-NULL path counts as inside.
 *
 * Return: 1 when inside, 0 otherwise (including NULL args or realpath
 * failure). Never fails; thread-safe.
 */
int safety_path_is_within_workspace(const SafetyConfig *cfg, const char *path);

#endif
