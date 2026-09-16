cmake_minimum_required(VERSION 3.13)
project(sentum)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

option(SENTUM_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(SENTUM_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(SENTUM_ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(SENTUM_BUILD_BENCHMARKS "Build Sentum microbenchmarks" OFF)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -DNDEBUG -flto=auto")

execute_process(
	COMMAND git rev-parse --verify HEAD
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	OUTPUT_VARIABLE SENTUM_GIT_COMMIT
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET
)
if(NOT SENTUM_GIT_COMMIT)
	set(SENTUM_GIT_COMMIT "unknown")
endif()

set(SENTUM_SANITIZER_COUNT 0)
foreach(option_name SENTUM_ENABLE_ASAN SENTUM_ENABLE_UBSAN SENTUM_ENABLE_TSAN)
	if(${option_name})
		math(EXPR SENTUM_SANITIZER_COUNT "${SENTUM_SANITIZER_COUNT} + 1")
	endif()
endforeach()
if(SENTUM_SANITIZER_COUNT GREATER 1)
	message(FATAL_ERROR "Enable at most one Sentum sanitizer at a time")
endif()

if(SENTUM_SANITIZER_COUNT GREATER 0 AND NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	message(FATAL_ERROR "Sentum sanitizers require GCC or Clang")
endif()

if(SENTUM_ENABLE_ASAN)
	add_compile_options(-O1 -g -fno-omit-frame-pointer -fsanitize=address)
	add_link_options(-fsanitize=address)
elseif(SENTUM_ENABLE_UBSAN)
	add_compile_options(-O1 -g -fno-omit-frame-pointer -fsanitize=undefined)
	add_link_options(-fsanitize=undefined)
elseif(SENTUM_ENABLE_TSAN)
	add_compile_options(-O1 -g -fno-omit-frame-pointer -fsanitize=thread)
	add_link_options(-fsanitize=thread)
endif()

find_package(CURL REQUIRED)
find_package(Boost REQUIRED system)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

file(GLOB_RECURSE SENTUM_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
set(SENTUM_CORE_SOURCES ${SENTUM_SOURCES})
list(FILTER SENTUM_CORE_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

add_library(sentum_core STATIC ${SENTUM_CORE_SOURCES})
target_include_directories(
	sentum_core
	PUBLIC
		${CMAKE_SOURCE_DIR}/src
	PRIVATE
		${CURL_INCLUDE_DIR}
)
target_link_libraries(
	sentum_core
	PUBLIC
		Boost::system
		OpenSSL::SSL
		OpenSSL::Crypto
		SQLite::SQLite3
		Threads::Threads
		${CURL_LIBRARIES}
)

add_executable(sentum src/main.cpp)
target_compile_definitions(sentum PRIVATE SENTUM_GIT_COMMIT="${SENTUM_GIT_COMMIT}")
target_link_libraries(sentum PRIVATE sentum_core)

add_executable(sentum_portfolio_research tools/portfolio_research_main.cpp)
target_link_libraries(sentum_portfolio_research PRIVATE sentum_core)

add_executable(sentum_experiment tools/experiment_main.cpp)
target_link_libraries(sentum_experiment PRIVATE sentum_core)

add_executable(sentum_model tools/model_promotion_main.cpp)
target_link_libraries(sentum_model PRIVATE sentum_core)

if(BUILD_TESTING)
	add_executable(sentum_runtime_lifecycle_tests tests/runtime_lifecycle_tests.cpp)
	target_link_libraries(sentum_runtime_lifecycle_tests PRIVATE sentum_core)
	add_test(NAME runtime_lifecycle COMMAND sentum_runtime_lifecycle_tests)
	set_tests_properties(runtime_lifecycle PROPERTIES TIMEOUT 60)

	add_executable(sentum_market_hot_path_tests tests/market_hot_path_tests.cpp)
	target_link_libraries(sentum_market_hot_path_tests PRIVATE sentum_core)
	add_test(NAME market_hot_path COMMAND sentum_market_hot_path_tests)
	set_tests_properties(market_hot_path PROPERTIES TIMEOUT 60)

	add_executable(sentum_runtime_observability_tests tests/runtime_observability_tests.cpp)
	target_link_libraries(sentum_runtime_observability_tests PRIVATE sentum_core)
	add_test(NAME runtime_observability COMMAND sentum_runtime_observability_tests)
	set_tests_properties(runtime_observability PROPERTIES TIMEOUT 60)

	add_executable(sentum_dashboard_snapshot_tests tests/dashboard_snapshot_tests.cpp)
	target_link_libraries(sentum_dashboard_snapshot_tests PRIVATE sentum_core)
	add_test(NAME dashboard_snapshot COMMAND sentum_dashboard_snapshot_tests)
	set_tests_properties(dashboard_snapshot PROPERTIES TIMEOUT 60)

	add_executable(sentum_terminal_ui_policy_tests tests/terminal_ui_policy_tests.cpp)
	target_link_libraries(sentum_terminal_ui_policy_tests PRIVATE sentum_core)
	add_test(NAME terminal_ui_policy COMMAND sentum_terminal_ui_policy_tests)
	set_tests_properties(terminal_ui_policy PROPERTIES TIMEOUT 60)

	add_executable(sentum_terminal_render_pipeline_tests tests/terminal_render_pipeline_tests.cpp)
	target_link_libraries(sentum_terminal_render_pipeline_tests PRIVATE sentum_core)
	add_test(NAME terminal_render_pipeline COMMAND sentum_terminal_render_pipeline_tests)
	set_tests_properties(terminal_render_pipeline PROPERTIES TIMEOUT 60)
endif()

if(SENTUM_BUILD_BENCHMARKS)
	add_executable(sentum_market_benchmark benchmarks/market_path_benchmark.cpp)
	target_include_directories(sentum_market_benchmark PRIVATE ${CMAKE_SOURCE_DIR}/src)
	target_link_libraries(sentum_market_benchmark PRIVATE Threads::Threads)

	add_executable(sentum_parser_allocation_benchmark benchmarks/parser_allocation_benchmark.cpp)
	target_include_directories(sentum_parser_allocation_benchmark PRIVATE ${CMAKE_SOURCE_DIR}/src)

	add_executable(sentum_scanner_hot_path_benchmark benchmarks/scanner_hot_path_benchmark.cpp)
	target_link_libraries(sentum_scanner_hot_path_benchmark PRIVATE sentum_core)

	add_executable(sentum_dashboard_snapshot_benchmark benchmarks/dashboard_snapshot_benchmark.cpp)
	target_link_libraries(sentum_dashboard_snapshot_benchmark PRIVATE sentum_core)

	add_executable(sentum_terminal_render_pipeline_benchmark benchmarks/terminal_render_pipeline_benchmark.cpp)
	target_link_libraries(sentum_terminal_render_pipeline_benchmark PRIVATE sentum_core)
endif()

if(NOT SENTUM_ENABLE_ASAN AND NOT SENTUM_ENABLE_UBSAN AND NOT SENTUM_ENABLE_TSAN)
	add_custom_command(
		TARGET sentum POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:sentum> ${CMAKE_SOURCE_DIR}/sentum
		COMMAND ${CMAKE_STRIP} ${CMAKE_SOURCE_DIR}/sentum
		COMMENT "Strip optimized Sentum binary"
	)
endif()
