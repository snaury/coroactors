add_executable(benchmark_actor benchmark_actor.cpp)

target_link_libraries(benchmark_actor
    coroactors
    benchmark::benchmark
)

target_compile_definitions(benchmark_actor PRIVATE HAVE_ABSEIL=1)
target_link_libraries(benchmark_actor absl::synchronization)

add_executable(benchmark_blocking_queue benchmark_blocking_queue.cpp)

target_link_libraries(benchmark_blocking_queue
    coroactors
    benchmark::benchmark
)

add_executable(benchmark_mailbox benchmark_mailbox.cpp)

target_link_libraries(benchmark_mailbox
    coroactors
    benchmark::benchmark
)

add_executable(benchmark_actor_latency benchmark_actor_latency.cpp)

target_link_libraries(benchmark_actor_latency
    coroactors
)

target_compile_definitions(benchmark_actor_latency PRIVATE HAVE_ABSEIL=1)
target_link_libraries(benchmark_actor_latency absl::synchronization)

if(asio_FOUND)
    target_compile_definitions(benchmark_actor_latency PRIVATE HAVE_ASIO=1)
    target_link_libraries(benchmark_actor_latency PkgConfig::asio)
endif()
