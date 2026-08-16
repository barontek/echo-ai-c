add_executable(test_browser
    ../../src/browser/cdp.c
    ../../src/browser/browser.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    fake_cdp.c
    test_browser.c
)
target_compile_definitions(test_browser PRIVATE BROWSER_TEST=1 CDP_TEST=1)
target_include_directories(test_browser PRIVATE ../../src)
target_link_libraries(test_browser PRIVATE echo_ai_test_deps)
add_test(NAME test_browser COMMAND test_browser)

add_executable(test_tool_browser
    ../../src/browser/cdp.c
    ../../src/browser/browser.c
    ../../src/tools/tool_browser.c
    ../../src/tools/tool.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    fake_cdp.c
    test_tool_browser.c
)
target_compile_definitions(test_tool_browser PRIVATE
    TOOL_BROWSER_TEST=1 BROWSER_TEST=1 CDP_TEST=1)
target_include_directories(test_tool_browser PRIVATE ../../src ../browser)
target_link_libraries(test_tool_browser PRIVATE echo_ai_test_deps)
add_test(NAME test_tool_browser COMMAND test_tool_browser)
