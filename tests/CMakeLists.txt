add_executable(hlua_test hlua_test.cpp)

target_include_directories(hlua_test PRIVATE
	${CMAKE_SOURCE_DIR}/dlls
	${CMAKE_SOURCE_DIR}/common
	${CMAKE_SOURCE_DIR}/engine
	${CMAKE_SOURCE_DIR}/pm_shared
	${CMAKE_SOURCE_DIR}/game_shared
	${CMAKE_SOURCE_DIR}/public
	${CMAKE_SOURCE_DIR}/hlua
	${CMAKE_SOURCE_DIR}/external/lua54
)

target_link_libraries(hlua_test PRIVATE ${CMAKE_DL_LIBS})

add_test(NAME hlua_test
	COMMAND hlua_test $<TARGET_FILE:server> ${CMAKE_SOURCE_DIR}/tests/mod)
