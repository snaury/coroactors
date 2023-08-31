cmake_minimum_required(VERSION 3.15)
project(coroactors)

option(WITH_ASAN "Enable address sanitizer" OFF)
option(WITH_TSAN "Enable thread sanitizer" OFF)
option(WITH_COVERAGE "Compile with coverage profiling" OFF)

# This codebase is c++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# But don't want gnu++20
set(CMAKE_CXX_EXTENSIONS OFF)

if(WITH_ASAN)
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif()

if(WITH_TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

if(WITH_COVERAGE)
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate -fcoverage-mapping)
endif()

# coroactors is a headers-only library
add_library(coroactors INTERFACE)
target_include_directories(coroactors INTERFACE ${PROJECT_SOURCE_DIR})

enable_testing()

# Support for `make check` which builds tests first
add_custom_target(check ${CMAKE_CTEST_COMMAND})

add_subdirectory(src)
