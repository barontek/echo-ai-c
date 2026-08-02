cmake_minimum_required(VERSION 3.16)
project(echo-ai VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBUV REQUIRED IMPORTED_TARGET libuv)
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(OPENSSL REQUIRED IMPORTED_TARGET openssl)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)

add_executable(echo-ai
    src/main.c
    src/config/config.c
    src/utils/string_utils.c
    src/utils/logging.c
    src/utils/json.c
    src/agent/message.c
    src/agent/agent.c
    src/agent/context.c
    src/llm/ollama.c
    src/llm/openai.c
    src/llm/factory.c
    src/tools/tool.c
    src/tools/registry.c
    src/tools/bash.c
    src/tools/read_file.c
    src/tools/write_file.c
    src/tools/list_dir.c
    src/tools/glob_tool.c
    src/tools/grep_tool.c
    src/tools/web_fetch.c
    src/tools/web_search.c
    src/tools/replace_in_file.c
    src/tools/python_execute.c
    src/tools/rest_api.c
    src/tools/notes.c
    src/tools/git.c
    src/tools/ingest_document.c
    src/tools/semantic_search.c
    src/tools/deep_search.c
    src/tools/tool_memory.c
    src/tools/tool_delegate.c
    src/tools/tool_sqlite_query.c
    src/tools/tool_sqlite_schema.c
    src/tools/tool_ask_user.c
    src/tools/tool_humanizer.c
    src/tools/search_provider.c
    src/tools/search_brave.c
    src/tools/search_duckduckgo.c
    src/tools/search_tavily.c
    src/safety/safety.c
    src/session/session.c
    src/session/encryption.c
    src/session/session_manager.c
    src/session/memory.c
    src/session/migration.c
    src/utils/rate_limiter.c
    src/utils/circuit_breaker.c
    src/utils/callbacks.c
    src/utils/metrics.c
    src/change_tracker/change_tracker.c
    src/server/server.c
    src/server/websocket.c
    src/server/routes/routes.c
    src/server/routes/routes_auth.c
    src/server/routes/routes_session.c
    src/server/routes/routes_chat.c
    src/server/routes/routes_general.c
    src/server/routes/routes_ws.c
    src/server/middleware.c
)

target_link_libraries(echo-ai PRIVATE
    PkgConfig::LIBUV
    PkgConfig::LIBCURL
    PkgConfig::SQLITE3
    PkgConfig::OPENSSL
    PkgConfig::CJSON
    m
)

option(ENABLE_SANITIZERS "Build with ASan/UBSan" ON)
option(ENABLE_COVERAGE "Build with code coverage instrumentation (--coverage)" OFF)

if(ENABLE_COVERAGE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(echo-ai PRIVATE -Wall -Wextra -Wpedantic -Werror -g)
    if(ENABLE_SANITIZERS)
        target_compile_options(echo-ai PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(echo-ai PRIVATE -fsanitize=address,undefined)
    endif()
else()
    target_compile_options(echo-ai PRIVATE -Wall -Wextra -O2)
endif()

# Frontend build (requires Node.js)
set(FRONTEND_DIR "${CMAKE_SOURCE_DIR}/frontend")
add_custom_target(frontend-build
    COMMAND npm ci
    COMMAND npm run build
    WORKING_DIRECTORY "${FRONTEND_DIR}"
    COMMENT "Building frontend..."
)

# Tests
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    pkg_check_modules(CHECK IMPORTED_TARGET check)
    if(CHECK_FOUND)
        enable_testing()
        add_subdirectory(tests)
    endif()
endif()
