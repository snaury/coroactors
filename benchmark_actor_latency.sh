#!/bin/bash
clang++ -O3 -Wall -std=c++20 -I. \
    -I/opt/homebrew/include \
    -L/opt/homebrew/lib \
    benchmark_actor_latency.cpp \
    -labsl_synchronization \
    && ./a.out "$@"
