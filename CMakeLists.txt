cmake_minimum_required(VERSION 3.10)
project(sentum)

set(CMAKE_CXX_STANDARD 17)

include_directories(
	include
	lib
	/usr/include
	${CURL_INCLUDE_DIR}
)

file(GLOB SOURCES "src/*.cpp")

add_executable(
	client
	${SOURCES}
)

# CURL + Boost + OpenSSL
find_package(CURL REQUIRED)
find_package(Boost REQUIRED system)
find_package(OpenSSL REQUIRED)

target_link_libraries(
	client
	PRIVATE
		Boost::system
		OpenSSL::SSL
		OpenSSL::Crypto
		pthread
		${CURL_LIBRARIES}
)