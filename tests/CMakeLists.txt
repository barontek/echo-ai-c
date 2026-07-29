add_subdirectory(utils)
add_subdirectory(agent)
add_subdirectory(session)
add_subdirectory(tools)
add_subdirectory(routes)
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
endif()
