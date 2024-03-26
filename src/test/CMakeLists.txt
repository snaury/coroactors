set(SIMPLE_TESTS
    test_async
    test_coro
    test_stop_token
    test_task_group
    test_task_local
    test_with_continuation
    test_with_task_group
    test_mailbox
    test_intrusive_mailbox
)

# Common files and dependencies (compile once)
add_library(test_common main.cpp)
target_link_libraries(test_common PUBLIC
    coroactors
    GTest::gtest
)

foreach(TEST ${SIMPLE_TESTS})
    # Make a separate executable for each test
    add_executable(${TEST} ${TEST}.cpp)
    target_link_libraries(${TEST} test_common)

    # Build tests before running them with `make check`
    add_dependencies(all-tests ${TEST})
endforeach()

include(GoogleTest)
foreach(TEST ${SIMPLE_TESTS})
    gtest_discover_tests(${TEST})
endforeach()
