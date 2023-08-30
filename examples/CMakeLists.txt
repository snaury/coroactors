add_library(coroactors_examples
    counter.cpp
    fast_service.cpp
    queue.cpp
    timeout.cpp
)

target_link_libraries(coroactors_examples
    coroactors
)

add_subdirectory(asio_echo)
