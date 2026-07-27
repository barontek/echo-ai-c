add_executable(test_config ../src/config/config.c ../src/utils/string_utils.c test_config.c)
target_compile_definitions(test_config PRIVATE CONFIG_TEST=1)
target_include_directories(test_config PRIVATE ${CHECK_INCLUDE_DIRS} ../src)
target_link_libraries(test_config PRIVATE ${CHECK_LIBRARIES})
add_test(NAME test_config COMMAND test_config)

add_executable(test_string ../src/utils/string_utils.c test_string.c)
target_include_directories(test_string PRIVATE ${CHECK_INCLUDE_DIRS} ../src)
target_link_libraries(test_string PRIVATE ${CHECK_LIBRARIES})
add_test(NAME test_string COMMAND test_string)

add_executable(test_message
    ../src/agent/message.c
    ../src/session/session.c
    ../src/utils/string_utils.c
    ../src/utils/json.c
    test_message.c
)
target_include_directories(test_message PRIVATE ${CHECK_INCLUDE_DIRS} ${CJSON_INCLUDE_DIRS} ../src)
target_link_libraries(test_message PRIVATE ${CHECK_LIBRARIES} ${CJSON_LIBRARIES})
add_test(NAME test_message COMMAND test_message)

add_executable(test_circuit_breaker
    ../src/utils/circuit_breaker.c
    test_circuit_breaker.c
)
target_include_directories(test_circuit_breaker PRIVATE ${CHECK_INCLUDE_DIRS} ../src)
target_link_libraries(test_circuit_breaker PRIVATE ${CHECK_LIBRARIES})
add_test(NAME test_circuit_breaker COMMAND test_circuit_breaker)

add_executable(test_metrics
    ../src/utils/metrics.c
    ../src/utils/string_utils.c
    test_metrics.c
)
target_compile_definitions(test_metrics PRIVATE METRICS_TEST=1)
target_include_directories(test_metrics PRIVATE ${CHECK_INCLUDE_DIRS} ../src)
target_link_libraries(test_metrics PRIVATE ${CHECK_LIBRARIES})
add_test(NAME test_metrics COMMAND test_metrics)

add_executable(test_memory
    ../src/session/memory.c
    ../src/utils/string_utils.c
    ../src/utils/logging.c
    test_memory.c
)
target_compile_definitions(test_memory PRIVATE MEMORY_TEST=1)
target_include_directories(test_memory PRIVATE ${CHECK_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS} ../src)
target_link_libraries(test_memory PRIVATE ${CHECK_LIBRARIES} ${SQLITE3_LIBRARIES})
add_test(NAME test_memory COMMAND test_memory)

add_executable(test_session_manager
    ../src/session/session_manager.c
    ../src/session/session.c
    ../src/session/encryption.c
    ../src/session/migration.c
    ../src/session/memory.c
    ../src/agent/message.c
    ../src/tools/tool.c
    ../src/utils/string_utils.c
    ../src/utils/logging.c
    test_session_manager.c
)
target_compile_definitions(test_session_manager PRIVATE SESSION_MANAGER_TEST=1)
target_include_directories(test_session_manager PRIVATE ${CHECK_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIRS} ${CJSON_INCLUDE_DIRS} ../src)
target_link_libraries(test_session_manager PRIVATE ${CHECK_LIBRARIES} ${SQLITE3_LIBRARIES} ${OPENSSL_LIBRARIES} ${CJSON_LIBRARIES})
add_test(NAME test_session_manager COMMAND test_session_manager)

add_executable(test_change_tracker
    ../src/change_tracker/change_tracker.c
    ../src/utils/string_utils.c
    test_change_tracker.c
)
target_compile_definitions(test_change_tracker PRIVATE CHANGE_TRACKER_TEST=1)
target_include_directories(test_change_tracker PRIVATE ${CHECK_INCLUDE_DIRS} ../src)
target_link_libraries(test_change_tracker PRIVATE ${CHECK_LIBRARIES})
add_test(NAME test_change_tracker COMMAND test_change_tracker)

add_executable(test_semantic_search
    ../src/tools/semantic_search.c
    ../src/tools/tool.c
    ../src/utils/string_utils.c
    test_semantic_search.c
)
target_compile_definitions(test_semantic_search PRIVATE SEMANTIC_SEARCH_TEST=1)
target_include_directories(test_semantic_search PRIVATE ${CHECK_INCLUDE_DIRS} ${CJSON_INCLUDE_DIRS} ../src)
target_link_libraries(test_semantic_search PRIVATE ${CHECK_LIBRARIES} ${CJSON_LIBRARIES} m)
add_test(NAME test_semantic_search COMMAND test_semantic_search)

add_executable(test_tool_delegate
    ../src/tools/tool_delegate.c
    ../src/tools/tool.c
    ../src/agent/message.c
    ../src/utils/string_utils.c
    ../src/utils/logging.c
    test_tool_delegate.c
)
target_compile_definitions(test_tool_delegate PRIVATE TOOL_DELEGATE_TEST=1)
target_include_directories(test_tool_delegate PRIVATE ${CHECK_INCLUDE_DIRS} ${CJSON_INCLUDE_DIRS} ../src)
target_link_libraries(test_tool_delegate PRIVATE ${CHECK_LIBRARIES} ${CJSON_LIBRARIES})
add_test(NAME test_tool_delegate COMMAND test_tool_delegate)

add_executable(test_registry
    ../src/tools/registry.c
    ../src/utils/string_utils.c
    ../src/utils/logging.c
    test_registry.c
)
target_compile_definitions(test_registry PRIVATE REGISTRY_TEST=1)
target_include_directories(test_registry PRIVATE ${CHECK_INCLUDE_DIRS} ${CJSON_INCLUDE_DIRS} ../src)
target_link_libraries(test_registry PRIVATE ${CHECK_LIBRARIES} ${CJSON_LIBRARIES})
add_test(NAME test_registry COMMAND test_registry)

add_executable(test_ollama_tool_calls test_ollama_tool_calls.c)
target_include_directories(test_ollama_tool_calls PRIVATE ${CHECK_INCLUDE_DIRS} ${CJSON_INCLUDE_DIRS} ../src)
target_link_libraries(test_ollama_tool_calls PRIVATE ${CHECK_LIBRARIES} ${CJSON_LIBRARIES})
add_test(NAME test_ollama_tool_calls COMMAND test_ollama_tool_calls)

add_executable(test_agent_save
    ../src/agent/agent.c
    ../src/agent/message.c
    ../src/session/session_manager.c
    ../src/session/session.c
    ../src/session/encryption.c
    ../src/session/migration.c
    ../src/session/memory.c
    ../src/utils/string_utils.c
    ../src/utils/logging.c
    ../src/tools/tool.c
    test_agent_save.c
)
target_compile_definitions(test_agent_save PRIVATE AGENT_TEST=1)
target_include_directories(test_agent_save PRIVATE
    ${CHECK_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIRS} ${CJSON_INCLUDE_DIRS} ../src)
target_link_libraries(test_agent_save PRIVATE
    ${CHECK_LIBRARIES} ${SQLITE3_LIBRARIES} ${OPENSSL_LIBRARIES} ${CJSON_LIBRARIES})
add_test(NAME test_agent_save COMMAND test_agent_save)
