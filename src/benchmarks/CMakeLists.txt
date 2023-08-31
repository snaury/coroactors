find_package(absl COMPONENTS synchronization)
find_package(benchmark)

if(absl_FOUND AND benchmark_FOUND)

    add_executable(benchmark_actor benchmark_actor.cpp)

    target_link_libraries(benchmark_actor
        coroactors
        absl::synchronization
        benchmark::benchmark
    )

endif()

if(absl_FOUND AND asio_FOUND)

    add_executable(benchmark_actor_latency benchmark_actor_latency.cpp)

    target_link_libraries(benchmark_actor_latency
        coroactors
        absl::synchronization
        PkgConfig::asio
    )

endif()

if(benchmark_FOUND)

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

endif()
