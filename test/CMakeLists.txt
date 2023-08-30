find_package(GTest)

if(GTest_FOUND)

    add_executable(coroactors_test)

    target_sources(coroactors_test PRIVATE
        main.cpp
        test_actor.cpp
        test_stop_token.cpp
        test_task_group.cpp
        test_with_continuation.cpp
    )

    target_link_libraries(coroactors_test PUBLIC
        coroactors
        GTest::gtest
    )

    include(GoogleTest)
    gtest_discover_tests(coroactors_test)

else()

    message(WARNING "GTest is required for building tests")

endif()
