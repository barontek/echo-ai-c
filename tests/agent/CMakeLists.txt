add_executable(test_message
    ../../src/agent/message.c
    ../../src/session/session.c
    ../../src/utils/string_utils.c
    ../../src/utils/json.c
    test_message.c
)
target_compile_definitions(test_message PRIVATE MESSAGE_TEST=1)
target_include_directories(test_message PRIVATE ../../src)
target_link_libraries(test_message PRIVATE echo_ai_test_deps)
add_test(NAME test_message COMMAND test_message)

add_executable(test_context
    ../../src/agent/context.c
    ../../src/agent/message.c
    ../../src/utils/string_utils.c
    ../../src/utils/json.c
    test_context.c
)
target_compile_definitions(test_context PRIVATE CONTEXT_TEST=1)
target_include_directories(test_context PRIVATE ../../src)
target_link_libraries(test_context PRIVATE echo_ai_test_deps)
add_test(NAME test_context COMMAND test_context)

add_executable(test_safety
    ../../src/safety/safety.c
    ../../src/config/config.c
    ../../src/utils/string_utils.c
    test_safety.c
)
target_include_directories(test_safety PRIVATE ../../src)
target_link_libraries(test_safety PRIVATE echo_ai_test_deps)
add_test(NAME test_safety COMMAND test_safety)

add_executable(test_agent_save
    ../../src/agent/agent.c
    ../../src/agent/message.c
    ../../src/session/session_manager.c
    ../../src/session/session.c
    ../../src/session/encryption.c
    ../../src/session/migration.c
    ../../src/session/memory.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    ../../src/tools/tool.c
    provider_auth_stub.c
    test_agent_save.c
)
target_compile_definitions(test_agent_save PRIVATE AGENT_TEST=1)
target_include_directories(test_agent_save PRIVATE ../../src)
target_link_libraries(test_agent_save PRIVATE echo_ai_test_deps)
add_test(NAME test_agent_save COMMAND test_agent_save)

add_executable(test_agent_provider
    ../../src/agent/agent.c
    ../../src/agent/message.c
    ../../src/session/session_manager.c
    ../../src/session/session.c
    ../../src/session/encryption.c
    ../../src/session/migration.c
    ../../src/session/memory.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    ../../src/tools/tool.c
    provider_auth_stub.c
    test_agent_provider.c
)
target_include_directories(test_agent_provider PRIVATE ../../src)
target_link_libraries(test_agent_provider PRIVATE echo_ai_test_deps)
add_test(NAME test_agent_provider COMMAND test_agent_provider)
