/*
 * test_safety_conf.c - safety_load_from_conf tests for the safety
 * module. Split from test_safety.c (2026-08 file-length compliance).
 * Depends on: check, safety, config.
 */

#define _GNU_SOURCE
#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "safety/safety.h"
#include "config/config.h"

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
    ck_assert_int_eq(remove("/tmp/test_safety_ws.conf"), 0);
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
    ck_assert_int_eq(remove("/tmp/test_safety_net.conf"), 0);
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
    ck_assert_int_eq(remove("/tmp/test_safety_fs.conf"), 0);
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
    ck_assert_int_eq(remove("/tmp/test_safety_cmds.conf"), 0);
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
    ck_assert_int_eq(remove("/tmp/test_safety_bp.conf"), 0);
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
    ck_assert_int_eq(remove("/tmp/test_safety_app.conf"), 0);
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
    ck_assert_int_eq(remove("/tmp/test_safety_read.conf"), 0);
}
END_TEST

Suite *safety_conf_suite(void)
{
    Suite *s = suite_create("Safety Conf");

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
    Suite *s = safety_conf_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? 1 : 0;
}
