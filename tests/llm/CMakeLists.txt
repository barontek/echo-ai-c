add_executable(test_ollama_tool_calls
    ../../src/llm/ollama.c
    curl_stub.c
    ../../src/agent/message.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_ollama_tool_calls.c
)
target_compile_definitions(test_ollama_tool_calls PRIVATE OLLAMA_TEST=1)
target_include_directories(test_ollama_tool_calls PRIVATE ../../src)
target_link_libraries(test_ollama_tool_calls PRIVATE echo_ai_test_deps)
add_test(NAME test_ollama_tool_calls COMMAND test_ollama_tool_calls)

add_executable(test_ollama
    ../../src/llm/ollama.c
    curl_stub.c
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
    ../../src/llm/openai.c
    ../../src/llm/openai_request.c
    ../../src/llm/openai_stream.c
    ../../src/llm/openai_response.c
    ../../src/utils/http_client.c
    ../../src/llm/openai_compatible.c
    ../../src/llm/openai_compatible_stream.c
    ../../src/llm/opencode_zen.c
    ../../src/agent/message.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    openai_oauth_stub.c
    test_factory.c
)
target_include_directories(test_factory PRIVATE ../../src)
target_link_libraries(test_factory PRIVATE echo_ai_test_deps)
add_test(NAME test_factory COMMAND test_factory)

add_executable(test_openai_compatible
    ../../src/llm/openai_compatible.c
    ../../src/llm/openai_compatible_stream.c
    ../../src/utils/http_client.c
    ../../src/agent/message.c
    ../../src/utils/string_utils.c
    ../../src/utils/logging.c
    test_openai_compatible.c
)
target_compile_definitions(test_openai_compatible PRIVATE OPENAI_COMPATIBLE_TEST=1)
target_include_directories(test_openai_compatible PRIVATE ../../src)
target_link_libraries(test_openai_compatible PRIVATE echo_ai_test_deps)
add_test(NAME test_openai_compatible COMMAND test_openai_compatible)

add_executable(test_openai
    ../../src/llm/openai.c
    ../../src/llm/openai_request.c
    ../../src/llm/openai_stream.c
    ../../src/llm/openai_response.c
    ../../src/utils/http_client.c
    ../../src/agent/message.c
    ../../src/utils/string_utils.c
    test_openai.c
)
target_compile_definitions(test_openai PRIVATE OPENAI_TEST=1)
target_include_directories(test_openai PRIVATE ../../src)
target_link_libraries(test_openai PRIVATE echo_ai_test_deps)
add_test(NAME test_openai COMMAND test_openai)

add_executable(test_openai_stream
    ../../src/llm/openai.c
    ../../src/llm/openai_request.c
    ../../src/llm/openai_stream.c
    ../../src/llm/openai_response.c
    ../../src/utils/http_client.c
    ../../src/agent/message.c
    ../../src/utils/string_utils.c
    test_openai_stream.c
)
target_compile_definitions(test_openai_stream PRIVATE OPENAI_TEST=1)
target_include_directories(test_openai_stream PRIVATE ../../src)
target_link_libraries(test_openai_stream PRIVATE echo_ai_test_deps)
add_test(NAME test_openai_stream COMMAND test_openai_stream)

add_executable(test_openai_oauth
    ../../src/llm/openai_oauth.c
    ../../src/llm/oauth_codec.c
    ../../src/llm/oauth_jwt.c
    ../../src/llm/oauth_vault.c
    ../../src/llm/oauth_http.c
    ../../src/llm/oauth_callback.c
    ../../src/llm/oauth_device.c
    ../../src/utils/http_client.c
    test_openai_oauth.c
)
target_compile_definitions(test_openai_oauth PRIVATE OPENAI_OAUTH_TEST=1)
target_include_directories(test_openai_oauth PRIVATE ../../src)
target_link_libraries(test_openai_oauth PRIVATE echo_ai_test_deps)
add_test(NAME test_openai_oauth COMMAND test_openai_oauth)
