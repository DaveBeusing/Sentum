cmake_minimum_required(VERSION 3.10)
project(sentum)

set(CMAKE_CXX_STANDARD 17)

file(GLOB_RECURSE SOURCES "src/*.cpp")

add_executable(
	client
	${SOURCES}
)

target_include_directories(
	client
	PRIVATE
		${CMAKE_SOURCE_DIR}/src
		${CURL_INCLUDE_DIR}
		/usr/include
)

# CURL + Boost + OpenSSL + SQLite3
find_package(CURL REQUIRED)
find_package(Boost REQUIRED system)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)

target_link_libraries(
	client
	PRIVATE
		Boost::system
		OpenSSL::SSL
		OpenSSL::Crypto
		SQLite::SQLite3
		pthread
		${CURL_LIBRARIES}
)

add_custom_command(
	TARGET client POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:client> ${CMAKE_SOURCE_DIR}/client
	COMMAND strip ${CMAKE_SOURCE_DIR}/client
	COMMENT "Copy and optimize binary with strip"
)