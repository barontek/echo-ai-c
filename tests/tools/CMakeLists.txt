add_executable(test_semantic_search
    ../../src/tools/semantic_search.c
    ../../src/tools/tool.c
    ../../src/utils/string_utils.c
    test_semantic_search.c
)
target_compile_definitions(test_semantic_search PRIVATE SEMANTIC_SEARCH_TEST=1)
target_include_directories(test_semantic_search PRIVATE ../../src)
target_link_libraries(test_semantic_search PRIVATE PkgConfig::CHECK PkgConfig::CJSON m)
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
target_link_libraries(test_tool_delegate PRIVATE PkgConfig::CHECK PkgConfig::CJSON)
add_test(NAME test_tool_delegate COMMAND test_tool_delegate)

add_executable(test_registry
    ../../src/tools/registry.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_registry.c
)
target_compile_definitions(test_registry PRIVATE REGISTRY_TEST=1)
target_include_directories(test_registry PRIVATE ../../src)
target_link_libraries(test_registry PRIVATE PkgConfig::CHECK PkgConfig::CJSON)
add_test(NAME test_registry COMMAND test_registry)
