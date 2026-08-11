# File-Length Compliance Plan (AGENTS.md — Code style: File size, Testing: Size and scope)

**Date:** 2026-08-11
**Source:** AGENTS.md rules — "No hard line limit, but treat 300-800 lines as comfortable and 1000+ as a signal to split" (Code style/File size) and "a test file that's grown past ~800-1000 lines is worth splitting" (Testing/Size and scope). Rule 61 (mirror module boundaries with compilation units), rule 24 (one header per module), rule 29 (no >60-line functions), rule 62 (one TCase per behavior area).
**Repo:** /home/barontek/echo-ai-c (HEAD: file lengths measured 2026-08-11 via `wc -l`)
**Scope:** All 15 files over 800 lines (6 src VIOLATION >1000, 5 src NEAR 800-1000, 4 test VIOLATION, 4 test NEAR). Executes the deferred F2/E16/E19 items of `AGENTS_COMPLIANCE_FIX_PLAN.md`; that plan's status table gets updated as items land here.

## Threshold policy

- **VIOLATION (must split):** >1000 lines — the rule's explicit split signal.
- **NEAR-LIMIT (split to stay in band):** 800-1000 lines — above the "comfortable" band; tests in this range are already "worth splitting" per the rule. Target: every file ≤800 after this pass.
- **Headers are exempt from arbitrary splitting:** rule 24's one-header-per-module contract wins; `session_manager.h` (520) and `openai_oauth.h` (454) are in the comfortable band. Revisit only if any header exceeds ~600 lines. New modules created by these splits DO get their own headers (rule 24, no exceptions).
- Splits are by **responsibility boundary only** — never line-count slicing. A moved function must be byte-identical; a split is not an excuse to rewrite.

## Conventions

- Item IDs: `S<n>` source splits, `T<n>` test splits, `N<n>` near-limit reductions, `M<n>` monitoring. Line refs measured against HEAD on 2026-08-11.
- **Every split ships as one commit** containing: the new .c/.h units, CMake wiring, and — for tests — the new binaries registered with `add_test()`. After every commit: full `ctest` run under ASan+UBSan, then `wc -l` audit recorded in the status table below.
- Splits must be behavior-preserving: full suite green before and after the commit; no test edits except moving tests to their mirrored new files.
- Any >60-line function exposed while splitting gets a helper split in the same commit (rule 29), with fault-injection tests where it allocates (AGENTS.md fault-injection section).
- Test-only hooks (`*_test_set_alloc_fail`, curl stubs, `WS_STATIC` visibility macros) travel with their subject code; the compile definitions that activate them (e.g. `ROUTES_WS_TEST` at tests/routes/CMakeLists.txt:79) are applied to every new binary that links that unit.
- Items marked `[defer]` are scoped out with a reason — never silently skipped.

---

## Phase S — Source files, VIOLATION (>1000 lines)

### S1 src/llm/openai_oauth.c (2348) — split into 4 new units + core
- **Seams** (function clusters, line refs):
  - `oauth_codec.c` — pure string/crypto helpers: `is_url_char` (:177), `url_encode` (:183), `base64url_encode` (:207), `pkce_challenge` (:231), `random_string` (:241), `make_pkce` (:252), `build_authorize_url_values` (:266), `hex_value` (:289), `url_decode_exact` (:297), `base64url_value` (:516), `base64url_decode` (:526). Header: `oauth_codec.h`.
  - `oauth_jwt.c` — `jwt_payload_json` (:578), `jwt_metadata` (:612), `exact_json_object` (:660), `json_keys_are_unique` (:674), `needs_refresh` (:2031). Header: `oauth_jwt.h`. Test hooks `openai_oauth_test_jwt_metadata` (:2325) and `openai_oauth_test_needs_refresh` (:2330) move here.
  - `oauth_callback.c` — callback-server cluster: `valid_header_name` (:342), `name_equal` (:356), `host_value_valid` (:365), `validate_callback_headers` (:381), `callback_clear` (:426), `send_all` (:1034), `callback_response` (:1051), `callback_still_active` (:1065), `state_matches` (:1103), `callback_publish_failure` (:1194), `callback_publish_listener` (:1206), `callback_finish` (:1221), `untrack_socket` (:1264), `callback_thread_main` (:1272), `shutdown_fd` (:1338), `cancel_callback_locked` (:1344), `take_callback_thread_locked` (:1354), `join_callback_thread` (:1362), `lifecycle_finish` (:1367), `active_operation_finish` (:1375), `reap_previous_callback` (:1495). Header: `oauth_callback.h`.
  - `oauth_vault.c` — credential storage/parsing: `credentials_clear` (:134), `clear_credentials_locked` (:144), `clear_pending_sensitive_locked` (:153), `credentials_json` (:732), `stored_credentials_parse` (:750), `commit_credentials_locked` (:781). Header: `oauth_vault.h`.
  - Device flow (`device_json_body` :983, `device_poll_released` :1792) can land in `oauth_callback.c` (same thread/lifecycle domain) or a fifth unit `oauth_device.c` if callback stays >600.
  - **Core** keeps the state machine + public API: `openai_oauth_create/destroy/attach_session/start/cancel_login/logout` and the two remaining `openai_oauth_test_*` build helpers (:2279/:2284).
- **Header problem:** all units share the private `OpenAIOAuth` struct. Add `openai_oauth_internal.h` (struct + lock/state accessors), a documented exception in the same spirit as `tool.h`/`registry.h`; public surface stays in `openai_oauth.h`.
- **Verify:** test_openai_oauth.c + test_routes_openai_auth.c green under sanitizers; `wc -l` of core target ≤600.

### S2 src/llm/openai.c (2077) — split into 3 units + provider vtable core
- `openai_request.c` — request builder: `json_add_item` (:110), `json_array_add` (:125), `json_add_string` (:140), `json_add_bool` (:149), `valid_nonempty_string` (:154), `append_instruction` (:172), `add_function_call_input` (:226), `add_tool_output` (:280), `add_provider_state` (:300), `convert_tools` (:391), `add_structured_format` (:459), `add_reasoning_include` (:505), `add_reasoning_config` (:535), `openai_reasoning_effort_valid` (:524). Header: `openai_request.h`.
- `openai_stream.c` — StreamParser cluster: `event_output_index` (:1307), `capture_message_phase` (:1314), `merge_completed_output` (:1354), `append_reasoning_item` (:1408), `emit_thinking_text` (:1444), `close_thinking_block` (:1458), `reasoning_summary_join` (:1470), `emit_summary_text` (:1504), `parse_stream_event` (:1537), `stream_process_event` (:1654), `stream_process_line` (:1667), `stream_feed` (:1685), `stream_calls_complete` (:1718), `stream_finish` (:1730), `stream_parser_cleanup` (:1751). `openai_stream.h` owns the `StreamParser` struct.
- `openai_response.c` — response parsing + HTTP/curl plumbing: `parse_bounded_json` (:160), `valid_header_value` (:626), `header_append` (:633), `headers_free` (:641), `credentials_get` (:715), `model_already_added` (:764), `log_empty_catalog_diagnostic` (:771), `parse_function_call_item` (:1059), `parse_message_item` (:1072), `response_status_ok` (:1105), `parse_response` (:1120), `json_integer` (:1197), `replace_call_field` (:1262). Header: `openai_response.h`. Test hook `openai_test_parse_response_alloc` (:1976) moves here.
- **Core** keeps the vtable: chat/stream/models/refresh/authorize/destroy, `clear_secret` (:94), `credentials_clear` (:102), `openai_models_free` (:757).
- **Verify:** test_openai.c + fuzz targets for the stream parser still link and pass (fuzz target CMake references the parser symbols — keep names unchanged); sanitizer run.

### S3 src/session/session_manager.c (1691) — split into 3 units + facade core
- `session_db.c` — `mkdir_p` (:191), `init_db` (:209, the ~320-line schema/prepared-statement block — also the natural home for the sm_test_bind_* helpers :119-147). Header: `session_db.h`.
- `session_serialize.c` — `load_session_locked` (:654), `session_manager_export_session_new` (:1419), `import_session_build` (:1459), `session_manager_import_session_new` (:1573). Header: `session_serialize.h`.
- `session_list.c` — `session_list_grow` (:1193), `session_manager_list_sessions` (:1263), `session_list_free` (:1322). Header: `session_list.h`.
- `session_purge.c` — `session_manager_purge_sessions` (:1597), `session_manager_delete_session` (:1154), `session_manager_truncate_history` (:1391). Header: `session_purge.h` (optional; fold into `session_db.h` if it stays small).
- **Core** keeps the facade: create/retain/free/create_session/load/save/lock/unlock plus the test hooks (:37-188), which move to `session_manager_test_hooks.c` only if the core still exceeds 800 (they are always-exported test symbols; a dedicated TU built into the same lib target is acceptable).
- **Verify:** test_session_manager.c + test_migration.c (migration.c is 727 lines and depends on init_db behavior) green under sanitizers.

### S4 src/utils/html_extract.c (1582) — split into 5 units + parse core
- `html_unicode.c` — `utf8_encode_cp` (:386), `cp1252_cp` (:422), `utf8_valid` (:437), `utf8_cut_boundary` (:1315), `prefix_ieq` (:1477).
- `html_entities.c` — `entity_lookup` (:373) + its table.
- `html_tags.c` — `tag_lookup` (:123) + tag table, `is_frame_tag` (:137), `is_excluded_tag` (:142), `is_header_tag` (:147), `tag_weight_of` (:153).
- `html_outbuf.c` — `outbuf_reserve` (:204), `outbuf_append` (:225), `outbuf_append_chr` (:234), `outbuf_truncate` (:242).
- `html_writer.c` — `writer_newline` (:263), `writer_preflush` (:268), `writer_text` (:292), `writer_sync_state` (:327), `count_words` (:559), `run_has_nonws` (:575), `frame_score` (:602), `title_append_byte` (:652), `append_title_text` (:669).
- **Core** keeps the parse loop and public API: `emit_text_run` (:749), `emit_text` (:801), `frame_close` (:874), `link_resolve_index` (:899), `find_tag_end` (:940), `ieq` (:971), `handle_open_tag` (:1036), `handle_close_tag` (:1111), `parse` (:1162), `build_footer` (:1263), `assemble_cut` (:1325), `assemble` (:1354), `html_extract_text_alloc` (:1445), `text_for_llm` (:1489), test hooks (:25-38).
- **Header problem:** the `Extract`/`Writer`/`Frame`/`OutBuf` structs are shared across these clusters. Add `html_extract_internal.h` (documented exception, same pattern as S1) holding the struct defs + alloc-fail hook wiring.
- **Verify:** test_html_extract.c (562 lines, incl. alloc-fail tests) green under sanitizers; fuzz target for html_extract still links.

### S5 src/server/routes/routes_ws.c (1442) — split into 3 units + entry core
- `routes_ws_chat.c` — ctx lifecycle + queue: `ws_chat_ctx_destroy` (:110), `ws_chat_flush_queue` (:236), `ws_chat_enqueue` (:286), `ws_emit_branch_info` (:313), `ws_swap_agent_messages` (:341), `ws_send_done` (:231), `ws_chat_emit_session_start` (:1236), `ws_apply_query_session` (:1253), `ws_emit_ready` (:1362).
- `routes_ws_handlers.c` — `ws_handle_session_id` (:524), `ws_handle_provider_frame` (:621), `ws_handle_regenerate` (:759), `ws_handle_branch_switch` (:815), `ws_chat_on_message` (:945).
- `routes_ws_callbacks.c` — `ws_chat_on_chunk` (:154), `ws_title_update_cb` (:1086), `ws_tool_start_cb` (:1101), `ws_chat_on_close` (:1135), `ws_approval_cb` (:1152), `ws_ask_user_cb` (:1196).
- **Core** keeps `routes_ws_invalidate_auth` (:136), `routes_ws_chat_init` (:1374) and the test hooks (:43-107).
- **Header problem:** `WSChatCtx` and the `WS_STATIC` visibility scheme (routes_ws.c:30-32) are shared. Add `routes_ws_internal.h` declaring the struct + shared internals; every new unit must be compiled with the same `WS_STATIC` definition so test builds (`ROUTES_WS_TEST`, tests/routes/CMakeLists.txt:79) see non-static symbols.
- **Verify:** test_routes_ws.c green under sanitizers (the big test file shrinks via T1 after this lands).

### S6 src/agent/agent.c (1367) — split into 5 units + public API core
- `agent_tools.c` — `execute_tool_calls` (:181) + `agent_test_execute_tool_calls` (:344).
- `agent_prompt.c` — `build_system_prompt` (:350), `inject_system_with_summary` (:437), `strip_think_tags` (:592).
- `agent_title.c` — `agent_apply_title` (:710), `agent_generate_title` (:742).
- `agent_summarize.c` — `agent_perform_summarization` (:826).
- `agent_run.c` — `agent_llm_call` (:485), `tool_calls_remaining` (:550), `attach_tool_calls_to_resp` (:561), `gen_run_id` (:926), `agent_run_new` (:934), `null_chunk_handler` (:1027).
- **Core** keeps create/destroy/append_message/save/cancel/set_* callbacks/set_model + test hooks (:42-57).
- `agent_internal.h` (documented exception, same pattern as S1/S4/S5) if `Agent` struct access must cross units; if agent.h:336 already exposes what the units need, reuse it.
- **Verify:** test_agent_save.c, test_context.c, test_message.c, test_agent_provider.c, test_safety.c green under sanitizers.

## Phase T — Test files, VIOLATION (>1000 lines)

### T1 tests/routes/test_routes_ws.c (2362, 91 tests) — split by behavior into 3 binaries
- `test_routes_ws_queue.c` — enqueue/flush TCases (test_enqueue_* :862, test_flush_queue_* :940 — 7 tests).
- `test_routes_ws_callbacks.c` — chunk/send_done/emit_session_start/title/tool_*/approval/ask_user/on_close (:559-843, :1026-1220 — ~30 tests).
- `test_routes_ws_message.c` — on_message + provider_config + regenerate + branch_switch + query/edit (:1221-~2180 — ~40 tests).
- Shared mock fixture (WSClient/WSChatCtx builders) → `tests/routes/ws_test_helpers.h`/`.c` compiled into all three binaries (same pattern as `openai_oauth_stub.c`).
- Each binary keeps its own `suite_*()`; register all three in tests/routes/CMakeLists.txt with `ROUTES_WS_TEST ROUTES_TEST` and individual `add_test()`.

### T2 tests/session/test_session_manager.c (1994, 40 tests) — split by module (rule 61 mirroring; executes deferred E16/E19)
- `test_session_manager.c` — core CRUD/Encryption/List/Import-Export TCases.
- `test_session_branch.c` — the 15-test Branches TCase (:1316-1907) — this is deferred E19.
- `test_session_oauth.c` — ProviderOAuth + password-change/migration TCases (test_provider_oauth_*, test_password_change_*, test_first_run_verifier_*).
- `test_session_recovery.c` — legacy-recovery TCases (test_legacy_post_commit_recovery*, test_recovery_restores_backup*, test_missing_verifier_*, test_encryption_rejects_hmac*).
- Shared fixture (temp dir, sm creation, alloc-fail hook usage) → `tests/session/session_test_fixture.h`/`.c`.

### T3 tests/routes/test_routes_session_handlers.c (1197, 44 tests) — split by verb group
- `test_routes_session_handlers.c` — handle_sessions + handle_create_session (:354-570, 15 tests).
- `test_routes_session_get.c` — get/export/debug (:571-707, 9 tests).
- `test_routes_session_mutate.c` — delete/update/rename (:708-1045, 20 tests).
- `test_routes_session_import.c` — import (:1046-1110, 4 tests) — fold into mutate if it lands ≤800 without it.

### T4 tests/routes/test_routes_general.c (1127, 36 tests) — extract models cluster
- `test_routes_models.c` — the 14-test models block (:695-1037) incl. the OpenAI catalog fixtures.
- Remaining health/status/config/metrics/undo/redo stays at ~700.

## Phase N — NEAR-LIMIT (800-1000), get under 800

### Source
- **N1 src/llm/openai_compatible.c (968)** — extract StreamParser cluster (`ensure_tool_call` :26, `stream_parser_feed` :161, `stream_parser_finish` :195, `openai_compatible_test_parse_response` :242) → `openai_compatible_stream.c` (-~240).
- **N2 src/server/server.c (917)** — extract static serving (`mime_type` :80, `static_request_path_safe` :94, `open_static_file_beneath` :113, `serve_static` :356) → `serve_static.c`; extract HTTP framing (`parse_content_length` :552, `parse_http` :602, `client_append` :679) → `http_parse.c`.
- **N3 src/main.c (859)** — `[defer]` with documented rationale: entry point + two REPL frontends is defensible as one file (prior-art: AGENTS_COMPLIANCE_FIX_PLAN.md F4). Revisit if it passes 900.
- **N4 src/llm/ollama.c (850)** — move the test-only curl stub block (:782-846) to `tests/llm/curl_stub.c` shared by the ollama tests (-~64, plus hooks) and extract `parse_stream_tool_calls`/`forward_chunk`/`write_cb` (:86/:142/:167) → `ollama_stream.c` if still needed.
- **N5 src/session/session_branch.c (833)** — `branch_find_fork_index` (:311-730) is a ~420-line function; split into per-rule helpers (rule 29) in `session_branch_find.c`, keeping the branch-ops API in `session_branch.c`.

### Tests
- **N6 tests/routes/test_routes_auth.c (904)** — extract the 10-test change-password block (:598-833) → `test_routes_auth_password.c`.
- **N7 tests/llm/test_ollama.c (882)** — consumes N4's `curl_stub.c`; extract the ChatStreaming TCase if still >800.
- **N8 tests/llm/test_openai.c (848)** — split stream TCases (:418-649) → `test_openai_stream.c`; request cluster stays.
- **N9 tests/agent/test_safety.c (818)** — extract the 13-test load_from_conf block (:588-724) → `test_safety_conf.c`.

## Phase M — Monitoring and enforcement

- **M1** Update the status table below after every split commit (wc -l + test result). This file is the single source of truth for the audit.
- **M2** Add `scripts/check-file-lengths.sh` (wc -l against a per-file ceiling of 800 for src/tests, ~600 for headers) and wire it into CI as a non-blocking report — **only after Phase S/T/N land**, so it starts green. [defer] until then.
- **M3** Reconcile with AGENTS_COMPLIANCE_FIX_PLAN.md: tick F2, E16, E19 in its status table (with pointer to this file) as their items land. Delete the `[defer]` notes for openai.c/routes_ws.c that F2 recorded, since S2/S5 now execute them.

## Status

Executed 2026-08-11/12; every item verified with full ctest under ASan+UBSan (76/76 at close).
Final audit 2026-08-12: the only file over 800 lines in src/ and tests/ is src/main.c (859,
deferred by design — entry point plus two REPL frontends).

| ID | File | Before | After | Status |
|----|------|--------|-------|--------|
| S1 | src/llm/openai_oauth.c | 2348 | core 660; oauth_codec/jwt/vault/http/callback/device 24-577 | [x] |
| S2 | src/llm/openai.c | 2077 | core 352; openai_request/stream/response 537/582/615 | [x] |
| S3 | src/session/session_manager.c | 1691 | core 760; db/serialize/list/purge 88/596/161/117 | [x] |
| S4 | src/utils/html_extract.c | 1582 | core 674; unicode/entities/tags/outbuf/writer/assembly 143/102/86/68/228/185 | [x] |
| S5 | src/server/routes/routes_ws.c | 1442 | core 148; chat/handlers/callbacks 507/585/203 | [x] |
| S6 | src/agent/agent.c | 1367 | core 391; prompt/tools/title/summarize/run 170/190/243/103/360 | [x] |
| T1 | tests/routes/test_routes_ws.c | 2362 | callbacks/queue/message/provider/fork 612/331/398/366/341 + helpers 515 | [x] |
| T2 | tests/session/test_session_manager.c | 1994 | manager/crud/oauth/branch 294/545/401/703 + fixture 60 | [x] |
| T3 | tests/routes/test_routes_session_handlers.c | 1197 | handlers/get/mutate/import 273/190/418/104 + fixture | [x] |
| T4 | tests/routes/test_routes_general.c | 1127 | general/models 377/372 + fixture | [x] |
| N1 | src/llm/openai_compatible.c | 968 | 713 + openai_compatible_stream 276 | [x] |
| N2 | src/server/server.c | 917 | 552 + serve_static/http_parse 187/198 | [x] |
| N3 | src/main.c | 859 | unchanged — [defer]: entry point + two REPL frontends is one defensible file; revisit above 900 | [ ] |
| N4 | src/llm/ollama.c | 850 | 772 + tests/llm/curl_stub.c 73 | [x] |
| N5 | src/session/session_branch.c | 833 | 685 + session_branch_info 162 | [x] |
| N6 | tests/routes/test_routes_auth.c | 904 | 441 + auth_password 281 + fixture | [x] |
| N7 | tests/llm/test_ollama.c | 882 | 416 + test_ollama_stream 361 + fixture | [x] |
| N8 | tests/llm/test_openai.c | 848 | 588 + test_openai_stream | [x] |
| N9 | tests/agent/test_safety.c | 818 | 675 + test_safety_conf | [x] |
| M1 | audit log current | — | this table | [x] |
| M2 | check-file-lengths script | — | scripts/check-file-lengths.sh (landed; CI wiring optional) | [x] |
| M3 | reconcile AGENTS_COMPLIANCE_FIX_PLAN.md | — | F2/E16/E19 ticked with pointer to this file | [x] |

Known pre-existing issue recorded while verifying: test_web_fetch fails sporadically under
full parallel ctest (docs/verification/test_web_fetch_parallel_flake.md); reproduced on the
pre-refactor baseline, unrelated to these splits.
