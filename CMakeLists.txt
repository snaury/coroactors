cmake_minimum_required(VERSION 3.15)
project(coroactors)

set(SANITIZE "off" CACHE STRING "Enable sanitizier: address/thread/memory/undefined")
option(WITH_COVERAGE "Compile with coverage profiling" OFF)

# This codebase is c++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# But don't want gnu++20
set(CMAKE_CXX_EXTENSIONS OFF)

#set(CMAKE_CXX_FLAGS_RELEASE -O3)
#set(CMAKE_CXX_FLAGS_RELWITHDEBINFO -O3 -g)

if((SANITIZE STREQUAL "") OR (SANITIZE STREQUAL "off"))
    # Don't enable sanitizers
else()
    # Enable the specified sanitizer
    add_compile_options(-fsanitize=${SANITIZE})
    add_link_options(-fsanitize=${SANITIZE})
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
