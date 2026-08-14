#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <arpa/inet.h>
#include "safety/safety.h"
#include "config/config.h"
#include "utils/string_utils.h"

/* test_safety - unit tests for safety. Depends on: check, the module under test. */
/* --- safety_config_create / safety_config_free --- */

START_TEST(test_safety_config_create_defaults)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_ptr_nonnull(cfg);
    ck_assert(cfg->max_file_size == 10485760);
    ck_assert_int_eq(cfg->max_execution_time, 300);
    ck_assert_int_eq(cfg->allow_network, 1);
    ck_assert_int_eq(cfg->read_requires_approval, 0);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_safety_config_free_null)
{
    safety_config_free(NULL);
}
END_TEST

/* --- safety_check_path --- */

START_TEST(test_path_null_returns_zero)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_path(cfg, NULL), 0);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_path_with_dotdot_rejected)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_path(cfg, "../etc/passwd"), 0);
    ck_assert_int_eq(safety_check_path(cfg, "foo/../../../bar"), 0);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_path_allowed_when_no_extension_filter)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_path(cfg, "tmp/file.txt"), 1);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_path_with_allowed_extension_filter)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->allowed_extensions = malloc(sizeof(char *));
    cfg->allowed_extensions[0] = str_dup(".txt");
    cfg->allowed_extensions_count = 1;
    ck_assert_int_eq(safety_check_path(cfg, "file.txt"), 1);
    ck_assert_int_eq(safety_check_path(cfg, "file.md"), 0);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_path_blocked_sensitive_extension)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_path(cfg, "secret.key"), 0);
    ck_assert_int_eq(safety_check_path(cfg, "config.env"), 0);
    ck_assert_int_eq(safety_check_path(cfg, ".aws"), 0);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_path_user_blocked_extension)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->blocked_extensions = malloc(sizeof(char *));
    cfg->blocked_extensions[0] = str_dup(".exe");
    cfg->blocked_extensions_count = 1;
    ck_assert_int_eq(safety_check_path(cfg, "program.exe"), 0);
    ck_assert_int_eq(safety_check_path(cfg, "script.sh"), 1);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_path_blocked_default_paths)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_path(cfg, "/etc/passwd"), 0);
    ck_assert_int_eq(safety_check_path(cfg, "path/.git/config"), 0);
    ck_assert_int_eq(safety_check_path(cfg, "/etc/shadow"), 0);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_path_user_blocked_paths)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->blocked_paths = malloc(sizeof(char *));
    cfg->blocked_paths[0] = str_dup("secret_dir");
    cfg->blocked_paths_count = 1;
    ck_assert_int_eq(safety_check_path(cfg, "path/secret_dir/file"), 0);
    ck_assert_int_eq(safety_check_path(cfg, "normal/file"), 1);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_path_clean_passes)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_path(cfg, "src/main.c"), 1);
    ck_assert_int_eq(safety_check_path(cfg, "tmp/test.txt"), 1);
    ck_assert_int_eq(safety_check_path(cfg, "README.md"), 1);
    safety_config_free(cfg);
}
END_TEST

/* The syntactic check no longer rejects absolute paths: the workspace
 * pinning in safety_resolve_path_alloc() is the real boundary (every file
 * tool calls it right after this check). Regression test for the "agent
 * cannot read files by absolute path" complaint. */
START_TEST(test_path_absolute_allowed)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_path(cfg, "/home/user/repo/src/main.c"), 1);
    ck_assert_int_eq(safety_check_path(cfg, "/etc/passwd"), 0);
    safety_config_free(cfg);
}
END_TEST

/* A configured blocked_extensions list replaces the hardcoded defaults
 * (.key .pem .env ...) instead of extending them; with an explicit list
 * set, .env files are no longer blocked. */
START_TEST(test_path_configured_blocked_extensions_replace_defaults)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->blocked_extensions = malloc(sizeof(char *));
    cfg->blocked_extensions[0] = str_dup(".exe");
    cfg->blocked_extensions_count = 1;
    ck_assert_int_eq(safety_check_path(cfg, "program.exe"), 0);
    ck_assert_int_eq(safety_check_path(cfg, "config.env"), 1);
    ck_assert_int_eq(safety_check_path(cfg, "secret.key"), 1);
    safety_config_free(cfg);
}
END_TEST

/* Same replace-not-append semantics for blocked_paths: a configured list
 * drops the /etc/passwd /etc/shadow /etc/sudoers .git/config defaults. */
START_TEST(test_path_configured_blocked_paths_replace_defaults)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->blocked_paths = malloc(sizeof(char *));
    cfg->blocked_paths[0] = str_dup("secret_dir");
    cfg->blocked_paths_count = 1;
    ck_assert_int_eq(safety_check_path(cfg, "path/secret_dir/file"), 0);
    ck_assert_int_eq(safety_check_path(cfg, "path/.git/config"), 1);
    safety_config_free(cfg);
}
END_TEST

/* --- safety_check_command --- */

START_TEST(test_command_null_passes)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_command(cfg, NULL), 1);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_command_dangerous_rejected)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_command(cfg, "rm -rf /"), 0);
    ck_assert_int_eq(safety_check_command(cfg, "rm -rf /*"), 0);
    ck_assert_int_eq(safety_check_command(cfg, "sudo rm -rf /data"), 0);
    ck_assert_int_eq(safety_check_command(cfg, "chmod 777 file"), 0);
    ck_assert_int_eq(safety_check_command(cfg, "chmod -R 777 dir"), 0);
    ck_assert_int_eq(safety_check_command(cfg, "mkfs.ext4 /dev/sda"), 0);
    ck_assert_int_eq(safety_check_command(cfg, "dd if=/dev/zero of=disk.img"), 0);
    ck_assert_int_eq(safety_check_command(cfg, "shred file"), 0);
    ck_assert_int_eq(safety_check_command(cfg, "parted /dev/sda"), 0);
    ck_assert_int_eq(safety_check_command(cfg, "fdisk /dev/sda"), 0);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_command_safe_passes)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_command(cfg, "ls -la"), 1);
    ck_assert_int_eq(safety_check_command(cfg, "grep pattern file"), 1);
    ck_assert_int_eq(safety_check_command(cfg, "echo hello"), 1);
    ck_assert_int_eq(safety_check_command(cfg, "cat file.txt"), 1);
    safety_config_free(cfg);
}
END_TEST

/* Download tools are not destructive; the approval gate on bash is the
 * real boundary. Regression test for CI scripts blocked by "curl"/"wget"
 * substring matches. */
START_TEST(test_command_download_tools_allowed)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_command(cfg, "curl -fsSL https://example.com/install.sh"), 1);
    ck_assert_int_eq(safety_check_command(cfg, "wget -O setup https://example.com/setup"), 1);
    ck_assert_int_eq(safety_check_command(cfg, "curl -L https://example.com | tar xz"), 1);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_command_semicolon_chain_dangerous)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_command(cfg, "echo hello; rm -rf /"), 0);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_command_pipe_chain_dangerous)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_command(cfg, "cat file | shred"), 0);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_command_ampersand_chain_dangerous)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_command(cfg, "build && rm -rf /"), 0);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_command_semicolon_chain_safe)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_command(cfg, "echo hello; echo world"), 1);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_command_pipe_chain_safe)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_command(cfg, "cat file | grep pattern"), 1);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_command_newline_chain_dangerous)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_command(cfg, "echo hi\nrm -rf /"), 0);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_command_evasion_patterns_caught)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_command(cfg, "\\rm -rf /home/user"), 0);
    ck_assert_int_eq(safety_check_command(cfg, "/bin/rm important.txt"), 0);
    ck_assert_int_eq(safety_check_command(cfg, "/usr/bin/rm important.txt"), 0);
    ck_assert_int_eq(safety_check_command(cfg, "unlink(file)"), 0);
    ck_assert_int_eq(safety_check_command(cfg, "shutil.rmtree('/tmp/dir')"), 0);
    ck_assert_int_eq(safety_check_command(cfg, "os.remove('/path')"), 0);
    ck_assert_int_eq(safety_check_command(cfg, "os.unlink('/path')"), 0);
    safety_config_free(cfg);
}
END_TEST

/*
 * This test explicitly documents a KNOWN BYPASS that this blocklist cannot
 * catch: command substitution can construct dangerous patterns at runtime
 * by splitting them across expansions, e.g. "$(echo r)$(echo m) -rf /"
 * never contains the literal string "rm" (it's split by ")$("), so
 * strstr() cannot match it against the "rm -rf /" pattern.
 *
 * This is NOT a bug in the split/strstr approach — it is a structural
 * limitation of any blocklist operating on raw command strings.  The
 * safety boundary for these cases is the human-approval gate in
 * safety_needs_approval(), not this function.
 */
START_TEST(test_command_substitution_bypass_documented)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_command(cfg, "$(echo r)$(echo m) -rf /"), 1);
    safety_config_free(cfg);
}
END_TEST

/* --- safety_check_destructive --- */

START_TEST(test_destructive_null_returns_zero)
{
    ck_assert_int_eq(safety_check_destructive(NULL), 0);
}
END_TEST

START_TEST(test_destructive_keywords_detected)
{
    ck_assert_int_eq(safety_check_destructive("delete all records"), 1);
    ck_assert_int_eq(safety_check_destructive("destroy the database"), 1);
    ck_assert_int_eq(safety_check_destructive("format disk"), 1);
    ck_assert_int_eq(safety_check_destructive("drop table users"), 1);
    ck_assert_int_eq(safety_check_destructive("truncate table"), 1);
    ck_assert_int_eq(safety_check_destructive("shred sensitive files"), 1);
    ck_assert_int_eq(safety_check_destructive("wipe the disk"), 1);
    ck_assert_int_eq(safety_check_destructive("erase records"), 1);
    ck_assert_int_eq(safety_check_destructive("purge cache"), 1);
    ck_assert_int_eq(safety_check_destructive("reset password"), 1);
}
END_TEST

START_TEST(test_destructive_safe_returns_zero)
{
    ck_assert_int_eq(safety_check_destructive("read file"), 0);
    ck_assert_int_eq(safety_check_destructive("list directory"), 0);
    ck_assert_int_eq(safety_check_destructive("search text"), 0);
}
END_TEST

/* --- safety_check_url --- */

START_TEST(test_url_network_blocked)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->allow_network = 0;
    ck_assert_int_eq(safety_check_url(cfg, "https://example.com"), 0);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_url_no_domain_filter)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_url(cfg, "https://example.com"), 1);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_url_allowed_domain_passes)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->allowed_domains = malloc(sizeof(char *));
    cfg->allowed_domains[0] = str_dup("example.com");
    cfg->allowed_domains_count = 1;
    ck_assert_int_eq(safety_check_url(cfg, "https://example.com/path"), 1);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_url_unlisted_domain_rejected)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->allowed_domains = malloc(sizeof(char *));
    cfg->allowed_domains[0] = str_dup("example.com");
    cfg->allowed_domains_count = 1;
    ck_assert_int_eq(safety_check_url(cfg, "https://other.com"), 0);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_url_rejects_allowlist_substring_bypass)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->allowed_domains = malloc(sizeof(char *));
    cfg->allowed_domains[0] = str_dup("example.com");
    cfg->allowed_domains_count = 1;
    ck_assert_int_eq(safety_check_url(cfg, "https://example.com.evil.test/"), 0);
    ck_assert_int_eq(safety_check_url(cfg, "https://example.com@evil.test/"), 0);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_url_rejects_non_http_and_local_hosts)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_check_url(cfg, "file:///etc/passwd"), 0);
    ck_assert_int_eq(safety_check_url(cfg, "http://127.0.0.1/"), 0);
    ck_assert_int_eq(safety_check_url(cfg, "http://localhost/"), 0);
    ck_assert_int_eq(safety_check_url(cfg, "http://192.168.1.1/"), 0);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_socket_address_rejects_private_dns_destinations)
{
    SafetyConfig *cfg = safety_config_create();
    struct sockaddr_in private_addr = {0};
    private_addr.sin_family = AF_INET;
    ck_assert_int_eq(inet_pton(AF_INET, "10.2.3.4", &private_addr.sin_addr), 1);
    ck_assert_int_eq(safety_check_socket_address(
                         cfg, (const struct sockaddr *)&private_addr), 0);

    struct sockaddr_in public_addr = {0};
    public_addr.sin_family = AF_INET;
    ck_assert_int_eq(inet_pton(AF_INET, "8.8.8.8", &public_addr.sin_addr), 1);
    ck_assert_int_eq(safety_check_socket_address(
                         cfg, (const struct sockaddr *)&public_addr), 1);

    struct sockaddr_in carrier_grade_nat = {0};
    carrier_grade_nat.sin_family = AF_INET;
    ck_assert_int_eq(inet_pton(AF_INET, "100.100.100.200",
                               &carrier_grade_nat.sin_addr), 1);
    ck_assert_int_eq(safety_check_socket_address(
                         cfg, (const struct sockaddr *)&carrier_grade_nat), 0);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_url_empty_allowlist_entry_does_not_allow_host)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->allowed_domains = calloc(1, sizeof(char *));
    cfg->allowed_domains[0] = str_dup("");
    cfg->allowed_domains_count = 1;
    ck_assert_int_eq(safety_check_url(cfg, "https://evil.test./"), 0);
    safety_config_free(cfg);
}
END_TEST

/* --- safety_check_file_size --- */

START_TEST(test_file_size_within_limit)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->max_file_size = 1024;
    ck_assert_int_eq(safety_check_file_size(cfg, 1000), 1);
    ck_assert_int_eq(safety_check_file_size(cfg, 1024), 1);
    ck_assert_int_eq(safety_check_file_size(cfg, 1025), 0);
    safety_config_free(cfg);
}
END_TEST

/* --- safety_needs_approval --- */

START_TEST(test_approval_null_args)
{
    ck_assert_int_eq(safety_needs_approval(NULL, "bash"), 1);
    ck_assert_int_eq(safety_needs_approval(NULL, NULL), 1);
}
END_TEST

START_TEST(test_approval_default_tools)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->require_approval_for = malloc(sizeof(char *) * 6);
    const char *names[] = {"bash", "write_file", "replace_in_file", "git",
                           "python_execute", "delegate"};
    for (int i = 0; i < 6; i++)
        cfg->require_approval_for[i] = str_dup(names[i]);
    cfg->require_approval_count = 6;

    ck_assert_int_eq(safety_needs_approval(cfg, "bash"), 1);
    ck_assert_int_eq(safety_needs_approval(cfg, "write_file"), 1);
    ck_assert_int_eq(safety_needs_approval(cfg, "delegate"), 1);
    ck_assert_int_eq(safety_needs_approval(cfg, "read_file"), 0);
    ck_assert_int_eq(safety_needs_approval(cfg, "list_dir"), 0);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_approval_empty_list)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(safety_needs_approval(cfg, "bash"), 0);
    safety_config_free(cfg);
}
END_TEST

/* --- safety.mode --- */

START_TEST(test_mode_default_is_restricted)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_int_eq(cfg->mode, SAFETY_MODE_RESTRICTED);
    ck_assert_int_eq(safety_check_path(cfg, "secret.key"), 0);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_mode_unrestricted_bypasses_path_check)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->mode = SAFETY_MODE_UNRESTRICTED;
    ck_assert_int_eq(safety_check_path(cfg, "secret.key"), 1);
    ck_assert_int_eq(safety_check_path(cfg, "../etc/passwd"), 1);
    ck_assert_int_eq(safety_check_path(cfg, "/etc/shadow"), 1);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_mode_unrestricted_bypasses_command_check)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->mode = SAFETY_MODE_UNRESTRICTED;
    ck_assert_int_eq(safety_check_command(cfg, "rm -rf /"), 1);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_mode_unrestricted_bypasses_url_check)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->mode = SAFETY_MODE_UNRESTRICTED;
    cfg->allow_network = 0;
    ck_assert_int_eq(safety_check_url(cfg, "http://localhost/"), 1);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_mode_unrestricted_bypasses_socket_check)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->mode = SAFETY_MODE_UNRESTRICTED;
    struct sockaddr_in private_addr = {0};
    private_addr.sin_family = AF_INET;
    ck_assert_int_eq(inet_pton(AF_INET, "10.2.3.4", &private_addr.sin_addr), 1);
    ck_assert_int_eq(safety_check_socket_address(
                         cfg, (const struct sockaddr *)&private_addr), 1);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_mode_unrestricted_bypasses_file_size)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->mode = SAFETY_MODE_UNRESTRICTED;
    cfg->max_file_size = 1024;
    ck_assert_int_eq(safety_check_file_size(cfg, 10485760), 1);
    safety_config_free(cfg);
}
END_TEST

/* Unrestricted disables approval prompts; NULL args still fail closed. */
START_TEST(test_mode_unrestricted_no_approval_required)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->mode = SAFETY_MODE_UNRESTRICTED;
    ck_assert_int_eq(safety_needs_approval(cfg, "bash"), 0);
    ck_assert_int_eq(safety_needs_approval(cfg, "read_file"), 0);
    ck_assert_int_eq(safety_needs_approval(cfg, NULL), 1);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_mode_unrestricted_resolve_returns_path_verbatim)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->mode = SAFETY_MODE_UNRESTRICTED;
    cfg->workspace = str_dup("/tmp/echo_test_ws");
    char *resolved = safety_resolve_path_alloc(cfg, "../outside/file.txt");
    ck_assert_ptr_nonnull(resolved);
    ck_assert_str_eq(resolved, "../outside/file.txt");
    ck_assert_int_eq(safety_path_is_within_workspace(cfg, "/etc/passwd"), 1);
    free(resolved);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_mode_approve_all_requires_approval_for_every_tool)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->mode = SAFETY_MODE_APPROVE_ALL;
    ck_assert_int_eq(safety_needs_approval(cfg, "bash"), 1);
    ck_assert_int_eq(safety_needs_approval(cfg, "read_file"), 1);
    ck_assert_int_eq(safety_needs_approval(cfg, "list_dir"), 1);
    safety_config_free(cfg);
}
END_TEST

/* approve_all is stricter, not looser: policy checks stay on so the
 * approver still sees obvious-destructive flags. */
START_TEST(test_mode_approve_all_keeps_policy_checks)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->mode = SAFETY_MODE_APPROVE_ALL;
    ck_assert_int_eq(safety_check_path(cfg, "secret.key"), 0);
    ck_assert_int_eq(safety_check_command(cfg, "rm -rf /"), 0);
    safety_config_free(cfg);
}
END_TEST

/* --- safety_resolve_path_alloc --- */

static char resolved_ws_base[PATH_MAX];

static void setup_workspace(void)
{
    mkdir("/tmp/echo_test_ws", 0755);
    mkdir("/tmp/echo_test_ws/subdir", 0755);
    FILE *f = fopen("/tmp/echo_test_ws/existing.txt", "w");
    if (f) fclose(f);
    f = fopen("/tmp/echo_test_ws/subdir/existing.txt", "w");
    if (f) fclose(f);
    if (realpath("/tmp/echo_test_ws", resolved_ws_base) == NULL)
        ck_assert_uint_lt(strlcpy(resolved_ws_base, "/tmp/echo_test_ws",
                                 sizeof(resolved_ws_base)), sizeof(resolved_ws_base));
}

static void teardown_workspace(void)
{
    ck_assert_int_eq(remove("/tmp/echo_test_ws/subdir/existing.txt"), 0);
    ck_assert_int_eq(remove("/tmp/echo_test_ws/existing.txt"), 0);
    ck_assert_int_eq(remove("/tmp/echo_test_ws/subdir"), 0);
    ck_assert_int_eq(remove("/tmp/echo_test_ws"), 0);
    unlink("/tmp/echo_test_sym");
}

START_TEST(test_resolve_null_workspace)
{
    SafetyConfig *cfg = safety_config_create();
    ck_assert_ptr_null(safety_resolve_path_alloc(cfg, "file.txt"));
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_resolve_null_path)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->workspace = str_dup("/tmp/echo_test_ws");
    ck_assert_ptr_null(safety_resolve_path_alloc(cfg, NULL));
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_resolve_existing_file_inside_workspace)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->workspace = str_dup(resolved_ws_base);
    char *expected = NULL;
    ck_assert_int_ne(asprintf(&expected, "%s/existing.txt", resolved_ws_base), -1);
    char *resolved = safety_resolve_path_alloc(cfg, "existing.txt");
    ck_assert_ptr_nonnull(resolved);
    ck_assert_str_eq(resolved, expected);
    free(expected);
    free(resolved);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_resolve_existing_nested_file_inside_workspace)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->workspace = str_dup(resolved_ws_base);
    char *expected = NULL;
    ck_assert_int_ne(asprintf(&expected, "%s/subdir/existing.txt", resolved_ws_base), -1);
    char *resolved = safety_resolve_path_alloc(cfg, "subdir/existing.txt");
    ck_assert_ptr_nonnull(resolved);
    ck_assert_str_eq(resolved, expected);
    free(expected);
    free(resolved);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_resolve_absolute_path_outside_workspace)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->workspace = str_dup("/tmp/echo_test_ws");
    char *resolved = safety_resolve_path_alloc(cfg, "/etc/hostname");
    ck_assert_ptr_null(resolved);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_resolve_dotdot_escape_rejected)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->workspace = str_dup("/tmp/echo_test_ws");
    char *resolved = safety_resolve_path_alloc(cfg, "subdir/../../etc/hostname");
    ck_assert_ptr_null(resolved);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_resolve_symlink_outside_workspace_rejected)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->workspace = str_dup("/tmp/echo_test_ws");

    int rc = symlink("/tmp", "/tmp/echo_test_ws/escape_link");
    ck_assert_int_eq(rc, 0);

    char *resolved = safety_resolve_path_alloc(cfg, "escape_link");
    ck_assert_ptr_null(resolved);

    unlink("/tmp/echo_test_ws/escape_link");
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_resolve_nonexistent_file_inside_workspace)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->workspace = str_dup(resolved_ws_base);
    char *expected = NULL;
    ck_assert_int_ne(asprintf(&expected, "%s/newfile.txt", resolved_ws_base), -1);
    char *resolved = safety_resolve_path_alloc(cfg, "newfile.txt");
    ck_assert_ptr_nonnull(resolved);
    ck_assert_str_eq(resolved, expected);
    free(expected);
    free(resolved);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_resolve_nonexistent_parent_rejected)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->workspace = str_dup("/tmp/echo_test_ws");
    char *resolved = safety_resolve_path_alloc(cfg, "nosuchdir/newfile.txt");
    ck_assert_ptr_null(resolved);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_resolve_absolute_inside_workspace_resolves)
{
    SafetyConfig *cfg = safety_config_create();
    cfg->workspace = str_dup(resolved_ws_base);
    char *abspath = NULL;
    char *expected = NULL;
    ck_assert_int_ne(asprintf(&abspath, "%s/existing.txt", resolved_ws_base), -1);
    ck_assert_int_ne(asprintf(&expected, "%s/existing.txt", resolved_ws_base), -1);
    char *resolved = safety_resolve_path_alloc(cfg, abspath);
    ck_assert_ptr_nonnull(resolved);
    ck_assert_str_eq(resolved, expected);
    free(abspath);
    free(expected);
    free(resolved);
    safety_config_free(cfg);
}
END_TEST

/* --- safety_load_from_conf --- */

Suite *safety_suite(void)
{
    Suite *s = suite_create("Safety");
    TCase *tc = tcase_create("ConfigLifecycle");
    tcase_set_timeout(tc, 10);
    tcase_add_test(tc, test_safety_config_create_defaults);
    tcase_add_test(tc, test_safety_config_free_null);
    suite_add_tcase(s, tc);

    TCase *tc_path = tcase_create("Path");
    tcase_set_timeout(tc_path, 10);
    tcase_add_test(tc_path, test_path_null_returns_zero);
    tcase_add_test(tc_path, test_path_with_dotdot_rejected);
    tcase_add_test(tc_path, test_path_allowed_when_no_extension_filter);
    tcase_add_test(tc_path, test_path_with_allowed_extension_filter);
    tcase_add_test(tc_path, test_path_blocked_sensitive_extension);
    tcase_add_test(tc_path, test_path_user_blocked_extension);
    tcase_add_test(tc_path, test_path_blocked_default_paths);
    tcase_add_test(tc_path, test_path_user_blocked_paths);
    tcase_add_test(tc_path, test_path_clean_passes);
    tcase_add_test(tc_path, test_path_absolute_allowed);
    tcase_add_test(tc_path, test_path_configured_blocked_extensions_replace_defaults);
    tcase_add_test(tc_path, test_path_configured_blocked_paths_replace_defaults);
    suite_add_tcase(s, tc_path);

    TCase *tc_cmd = tcase_create("Command");
    tcase_set_timeout(tc_cmd, 10);
    tcase_add_test(tc_cmd, test_command_null_passes);
    tcase_add_test(tc_cmd, test_command_dangerous_rejected);
    tcase_add_test(tc_cmd, test_command_safe_passes);
    tcase_add_test(tc_cmd, test_command_download_tools_allowed);
    tcase_add_test(tc_cmd, test_command_semicolon_chain_dangerous);
    tcase_add_test(tc_cmd, test_command_pipe_chain_dangerous);
    tcase_add_test(tc_cmd, test_command_ampersand_chain_dangerous);
    tcase_add_test(tc_cmd, test_command_semicolon_chain_safe);
    tcase_add_test(tc_cmd, test_command_pipe_chain_safe);
    tcase_add_test(tc_cmd, test_command_newline_chain_dangerous);
    tcase_add_test(tc_cmd, test_command_evasion_patterns_caught);
    tcase_add_test(tc_cmd, test_command_substitution_bypass_documented);
    suite_add_tcase(s, tc_cmd);

    TCase *tc_dest = tcase_create("Destructive");
    tcase_set_timeout(tc_dest, 10);
    tcase_add_test(tc_dest, test_destructive_null_returns_zero);
    tcase_add_test(tc_dest, test_destructive_keywords_detected);
    tcase_add_test(tc_dest, test_destructive_safe_returns_zero);
    suite_add_tcase(s, tc_dest);

    TCase *tc_url = tcase_create("URL");
    tcase_set_timeout(tc_url, 10);
    tcase_add_test(tc_url, test_url_network_blocked);
    tcase_add_test(tc_url, test_url_no_domain_filter);
    tcase_add_test(tc_url, test_url_allowed_domain_passes);
    tcase_add_test(tc_url, test_url_unlisted_domain_rejected);
    tcase_add_test(tc_url, test_url_rejects_allowlist_substring_bypass);
    tcase_add_test(tc_url, test_url_rejects_non_http_and_local_hosts);
    tcase_add_test(tc_url, test_socket_address_rejects_private_dns_destinations);
    tcase_add_test(tc_url, test_url_empty_allowlist_entry_does_not_allow_host);
    suite_add_tcase(s, tc_url);

    TCase *tc_fs = tcase_create("FileSize");
    tcase_set_timeout(tc_fs, 10);
    tcase_add_test(tc_fs, test_file_size_within_limit);
    suite_add_tcase(s, tc_fs);

    TCase *tc_app = tcase_create("Approval");
    tcase_set_timeout(tc_app, 10);
    tcase_add_test(tc_app, test_approval_null_args);
    tcase_add_test(tc_app, test_approval_default_tools);
    tcase_add_test(tc_app, test_approval_empty_list);
    suite_add_tcase(s, tc_app);

    TCase *tc_mode = tcase_create("Mode");
    tcase_set_timeout(tc_mode, 10);
    tcase_add_test(tc_mode, test_mode_default_is_restricted);
    tcase_add_test(tc_mode, test_mode_unrestricted_bypasses_path_check);
    tcase_add_test(tc_mode, test_mode_unrestricted_bypasses_command_check);
    tcase_add_test(tc_mode, test_mode_unrestricted_bypasses_url_check);
    tcase_add_test(tc_mode, test_mode_unrestricted_bypasses_socket_check);
    tcase_add_test(tc_mode, test_mode_unrestricted_bypasses_file_size);
    tcase_add_test(tc_mode, test_mode_unrestricted_no_approval_required);
    tcase_add_test(tc_mode, test_mode_unrestricted_resolve_returns_path_verbatim);
    tcase_add_test(tc_mode, test_mode_approve_all_requires_approval_for_every_tool);
    tcase_add_test(tc_mode, test_mode_approve_all_keeps_policy_checks);
    suite_add_tcase(s, tc_mode);

    TCase *tc_res = tcase_create("Resolve");
    tcase_set_timeout(tc_res, 10);
    tcase_add_checked_fixture(tc_res, setup_workspace, teardown_workspace);
    tcase_add_test(tc_res, test_resolve_null_workspace);
    tcase_add_test(tc_res, test_resolve_null_path);
    tcase_add_test(tc_res, test_resolve_existing_file_inside_workspace);
    tcase_add_test(tc_res, test_resolve_existing_nested_file_inside_workspace);
    tcase_add_test(tc_res, test_resolve_absolute_path_outside_workspace);
    tcase_add_test(tc_res, test_resolve_dotdot_escape_rejected);
    tcase_add_test(tc_res, test_resolve_symlink_outside_workspace_rejected);
    tcase_add_test(tc_res, test_resolve_nonexistent_file_inside_workspace);
    tcase_add_test(tc_res, test_resolve_nonexistent_parent_rejected);
    tcase_add_test(tc_res, test_resolve_absolute_inside_workspace_resolves);
    suite_add_tcase(s, tc_res);


    return s;
}

int main(void)
{
    Suite *s = safety_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
