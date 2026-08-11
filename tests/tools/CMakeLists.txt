add_executable(test_semantic_search
    ../../src/tools/semantic_search.c
    ../../src/tools/tool.c
    ../../src/utils/string_utils.c
    test_semantic_search.c
)
target_compile_definitions(test_semantic_search PRIVATE SEMANTIC_SEARCH_TEST=1)
target_include_directories(test_semantic_search PRIVATE ../../src)
target_link_libraries(test_semantic_search PRIVATE echo_ai_test_deps)
add_test(NAME test_semantic_search COMMAND test_semantic_search)

add_executable(test_tool_delegate
    ../../src/tools/tool_delegate.c
    ../../src/tools/tool.c
    ../../src/agent/message.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_tool_delegate.c
)
target_compile_definitions(test_tool_delegate PRIVATE TOOL_DELEGATE_TEST=1)
target_include_directories(test_tool_delegate PRIVATE ../../src)
target_link_libraries(test_tool_delegate PRIVATE echo_ai_test_deps)
add_test(NAME test_tool_delegate COMMAND test_tool_delegate)

add_executable(test_registry
    ../../src/tools/registry.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_registry.c
)
target_compile_definitions(test_registry PRIVATE REGISTRY_TEST=1)
target_include_directories(test_registry PRIVATE ../../src)
target_link_libraries(test_registry PRIVATE echo_ai_test_deps)
add_test(NAME test_registry COMMAND test_registry)

add_executable(test_tool_memory
    ../../src/tools/tool_memory.c
    ../../src/tools/registry.c
    ../../src/tools/tool.c
    ../../src/session/memory.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_tool_memory.c
)
target_compile_definitions(test_tool_memory PRIVATE TOOL_MEMORY_TEST=1 REGISTRY_TEST=1)
target_include_directories(test_tool_memory PRIVATE ../../src)
target_link_libraries(test_tool_memory PRIVATE echo_ai_test_deps)
add_test(NAME test_tool_memory COMMAND test_tool_memory)

add_executable(test_read_file
    ../../src/tools/read_file.c
    ../../src/tools/tool.c
    ../../src/safety/safety.c
    ../../src/config/config.c
    ../../src/utils/string_utils.c
    test_read_file.c
)
target_include_directories(test_read_file PRIVATE ../../src)
target_link_libraries(test_read_file PRIVATE echo_ai_test_deps)
add_test(NAME test_read_file COMMAND test_read_file)

add_executable(test_write_file
    ../../src/tools/write_file.c
    ../../src/tools/tool.c
    ../../src/safety/safety.c
    ../../src/config/config.c
    ../../src/change_tracker/change_tracker.c
    ../../src/utils/logging.c
    ../../src/utils/string_utils.c
    test_write_file.c
)
target_include_directories(test_write_file PRIVATE ../../src)
target_link_libraries(test_write_file PRIVATE echo_ai_test_deps)
add_test(NAME test_write_file COMMAND test_write_file)

add_executable(test_list_dir
    ../../src/tools/list_dir.c
    ../../src/tools/tool.c
    ../../src/safety/safety.c
    ../../src/config/config.c
    ../../src/utils/string_utils.c
    test_list_dir.c
)
target_include_directories(test_list_dir PRIVATE ../../src)
target_link_libraries(test_list_dir PRIVATE echo_ai_test_deps)
add_test(NAME test_list_dir COMMAND test_list_dir)

add_executable(test_glob_tool
    ../../src/tools/glob_tool.c
    ../../src/tools/tool.c
    ../../src/safety/safety.c
    ../../src/config/config.c
    ../../src/utils/string_utils.c
    test_glob_tool.c
)
target_include_directories(test_glob_tool PRIVATE ../../src)
target_link_libraries(test_glob_tool PRIVATE echo_ai_test_deps)
add_test(NAME test_glob_tool COMMAND test_glob_tool)

add_executable(test_replace_in_file
    ../../src/tools/replace_in_file.c
    ../../src/tools/tool.c
    ../../src/safety/safety.c
    ../../src/config/config.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_replace_in_file.c
)
target_compile_definitions(test_replace_in_file PRIVATE REPLACE_IN_FILE_TEST=1)
target_include_directories(test_replace_in_file PRIVATE ../../src)
target_link_libraries(test_replace_in_file PRIVATE echo_ai_test_deps)
add_test(NAME test_replace_in_file COMMAND test_replace_in_file)

add_executable(test_tool_sqlite_query
    ../../src/tools/tool_sqlite_query.c
    ../../src/tools/registry.c
    ../../src/tools/tool.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_tool_sqlite_query.c
)
target_compile_definitions(test_tool_sqlite_query PRIVATE REGISTRY_TEST=1)
target_include_directories(test_tool_sqlite_query PRIVATE ../../src)
target_link_libraries(test_tool_sqlite_query PRIVATE echo_ai_test_deps)
add_test(NAME test_tool_sqlite_query COMMAND test_tool_sqlite_query)

add_executable(test_tool_sqlite_schema
    ../../src/tools/tool_sqlite_schema.c
    ../../src/tools/registry.c
    ../../src/tools/tool.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_tool_sqlite_schema.c
)
target_compile_definitions(test_tool_sqlite_schema PRIVATE REGISTRY_TEST=1)
target_include_directories(test_tool_sqlite_schema PRIVATE ../../src)
target_link_libraries(test_tool_sqlite_schema PRIVATE echo_ai_test_deps)
add_test(NAME test_tool_sqlite_schema COMMAND test_tool_sqlite_schema)

add_executable(test_tool_ask_user
    ../../src/tools/tool_ask_user.c
    ../../src/tools/registry.c
    ../../src/tools/tool.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_tool_ask_user.c
)
target_compile_definitions(test_tool_ask_user PRIVATE REGISTRY_TEST=1)
target_include_directories(test_tool_ask_user PRIVATE ../../src)
target_link_libraries(test_tool_ask_user PRIVATE echo_ai_test_deps)
add_test(NAME test_tool_ask_user COMMAND test_tool_ask_user)

add_executable(test_tool_humanizer
    ../../src/tools/tool_humanizer.c
    ../../src/tools/tool.c
    ../../src/utils/string_utils.c
    test_tool_humanizer.c
)
target_include_directories(test_tool_humanizer PRIVATE ../../src)
target_link_libraries(test_tool_humanizer PRIVATE echo_ai_test_deps)
add_test(NAME test_tool_humanizer COMMAND test_tool_humanizer)

add_executable(test_web_search
    ../../src/tools/web_search.c
    ../../src/tools/registry.c
    ../../src/tools/tool.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_web_search.c
)
target_compile_definitions(test_web_search PRIVATE REGISTRY_TEST=1)
target_include_directories(test_web_search PRIVATE ../../src)
target_link_libraries(test_web_search PRIVATE echo_ai_test_deps)
add_test(NAME test_web_search COMMAND test_web_search)

add_executable(test_search_provider
    ../../src/tools/search_provider.c
    ../../src/tools/search_brave.c
    ../../src/tools/search_duckduckgo.c
    ../../src/tools/search_tavily.c
    ../../src/utils/http_client.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_search_provider.c
)
target_include_directories(test_search_provider PRIVATE ../../src)
target_link_libraries(test_search_provider PRIVATE echo_ai_test_deps)
add_test(NAME test_search_provider COMMAND test_search_provider)

add_executable(test_python_execute
    ../../src/tools/python_execute.c
    ../../src/tools/tool.c
    ../../src/utils/string_utils.c
    test_python_execute.c
)
target_include_directories(test_python_execute PRIVATE ../../src)
target_link_libraries(test_python_execute PRIVATE echo_ai_test_deps)
add_test(NAME test_python_execute COMMAND test_python_execute)

add_executable(test_git
    ../../src/tools/git.c
    ../../src/tools/tool.c
    ../../src/utils/string_utils.c
    test_git.c
)
target_include_directories(test_git PRIVATE ../../src)
target_link_libraries(test_git PRIVATE echo_ai_test_deps)
add_test(NAME test_git COMMAND test_git)

add_executable(test_search_brave
    ../../src/tools/search_brave.c
    ../../src/utils/http_client.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_search_brave.c
)
target_compile_definitions(test_search_brave PRIVATE SEARCH_BRAVE_TEST=1)
target_include_directories(test_search_brave PRIVATE ../../src)
target_link_libraries(test_search_brave PRIVATE echo_ai_test_deps)
add_test(NAME test_search_brave COMMAND test_search_brave)

add_executable(test_search_tavily
    ../../src/tools/search_tavily.c
    ../../src/utils/http_client.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_search_tavily.c
)
target_compile_definitions(test_search_tavily PRIVATE SEARCH_TAVILY_TEST=1)
target_include_directories(test_search_tavily PRIVATE ../../src)
target_link_libraries(test_search_tavily PRIVATE echo_ai_test_deps)
add_test(NAME test_search_tavily COMMAND test_search_tavily)

add_executable(test_ingest_document
    ../../src/tools/ingest_document.c
    ../../src/tools/tool.c
    ../../src/tools/semantic_search.c
    ../../src/safety/safety.c
    ../../src/config/config.c
    ../../src/utils/string_utils.c
    test_ingest_document.c
)
target_compile_definitions(test_ingest_document PRIVATE SEMANTIC_SEARCH_TEST=1)
target_include_directories(test_ingest_document PRIVATE ../../src)
target_link_libraries(test_ingest_document PRIVATE echo_ai_test_deps)
add_test(NAME test_ingest_document COMMAND test_ingest_document)

add_executable(test_rest_api
    ../../src/tools/rest_api.c
    ../../src/tools/tool.c
    ../../src/utils/http_client.c
    ../../src/safety/safety.c
    ../../src/config/config.c
    ../../src/utils/string_utils.c
    test_rest_api.c
)
target_include_directories(test_rest_api PRIVATE ../../src)
target_link_libraries(test_rest_api PRIVATE echo_ai_test_deps)
add_test(NAME test_rest_api COMMAND test_rest_api)

add_executable(test_deep_search
    ../../src/tools/deep_search.c
    ../../src/tools/registry.c
    ../../src/tools/tool.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_deep_search.c
)
target_compile_definitions(test_deep_search PRIVATE DEEP_SEARCH_TEST=1 REGISTRY_TEST=1)
target_include_directories(test_deep_search PRIVATE ../../src)
target_link_libraries(test_deep_search PRIVATE echo_ai_test_deps)
add_test(NAME test_deep_search COMMAND test_deep_search)

add_executable(test_notes
    ../../src/tools/notes.c
    ../../src/tools/tool.c
    ../../src/safety/safety.c
    ../../src/config/config.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_notes.c
)
target_compile_definitions(test_notes PRIVATE NOTES_TEST=1)
target_include_directories(test_notes PRIVATE ../../src)
target_link_libraries(test_notes PRIVATE echo_ai_test_deps)
add_test(NAME test_notes COMMAND test_notes)

add_executable(test_bash
    ../../src/tools/bash.c
    ../../src/tools/tool.c
    ../../src/safety/safety.c
    ../../src/config/config.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_bash.c
)
target_include_directories(test_bash PRIVATE ../../src)
target_link_libraries(test_bash PRIVATE echo_ai_test_deps)
add_test(NAME test_bash COMMAND test_bash)

add_executable(test_grep_tool
    ../../src/tools/grep_tool.c
    ../../src/tools/tool.c
    ../../src/safety/safety.c
    ../../src/config/config.c
    ../../src/utils/string_utils.c
    test_grep_tool.c
)
target_include_directories(test_grep_tool PRIVATE ../../src)
target_link_libraries(test_grep_tool PRIVATE echo_ai_test_deps)
add_test(NAME test_grep_tool COMMAND test_grep_tool)

add_executable(test_web_fetch
    ../../src/tools/web_fetch.c
    ../../src/tools/tool.c
    ../../src/safety/safety.c
    ../../src/config/config.c
    ../../src/utils/http_client.c
    ../../src/utils/string_utils.c
    ../../src/utils/html_extract.c
    test_web_fetch.c
)
target_compile_definitions(test_web_fetch PRIVATE WEB_FETCH_TEST=1)
target_include_directories(test_web_fetch PRIVATE ../../src)
target_link_libraries(test_web_fetch PRIVATE echo_ai_test_deps)
add_test(NAME test_web_fetch COMMAND test_web_fetch)
