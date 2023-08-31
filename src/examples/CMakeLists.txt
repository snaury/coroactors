add_library(examples
    counter.cpp
    fast_service.cpp
    queue.cpp
    timeout.cpp
)

target_link_libraries(examples
    coroactors
)

add_subdirectory(asio_echo)
