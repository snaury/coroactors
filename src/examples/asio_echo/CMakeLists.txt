if(asio_FOUND)

    add_executable(asio_echo
        asio_echo.cpp
    )

    target_link_libraries(asio_echo
        coroactors
        PkgConfig::asio
    )

endif()
