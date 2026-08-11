# Fix Verification Evidence — 2026-08-11

Every entry records the regression test, the fail-on-old-code evidence,
and the pass-on-new-code evidence, per AGENTS.md's verification
discipline. Environment: `nix develop`, gcc 15.2.0, ASan+UBSan+LSan on,
Linux. Cross-references: docs/plans/AGENTS_COMPLIANCE_FIX_PLAN.md.

## A1 — server.c response-buffer leak (L1)
- Test: `test_server_response_frees_response_buffer_on_completion`,
  `test_server_response_cleans_up_on_uv_write_failure` (tests/server/test_server.c)
- Fail (old write_done): `Assertion 'server_test_free_tally() == 7'
  failed: server_test_free_tally() == 5` (the response buffer was never
  freed — one leak per HTTP response).
- Pass: 9/9 Checks green; Valgrind ERROR SUMMARY 0.
- Bonus found by the test: the SSE/HTTP uv_write synchronous-failure
  paths leaked req + frame too — fixed with cleanup + close.

## A2 — deep_search.c double-free + UAF (L2)
- Test: `test_deep_search_non_array_search_result_is_handled`
- Fail: `ERROR: AddressSanitizer: attempting double-free` in
  cJSON_Delete on a provider-returned object.
- Pass: 7/7 Checks green; Valgrind 0 errors.

## A3 — routes_ws.c negative-index heap underflow (L3)
- Test: `test_on_message_edit_rejects_negative_index`,
  `test_on_message_edit_rejects_huge_index`
- Fail: `ERROR: AddressSanitizer: SEGV ... in message_clear`
  (messages[-1] underflow from the edit frame).
- Pass: 91/91 Checks green.

## A4 — grep_tool.c stack OOB (L4)
- Test: `test_grep_large_output_across_files_stays_in_bounds`
- Fail: `ERROR: AddressSanitizer: stack-buffer-overflow ... in snprintf`
  (would-be-length accumulation across two files; 330-line fixture
  deliberately sits under the 32 KB cap, 360 lines overflows it).
- Pass: 2/2 Checks green; Valgrind 0 errors.

## A5 — session_branch.c uninitialized fork_copy (L5)
- Test: `test_fork_mint_allocation_failures_are_clean` (new asprintf
  fault hook `session_manager_test_set_asprintf_fail`), plus
  `test_fork_early_allocation_failures_are_clean` and the fixed
  `load_session_locked` unchecked-str_dup paths (positions 1-4 now all
  return -1 cleanly; position 4's title-dup failure used to silently
  produce a NULL title).
- Note: the free-of-garbage crash is stack-content dependent, so the
  tests assert the deterministic contract (rc == -1, *out zeroed, disk
  untouched) rather than the crash.
- Pass: 40/40 Checks green; Valgrind 0 errors.

## A6 — encryption.c unchecked EVP_DecryptFinal_ex (L6)
- Test: `test_encryption_rejects_hmac_valid_bad_padding_token`
- Fail: `Assertion 'dec == NULL' failed: dec == 0x...` (HMAC-valid token
  with corrupted ciphertext returned success + garbage plaintext).
- Pass: 40/40 Checks green (test lives in test_session_manager).

## A7 — rest_api.c missing <sys/socket.h> (L7)
- Compile-only fix; verified by the macOS CI job (implicit-function-
  declaration under -Werror). Linux build unaffected.

## B1 — registry.c delegate_config shutdown leak
- Test: `test_registry_destroy_releases_delegate_config`
- Fail: `Assertion 'registry_test_free_tally() == 4' failed:
  registry_test_free_tally() == 0` (4 strings leaked at destroy).
- Pass: 7/7 Checks green; Valgrind 0 errors.

## B2 — semantic_search.c index teardown + add_term rollback
- Tests: `test_sem_free_index_releases_and_reuses`,
  `test_sem_index_doc_alloc_fail_add_term_first` (now asserts rc == -1
  and no half-indexed commit).
- Fail: `Assertion 'rc == -1' failed: rc == 0` (document committed with
  a missing term).
- Pass: 7/7 Checks green.

## B3 — agent.c execute_tool_calls OOM paths
- Tests: `test_execute_tool_calls_appends_tool_result`,
  `test_execute_tool_calls_append_oom_keeps_count`,
  `test_execute_tool_calls_message_oom_is_handled` (revived the dead
  AGENT_TEST guard as a real seam: realloc fault hook + executor
  exposure).
- Bonus found by the tests: on the happy path the message struct itself
  (fields moved into the array) was leaked — fixed at all three append
  sites.
- Pass: 4/4 Checks green, LSan clean.

## B4 — server_stop NULL contract
- Test: `test_server_stop_null_ctx_is_noop`
- Fail: `runtime error: member access within null pointer` (UBSan).
- Pass: 9/9 Checks green.

## C2 — replace_in_file.c short-write (disk full)
- Test: `test_replace_in_file_write_failure_reports_error` (new fwrite
  fault hook under REPLACE_IN_FILE_TEST).
- Fix: temp-file + atomic rename, so a failed write never truncates the
  original; the old code reported success after truncating.
- Pass: 5/5 Checks green.

## C8 — handle_sessions DB error vs empty list
- Test: `test_handle_sessions_list_null` (contract updated: 500 +
  "session store error" instead of 200 {"sessions":[]}).
- Pass: 44/44 Checks green.

## E2 — routes_session dead alloc-fail hook
- Test: `test_handle_create_session_title_alloc_fail_returns_500`
  (positions 1-2: each aborts with 500 before any session is created).
- Pass: 44/44 Checks green.

## E3 — migration.c asprintf fault coverage
- Test: `test_migration_change_password_asprintf_failure_aborts_cleanly`
  (positions 1-3: each aborts with old state intact; the asprintf shim
  moved to session_manager.c so migration.c and session_branch.c share
  one counter, matching AGENTS.md's claim).
- Pass: 40/40 Checks green.

## E4 — memory.c coverage
- Tests: `test_memory_get_dup_alloc_fail_and_error_distinction`,
  `test_memory_set_delete_error_paths` (absent key vs store error vs
  alloc failure all distinguishable via is_error; NULL args; delete-
  missing-key semantics).
- Pass: 3/3 Checks green.

## E5 — tool_delegate loop-phase fault coverage
- Test: `test_delegate_loop_phase_alloc_failures_are_clean` (sweeps
  every loop-phase str_dup position 8-26 of a scripted tool round-trip).
- Pass: 9/9 Checks green, ASan clean.

## Valgrind spot-checks (rule 81 / plan definition-of-done :854)
All suites below: `CK_FORK=no valgrind --leak-check=full` → 0 errors:
- tests/server/test_server (A1/B4)
- tests/tools/test_deep_search (A2)
- tests/tools/test_grep_tool (A4)
- tests/session/test_session_manager (A5/A6/E3)
- tests/routes/test_routes_ws (A3/C6)
- tests/tools/test_registry (B1)
- tests/tools/test_semantic_search (B2/E8)
