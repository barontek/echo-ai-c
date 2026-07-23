add_executable(test_config ../src/config/config.c ../src/utils/string_utils.c test_config.c)
target_include_directories(test_config PRIVATE ${CHECK_INCLUDE_DIRS} ../src)
target_link_libraries(test_config PRIVATE ${CHECK_LIBRARIES})
add_test(NAME test_config COMMAND test_config)

add_executable(test_string ../src/utils/string_utils.c test_string.c)
target_include_directories(test_string PRIVATE ${CHECK_INCLUDE_DIRS} ../src)
target_link_libraries(test_string PRIVATE ${CHECK_LIBRARIES})
add_test(NAME test_string COMMAND test_string)

add_executable(test_message
    ../src/agent/message.c
    ../src/utils/string_utils.c
    ../src/utils/json.c
    test_message.c
)
target_include_directories(test_message PRIVATE ${CHECK_INCLUDE_DIRS} ${CJSON_INCLUDE_DIRS} ../src)
target_link_libraries(test_message PRIVATE ${CHECK_LIBRARIES} ${CJSON_LIBRARIES})
add_test(NAME test_message COMMAND test_message)
