cmake_minimum_required(VERSION 3.10)
project(sentum)

set(CMAKE_CXX_STANDARD 17)

include_directories(
	include
	lib
	/usr/include
)

add_executable(
	client
	src/main.cpp
	src/wsclient.cpp
	src/rsi.cpp
	src/sma.cpp
	src/strategy.cpp
	src/trader.cpp
	src/chart.cpp
)

# Boost + OpenSSL
find_package(Boost REQUIRED system)
find_package(OpenSSL REQUIRED)

target_link_libraries(client
	PRIVATE
		Boost::system
		OpenSSL::SSL
		OpenSSL::Crypto
		pthread
)