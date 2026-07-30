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

add_executable(test_notes
    ../../src/tools/notes.c
    ../../src/tools/tool.c
    ../../src/safety/safety.c
    ../../src/config/config.c
    ../../src/utils/string_utils.c
    test_notes.c
)
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
