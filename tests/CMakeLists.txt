add_compile_options(-Wall -Wextra -Wpedantic -Werror -g)
if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(echo_ai_test_deps INTERFACE)
target_link_libraries(echo_ai_test_deps INTERFACE
    PkgConfig::CHECK
    PkgConfig::LIBUV
    PkgConfig::LIBCURL
    PkgConfig::SQLITE3
    PkgConfig::OPENSSL
    PkgConfig::CJSON
    m
)

add_subdirectory(utils)
add_subdirectory(agent)
add_subdirectory(session)
add_subdirectory(tools)
add_subdirectory(routes)
add_subdirectory(server)
add_subdirectory(llm)

# --- Fuzz targets (libFuzzer) ---
# Section 6.3 of AGENTS.md requires fuzz targets for any function parsing
# external input.  These targets are built only when the compiler supports
# -fsanitize=fuzzer (clang / recent gcc).
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer,address,undefined")
check_c_source_compiles(
    "int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long s) { return 0; }"
    HAS_LIBFUZZER
)
set(CMAKE_REQUIRED_FLAGS "")

if(HAS_LIBFUZZER)
    # fuzz_ollama_tool_calls — standalone mirror (same pattern as test_ollama_tool_calls.c)
    add_executable(fuzz_ollama_tool_calls fuzz/fuzz_ollama_tool_calls.c)
    target_include_directories(fuzz_ollama_tool_calls PRIVATE ../src)
    target_compile_options(fuzz_ollama_tool_calls PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -Wno-unused-parameter)
    target_link_libraries(fuzz_ollama_tool_calls PRIVATE
        -fsanitize=fuzzer,address,undefined PkgConfig::CJSON)

    # fuzz_config — exercises conf_load with fuzzed .conf file contents
    add_executable(fuzz_config
        ../src/config/config.c
        ../src/utils/string_utils.c
        fuzz/fuzz_config.c
    )
    target_include_directories(fuzz_config PRIVATE ../src)
    target_compile_options(fuzz_config PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -Wno-unused-parameter)
    target_link_libraries(fuzz_config PRIVATE
        -fsanitize=fuzzer,address,undefined)

    # fuzz_session_deserialize — exercises session_deserialize_messages
    add_executable(fuzz_session_deserialize
        ../src/session/session.c
        ../src/agent/message.c
        ../src/utils/string_utils.c
        ../src/utils/json.c
        fuzz/fuzz_session_deserialize.c
    )
    target_include_directories(fuzz_session_deserialize PRIVATE ../src)
    target_compile_options(fuzz_session_deserialize PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -Wno-unused-parameter)
    target_link_libraries(fuzz_session_deserialize PRIVATE
        -fsanitize=fuzzer,address,undefined PkgConfig::CJSON)

    # fuzz_html_extract — exercises html_extract_text with fuzzed raw HTML
    add_executable(fuzz_html_extract
        ../src/utils/html_extract.c
        ../src/utils/string_utils.c
        fuzz/fuzz_html_extract.c
    )
    target_include_directories(fuzz_html_extract PRIVATE ../src)
    target_compile_options(fuzz_html_extract PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -Wno-unused-parameter)
    target_link_libraries(fuzz_html_extract PRIVATE
        -fsanitize=fuzzer,address,undefined m)

    add_executable(fuzz_http_parser
        ../src/server/server.c
        ../src/utils/string_utils.c
        fuzz/fuzz_http_parser.c
    )
    target_compile_definitions(fuzz_http_parser PRIVATE SERVER_TEST=1)
    target_include_directories(fuzz_http_parser PRIVATE ../src)
    target_compile_options(fuzz_http_parser PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)
    target_link_libraries(fuzz_http_parser PRIVATE
        -fsanitize=fuzzer,address,undefined PkgConfig::LIBUV PkgConfig::CJSON)

    add_executable(fuzz_openai_compatible_stream
        ../src/llm/openai_compatible.c
        ../src/agent/message.c
        ../src/utils/string_utils.c
        ../src/utils/logging.c
        fuzz/fuzz_openai_compatible_stream.c
    )
    target_compile_definitions(fuzz_openai_compatible_stream PRIVATE OPENAI_COMPATIBLE_TEST=1)
    target_include_directories(fuzz_openai_compatible_stream PRIVATE ../src)
    target_compile_options(fuzz_openai_compatible_stream PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)
    target_link_libraries(fuzz_openai_compatible_stream PRIVATE
        -fsanitize=fuzzer,address,undefined PkgConfig::LIBCURL PkgConfig::CJSON)

    foreach(OAUTH_FUZZ_TARGET IN ITEMS callback token jwt)
        add_executable(fuzz_openai_oauth_${OAUTH_FUZZ_TARGET}
            ../src/llm/openai_oauth.c
            fuzz/openai_oauth_stubs.c
            fuzz/fuzz_openai_oauth_${OAUTH_FUZZ_TARGET}.c
        )
        target_compile_definitions(fuzz_openai_oauth_${OAUTH_FUZZ_TARGET}
            PRIVATE OPENAI_OAUTH_TEST=1)
        target_include_directories(fuzz_openai_oauth_${OAUTH_FUZZ_TARGET} PRIVATE ../src)
        target_compile_options(fuzz_openai_oauth_${OAUTH_FUZZ_TARGET} PRIVATE
            -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)
        target_link_libraries(fuzz_openai_oauth_${OAUTH_FUZZ_TARGET} PRIVATE
            -fsanitize=fuzzer,address,undefined PkgConfig::LIBCURL
            PkgConfig::OPENSSL PkgConfig::CJSON m)
    endforeach()

    add_executable(fuzz_openai_codex_stream
        ../src/llm/openai.c
        ../src/agent/message.c
        ../src/utils/string_utils.c
        fuzz/openai_provider_stubs.c
        fuzz/fuzz_openai_codex_stream.c
    )
    target_compile_definitions(fuzz_openai_codex_stream PRIVATE OPENAI_TEST=1)
    target_include_directories(fuzz_openai_codex_stream PRIVATE ../src)
    target_compile_options(fuzz_openai_codex_stream PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)
    target_link_libraries(fuzz_openai_codex_stream PRIVATE
        -fsanitize=fuzzer,address,undefined PkgConfig::LIBCURL PkgConfig::CJSON
        PkgConfig::OPENSSL m)
endif()
