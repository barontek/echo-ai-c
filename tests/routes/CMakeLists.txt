add_executable(test_routes
    ../../src/server/routes/routes.c
    test_routes.c
)
target_compile_definitions(test_routes PRIVATE ROUTES_TEST=1)
target_include_directories(test_routes PRIVATE ../../src)
target_link_libraries(test_routes PRIVATE echo_ai_test_deps)
add_test(NAME test_routes COMMAND test_routes)

add_executable(test_routes_session
    ../../src/server/routes/routes_session.c
    test_routes_session.c
)
target_compile_definitions(test_routes_session PRIVATE ROUTES_SESSION_TEST=1)
target_include_directories(test_routes_session PRIVATE ../../src)
target_link_libraries(test_routes_session PRIVATE echo_ai_test_deps)
add_test(NAME test_routes_session COMMAND test_routes_session)

add_executable(test_routes_session_handlers
    ../../src/server/routes/routes_session.c
    ../../src/utils/string_utils.c
    test_routes_session_handlers.c
)
target_include_directories(test_routes_session_handlers PRIVATE ../../src)
target_link_libraries(test_routes_session_handlers PRIVATE echo_ai_test_deps)
add_test(NAME test_routes_session_handlers COMMAND test_routes_session_handlers)

add_executable(test_routes_general
    ../../src/server/routes/routes_general.c
    ../../src/config/config.c
    ../../src/utils/string_utils.c
    openai_oauth_stub.c
    test_routes_general.c
)
target_include_directories(test_routes_general PRIVATE ../../src)
target_link_libraries(test_routes_general PRIVATE echo_ai_test_deps)
add_test(NAME test_routes_general COMMAND test_routes_general)

add_executable(test_routes_auth
    ../../src/server/routes/routes_auth.c
    ../../src/utils/string_utils.c
    openai_oauth_stub.c
    test_routes_auth.c
)
target_include_directories(test_routes_auth PRIVATE ../../src)
target_link_libraries(test_routes_auth PRIVATE echo_ai_test_deps)
add_test(NAME test_routes_auth COMMAND test_routes_auth)

add_executable(test_routes_chat
    ../../src/server/routes/routes_chat.c
    ../../src/utils/string_utils.c
    test_routes_chat.c
)
target_include_directories(test_routes_chat PRIVATE ../../src)
target_link_libraries(test_routes_chat PRIVATE echo_ai_test_deps)
add_test(NAME test_routes_chat COMMAND test_routes_chat)

add_executable(test_routes_ws
    ../../src/server/routes/routes_ws.c
    ../../src/server/routes/routes.c
    ../../src/config/config.c
    ../../src/utils/string_utils.c
    ../../src/llm/factory.c
    test_routes_ws.c
)
target_include_directories(test_routes_ws PRIVATE ../../src)
target_compile_definitions(test_routes_ws PRIVATE ROUTES_WS_TEST ROUTES_TEST)
target_link_libraries(test_routes_ws PRIVATE echo_ai_test_deps)
add_test(NAME test_routes_ws COMMAND test_routes_ws)
