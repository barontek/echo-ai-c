add_executable(test_middleware
    ../../src/server/middleware.c
    test_middleware.c
)
target_include_directories(test_middleware PRIVATE ../../src)
target_link_libraries(test_middleware PRIVATE echo_ai_test_deps)
add_test(NAME test_middleware COMMAND test_middleware)

add_executable(test_server
    ../../src/server/server.c
    ../../src/server/serve_static.c
    ../../src/server/http_parse.c
    ../../src/utils/string_utils.c
    test_server.c
)
target_compile_definitions(test_server PRIVATE SERVER_TEST=1)
target_include_directories(test_server PRIVATE ../../src)
target_link_libraries(test_server PRIVATE echo_ai_test_deps)
add_test(NAME test_server COMMAND test_server)

add_executable(test_websocket
    ../../src/server/websocket.c
    test_websocket.c
)
target_compile_definitions(test_websocket PRIVATE WEBSOCKET_TEST=1)
target_include_directories(test_websocket PRIVATE ../../src)
target_link_libraries(test_websocket PRIVATE echo_ai_test_deps)
add_test(NAME test_websocket COMMAND test_websocket)
