add_executable(test_middleware
    ../../src/server/middleware.c
    test_middleware.c
)
target_include_directories(test_middleware PRIVATE ../../src)
target_link_libraries(test_middleware PRIVATE PkgConfig::CHECK PkgConfig::LIBUV PkgConfig::OPENSSL)
add_test(NAME test_middleware COMMAND test_middleware)
