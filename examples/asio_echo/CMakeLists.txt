find_package(Boost COMPONENTS headers)

if(Boost_FOUND)

    add_executable(asio_echo)

    target_sources(asio_echo PRIVATE
        asio_echo.cpp
    )

    target_link_libraries(asio_echo PRIVATE
        coroactors
        Boost::headers
    )

endif()
