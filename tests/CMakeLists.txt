find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(test-provider test-provider.c)
target_link_libraries(test-provider PRIVATE PkgConfig::CAMEL)

add_test(NAME provider-build-tree COMMAND test-provider $<TARGET_FILE:camelgroupwise>)

add_executable(test-connection test-connection.c)
target_link_libraries(test-connection PRIVATE egroupwise PkgConfig::SOUP)

add_test(NAME connection
	COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/mock/run-with-mock.py" $<TARGET_FILE:test-connection>)
set_tests_properties(connection PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "TZ=Europe/Berlin")

add_executable(test-camel test-camel.c)
target_link_libraries(test-camel PRIVATE PkgConfig::CAMEL PkgConfig::SOUP)

add_test(NAME camel
	COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/mock/run-with-mock.py"
		$<TARGET_FILE:test-camel> $<TARGET_FILE:camelgroupwise>)
set_tests_properties(camel PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test-utils test-utils.c
	"${PROJECT_SOURCE_DIR}/src/camel/camel-groupwise-utils.c"
	"${PROJECT_SOURCE_DIR}/src/camel/camel-groupwise-message-info.c"
	"${PROJECT_SOURCE_DIR}/src/camel/camel-groupwise-mime.c")
target_include_directories(test-utils PRIVATE "${PROJECT_SOURCE_DIR}/src/camel")
target_link_libraries(test-utils PRIVATE egroupwise egroupwise-eds PkgConfig::CAMEL)

add_test(NAME utils COMMAND test-utils)

add_executable(test-contact test-contact.c)
target_link_libraries(test-contact PRIVATE gw-contact)

add_test(NAME contact COMMAND test-contact)

add_executable(test-calendar test-calendar.c)
target_link_libraries(test-calendar PRIVATE gw-calendar)

add_test(NAME calendar COMMAND test-calendar)
