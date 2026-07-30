add_executable(test_ollama_tool_calls test_ollama_tool_calls.c)
target_include_directories(test_ollama_tool_calls PRIVATE ../../src)
target_link_libraries(test_ollama_tool_calls PRIVATE echo_ai_test_deps)
add_test(NAME test_ollama_tool_calls COMMAND test_ollama_tool_calls)

add_executable(test_ollama
    ../../src/llm/ollama.c
    ../../src/agent/message.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_ollama.c
)
target_compile_definitions(test_ollama PRIVATE OLLAMA_TEST=1)
target_include_directories(test_ollama PRIVATE ../../src)
target_link_libraries(test_ollama PRIVATE echo_ai_test_deps)
add_test(NAME test_ollama COMMAND test_ollama)

add_executable(test_factory
    ../../src/llm/factory.c
    ../../src/llm/ollama.c
    ../../src/agent/message.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_factory.c
)
target_include_directories(test_factory PRIVATE ../../src)
target_link_libraries(test_factory PRIVATE echo_ai_test_deps)
add_test(NAME test_factory COMMAND test_factory)
