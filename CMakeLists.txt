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

# Some targets require asio to build
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    # We need 1.23 and it is also the first version with pkg-config support
    pkg_check_modules(asio QUIET IMPORTED_TARGET asio>=1.23)
else()
    set(asio_FOUND 0)
endif()

enable_testing()

# Support for `make all-tests` which builds all tests
add_custom_target(all-tests)

# Support for `make check` which builds all tests first
add_custom_target(check ${CMAKE_CTEST_COMMAND})
add_dependencies(check all-tests)

add_subdirectory(src)
