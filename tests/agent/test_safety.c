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
    struct sockaddr_in private_addr = {0};
    private_addr.sin_family = AF_INET;
    ck_assert_int_eq(inet_pton(AF_INET, "10.2.3.4", &private_addr.sin_addr), 1);
    ck_assert_int_eq(safety_check_socket_address(
                         (const struct sockaddr *)&private_addr), 0);

    struct sockaddr_in public_addr = {0};
    public_addr.sin_family = AF_INET;
    ck_assert_int_eq(inet_pton(AF_INET, "8.8.8.8", &public_addr.sin_addr), 1);
    ck_assert_int_eq(safety_check_socket_address(
                         (const struct sockaddr *)&public_addr), 1);

    struct sockaddr_in carrier_grade_nat = {0};
    carrier_grade_nat.sin_family = AF_INET;
    ck_assert_int_eq(inet_pton(AF_INET, "100.100.100.200",
                               &carrier_grade_nat.sin_addr), 1);
    ck_assert_int_eq(safety_check_socket_address(
                         (const struct sockaddr *)&carrier_grade_nat), 0);
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
    remove("/tmp/echo_test_ws/subdir/existing.txt");
    remove("/tmp/echo_test_ws/existing.txt");
    remove("/tmp/echo_test_ws/subdir");
    remove("/tmp/echo_test_ws");
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

static void write_conf_file(const char *path, const char *content)
{
    FILE *fp = fopen(path, "w");
    ck_assert_ptr_nonnull(fp);
    fprintf(fp, "%s", content);
    fclose(fp);
}

START_TEST(test_safety_load_from_conf_workspace)
{
    write_conf_file("/tmp/test_safety_ws.conf",
        "safety.workspace = /test/workspace\n");
    Conf *conf = conf_load("/tmp/test_safety_ws.conf");
    ck_assert_ptr_nonnull(conf);
    SafetyConfig *cfg = safety_config_create();
    safety_load_from_conf(cfg, conf);
    ck_assert_ptr_nonnull(cfg->workspace);
    ck_assert_str_eq(cfg->workspace, "/test/workspace");
    safety_config_free(cfg);
    conf_free(conf);
    remove("/tmp/test_safety_ws.conf");
}
END_TEST

START_TEST(test_safety_load_from_conf_allow_network)
{
    write_conf_file("/tmp/test_safety_net.conf",
        "safety.allow_network = false\n");
    Conf *conf = conf_load("/tmp/test_safety_net.conf");
    SafetyConfig *cfg = safety_config_create();
    safety_load_from_conf(cfg, conf);
    ck_assert_int_eq(cfg->allow_network, 0);
    safety_config_free(cfg);
    conf_free(conf);
    remove("/tmp/test_safety_net.conf");
}
END_TEST

START_TEST(test_safety_load_from_conf_file_size)
{
    write_conf_file("/tmp/test_safety_fs.conf",
        "safety.max_file_size = 5000\n"
        "safety.max_execution_time = 120\n");
    Conf *conf = conf_load("/tmp/test_safety_fs.conf");
    SafetyConfig *cfg = safety_config_create();
    safety_load_from_conf(cfg, conf);
    ck_assert(cfg->max_file_size == 5000);
    ck_assert_int_eq(cfg->max_execution_time, 120);
    safety_config_free(cfg);
    conf_free(conf);
    remove("/tmp/test_safety_fs.conf");
}
END_TEST

START_TEST(test_safety_load_from_conf_allowed_commands)
{
    write_conf_file("/tmp/test_safety_cmds.conf",
        "safety.allowed_commands = ls, cat, echo\n");
    Conf *conf = conf_load("/tmp/test_safety_cmds.conf");
    SafetyConfig *cfg = safety_config_create();
    safety_load_from_conf(cfg, conf);
    ck_assert_int_eq(cfg->allowed_commands_count, 3);
    ck_assert_str_eq(cfg->allowed_commands[0], "ls");
    ck_assert_str_eq(cfg->allowed_commands[1], "cat");
    ck_assert_str_eq(cfg->allowed_commands[2], "echo");
    safety_config_free(cfg);
    conf_free(conf);
    remove("/tmp/test_safety_cmds.conf");
}
END_TEST

START_TEST(test_safety_load_from_conf_blocked_paths)
{
    write_conf_file("/tmp/test_safety_bp.conf",
        "safety.blocked_paths = /secret, vault/x\n");
    Conf *conf = conf_load("/tmp/test_safety_bp.conf");
    SafetyConfig *cfg = safety_config_create();
    safety_load_from_conf(cfg, conf);
    ck_assert_int_eq(cfg->blocked_paths_count, 2);
    ck_assert_str_eq(cfg->blocked_paths[0], "/secret");
    ck_assert_str_eq(cfg->blocked_paths[1], "vault/x");
    ck_assert_int_eq(safety_check_path(cfg, "vault/x/key.txt"), 0);
    ck_assert_int_eq(safety_check_path(cfg, "tmp/file.txt"), 1);
    safety_config_free(cfg);
    conf_free(conf);
    remove("/tmp/test_safety_bp.conf");
}
END_TEST

START_TEST(test_safety_load_from_conf_require_approval)
{
    write_conf_file("/tmp/test_safety_app.conf",
        "safety.require_approval_for = bash, write_file\n");
    Conf *conf = conf_load("/tmp/test_safety_app.conf");
    SafetyConfig *cfg = safety_config_create();
    safety_load_from_conf(cfg, conf);
    ck_assert_int_eq(cfg->require_approval_count, 2);
    ck_assert_str_eq(cfg->require_approval_for[0], "bash");
    ck_assert_str_eq(cfg->require_approval_for[1], "write_file");
    safety_config_free(cfg);
    conf_free(conf);
    remove("/tmp/test_safety_app.conf");
}
END_TEST

START_TEST(test_safety_load_from_conf_null_args)
{
    safety_load_from_conf(NULL, NULL);
    SafetyConfig *cfg = safety_config_create();
    safety_load_from_conf(cfg, NULL);
    ck_assert(cfg->max_file_size == 10485760);
    safety_load_from_conf(NULL, (Conf *)1);
    safety_config_free(cfg);
}
END_TEST

START_TEST(test_safety_load_from_conf_read_threshold)
{
    write_conf_file("/tmp/test_safety_read.conf",
        "safety.read_requires_approval = 1\n"
        "safety.read_size_threshold = 2048\n");
    Conf *conf = conf_load("/tmp/test_safety_read.conf");
    SafetyConfig *cfg = safety_config_create();
    safety_load_from_conf(cfg, conf);
    ck_assert_int_eq(cfg->read_requires_approval, 1);
    ck_assert(cfg->read_size_threshold == 2048);
    safety_config_free(cfg);
    conf_free(conf);
    remove("/tmp/test_safety_read.conf");
}
END_TEST

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
    suite_add_tcase(s, tc_path);

    TCase *tc_cmd = tcase_create("Command");
    tcase_set_timeout(tc_cmd, 10);
    tcase_add_test(tc_cmd, test_command_null_passes);
    tcase_add_test(tc_cmd, test_command_dangerous_rejected);
    tcase_add_test(tc_cmd, test_command_safe_passes);
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

    TCase *tc_load = tcase_create("LoadFromConf");
    tcase_set_timeout(tc_load, 10);
    tcase_add_test(tc_load, test_safety_load_from_conf_workspace);
    tcase_add_test(tc_load, test_safety_load_from_conf_allow_network);
    tcase_add_test(tc_load, test_safety_load_from_conf_file_size);
    tcase_add_test(tc_load, test_safety_load_from_conf_allowed_commands);
    tcase_add_test(tc_load, test_safety_load_from_conf_blocked_paths);
    tcase_add_test(tc_load, test_safety_load_from_conf_require_approval);
    tcase_add_test(tc_load, test_safety_load_from_conf_null_args);
    tcase_add_test(tc_load, test_safety_load_from_conf_read_threshold);
    suite_add_tcase(s, tc_load);

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
