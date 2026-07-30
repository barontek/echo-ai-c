add_executable(test_memory
    ../../src/session/memory.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_memory.c
)
target_compile_definitions(test_memory PRIVATE MEMORY_TEST=1)
target_include_directories(test_memory PRIVATE ../../src)
target_link_libraries(test_memory PRIVATE echo_ai_test_deps)
add_test(NAME test_memory COMMAND test_memory)

add_executable(test_session_manager
    ../../src/session/session_manager.c
    ../../src/session/session.c
    ../../src/session/encryption.c
    ../../src/session/migration.c
    ../../src/session/memory.c
    ../../src/agent/message.c
    ../../src/tools/tool.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_session_manager.c
)
target_compile_definitions(test_session_manager PRIVATE SESSION_MANAGER_TEST=1)
target_include_directories(test_session_manager PRIVATE ../../src)
target_link_libraries(test_session_manager PRIVATE echo_ai_test_deps)
add_test(NAME test_session_manager COMMAND test_session_manager)

add_executable(test_change_tracker
    ../../src/change_tracker/change_tracker.c
    ../../src/utils/string_utils.c
    test_change_tracker.c
)
target_compile_definitions(test_change_tracker PRIVATE CHANGE_TRACKER_TEST=1)
target_include_directories(test_change_tracker PRIVATE ../../src)
target_link_libraries(test_change_tracker PRIVATE echo_ai_test_deps)
add_test(NAME test_change_tracker COMMAND test_change_tracker)
