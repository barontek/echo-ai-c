# TUI test suites. Each suite compiles the module under test directly with
# its *(_TEST) guard (per-target definition only — production builds never
# see the fault-injection shims), mirroring the established metrics.c /
# routes_ws.c pattern.
add_compile_options(-Wall -Wextra -Wpedantic -Werror -g)
if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# --- tui_events (ring + self-pipe), fault-injection guards enabled ---
add_executable(test_tui_events
    test_tui_events.c
    ../../src/tui/tui_events.c
    ../../src/utils/string_utils.c
)
target_compile_definitions(test_tui_events PRIVATE TUI_EVENTS_TEST=1)
target_include_directories(test_tui_events PRIVATE ../../src)
target_link_libraries(test_tui_events PRIVATE echo_ai_test_deps)
add_test(NAME test_tui_events COMMAND test_tui_events)

# --- tui_theme (pure config/role logic) ---
add_executable(test_tui_theme
    test_tui_theme.c
    ../../src/tui/tui_theme.c
)
target_include_directories(test_tui_theme PRIVATE ../../src)
target_link_libraries(test_tui_theme PRIVATE echo_ai_test_deps)
add_test(NAME test_tui_theme COMMAND test_tui_theme)

# --- tui_input (line editor model) ---
add_executable(test_tui_input
    test_tui_input.c
    ../../src/tui/tui_input.c
    ../../src/utils/string_utils.c
)
target_compile_definitions(test_tui_input PRIVATE TUI_INPUT_TEST=1)
target_include_directories(test_tui_input PRIVATE ../../src)
target_link_libraries(test_tui_input PRIVATE echo_ai_test_deps)
add_test(NAME test_tui_input COMMAND test_tui_input)

# --- tui_chat (scrollback model), fault-injection guards enabled ---
add_executable(test_tui_chat
    test_tui_chat.c
    ../../src/tui/tui_chat.c
    ../../src/utils/string_utils.c
)
target_compile_definitions(test_tui_chat PRIVATE TUI_CHAT_TEST=1)
target_include_directories(test_tui_chat PRIVATE ../../src)
target_link_libraries(test_tui_chat PRIVATE echo_ai_test_deps)
add_test(NAME test_tui_chat COMMAND test_tui_chat)

# --- tui_dialogs (modal state machine, masking, round-trips) ---
add_executable(test_tui_dialogs
    test_tui_dialogs.c
    ../../src/tui/tui_dialogs.c
    ../../src/tui/tui_events.c
    ../../src/tui/tui_input.c
    ../../src/utils/string_utils.c
)
target_include_directories(test_tui_dialogs PRIVATE ../../src)
target_link_libraries(test_tui_dialogs PRIVATE echo_ai_test_deps)
add_test(NAME test_tui_dialogs COMMAND test_tui_dialogs)

# --- tui_stream (think/content chunk classifier) ---
add_executable(test_tui_stream
    test_tui_stream.c
    ../../src/tui/tui_stream.c
)
target_include_directories(test_tui_stream PRIVATE ../../src)
target_link_libraries(test_tui_stream PRIVATE echo_ai_test_deps)
add_test(NAME test_tui_stream COMMAND test_tui_stream)

# --- markdown (inline runs, line kinds, table geometry) ---
add_executable(test_markdown
    test_markdown.c
    ../../src/tui/markdown.c
    ../../src/tui/tui_chat.c
    ../../src/utils/string_utils.c
)
target_include_directories(test_markdown PRIVATE ../../src)
target_link_libraries(test_markdown PRIVATE echo_ai_test_deps)
add_test(NAME test_markdown COMMAND test_markdown)

# --- tool_args (compact tool-call argument summaries) ---
add_executable(test_tool_args
    test_tool_args.c
    ../../src/tui/tool_args.c
)
target_include_directories(test_tool_args PRIVATE ../../src)
target_link_libraries(test_tool_args PRIVATE echo_ai_test_deps)
add_test(NAME test_tool_args COMMAND test_tool_args)
