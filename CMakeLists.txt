cmake_minimum_required(VERSION 3.10)
project(sentum)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -DNDEBUG -flto=auto")

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

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
	COMMENT " ⇒ Strip optimize binary..."
)