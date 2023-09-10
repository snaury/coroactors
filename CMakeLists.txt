cmake_minimum_required(VERSION 3.15)
project(coroactors LANGUAGES CXX)

cmake_policy(PUSH)
cmake_policy(VERSION 3.15...3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(COROACTORS_IS_ROOT ON)
else()
    # included as a subdirectory in another project
    set(COROACTORS_IS_ROOT OFF)
endif()

if(COROACTORS_IS_ROOT)
    # This codebase is c++20
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    # But don't want gnu++20
    set(CMAKE_CXX_EXTENSIONS OFF)

    if(NOT MSVC)
        set(CMAKE_CXX_FLAGS "-Wall -Wextra -Werror")
        #set(CMAKE_CXX_FLAGS "-Wall -Wextra -Wpedantic -Werror")
        #set(CMAKE_CXX_FLAGS_RELEASE "-O3")
        #set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O3 -g")
    endif()

    set(SANITIZE "off" CACHE STRING "Enable sanitizier: address/thread/memory/undefined")
    option(WITH_COVERAGE "Compile with coverage profiling" OFF)

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
endif()

# coroactors is a headers-only library
add_library(coroactors INTERFACE)
target_include_directories(coroactors INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

if(COROACTORS_IS_ROOT)
    include(FetchContent)

    # Use system gtest or download from github when unavailable
    # Unfortunately FIND_PACKAGE_ARGS is not available in cmake 3.22 :(
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        )
        # For Windows: Prevent overriding the parent project's compiler/linker settings
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    # Use system google-benchmark or download from github when unavailable
    find_package(benchmark QUIET)
    if(NOT benchmark_FOUND)
        FetchContent_Declare(
            googlebenchmark
            URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.zip
        )
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        #set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
        #set(BENCHMARK_ENABLE_ASSEMBLY_TESTS OFF CACHE BOOL "" FORCE)
        #set(BENCHMARK_USE_BUNDLED_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googlebenchmark)
    endif()

    # Use system absl or download from github when unavailable
    # Always recompile from sources when sanitizers are enabled
    if((SANITIZE STREQUAL "") OR (SANITIZE STREQUAL "off"))
        find_package(absl QUIET)
    endif()
    if(NOT absl_FOUND)
        FetchContent_Declare(
            absl
            URL https://github.com/abseil/abseil-cpp/archive/refs/tags/20230802.0.zip
        )
        set(ABSL_USE_EXTERNAL_GOOGLETEST ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(absl)
    endif()

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
endif()

cmake_policy(POP)
