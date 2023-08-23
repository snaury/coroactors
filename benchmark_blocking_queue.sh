#!/bin/bash
clang++ -O3 -Wall -std=c++20 -I. \
    -I/opt/homebrew/include \
    -L/opt/homebrew/lib \
    benchmark_blocking_queue.cpp \
    -lbenchmark \
    && ./a.out "$@"
