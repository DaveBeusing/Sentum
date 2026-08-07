cmake_minimum_required(VERSION 3.10)
project(sentum)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SENTUM_ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(SENTUM_BUILD_BENCHMARKS "Build Sentum microbenchmarks" OFF)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -DNDEBUG -flto=auto")

if(SENTUM_ENABLE_TSAN)
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
		message(FATAL_ERROR "ThreadSanitizer requires GCC or Clang")
	endif()
	add_compile_options(-O1 -g -fno-omit-frame-pointer -fsanitize=thread)
	add_link_options(-fsanitize=thread)
endif()

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")

add_executable(client ${SOURCES})

set(SENTUM_CORE_SOURCES ${SOURCES})
list(FILTER SENTUM_CORE_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")
add_executable(sentum_portfolio_research tools/portfolio_research_main.cpp ${SENTUM_CORE_SOURCES})

find_package(CURL REQUIRED)
find_package(Boost REQUIRED system)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

foreach(target client sentum_portfolio_research)
	target_include_directories(
		${target}
		PRIVATE
			${CMAKE_SOURCE_DIR}/src
			${CURL_INCLUDE_DIR}
	)

	target_link_libraries(
		${target}
		PRIVATE
			Boost::system
			OpenSSL::SSL
			OpenSSL::Crypto
			SQLite::SQLite3
			Threads::Threads
			${CURL_LIBRARIES}
	)
endforeach()

if(SENTUM_BUILD_BENCHMARKS)
	add_executable(sentum_market_benchmark benchmarks/market_path_benchmark.cpp)
	target_include_directories(sentum_market_benchmark PRIVATE ${CMAKE_SOURCE_DIR}/src)
	target_link_libraries(sentum_market_benchmark PRIVATE Threads::Threads)
endif()

if(NOT SENTUM_ENABLE_TSAN)
	add_custom_command(
		TARGET client POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:client> ${CMAKE_SOURCE_DIR}/client
		COMMAND ${CMAKE_STRIP} ${CMAKE_SOURCE_DIR}/client
		COMMENT "Strip optimized binary"
	)
endif()
