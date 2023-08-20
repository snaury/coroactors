#!/bin/bash
clang++ -O3 -Wall -std=c++20 \
    -I/opt/homebrew/include \
    -L/opt/homebrew/lib \
    benchmark_actor.cpp \
    -lbenchmark \
    -lbenchmark_main \
    -labsl_synchronization \
    && ./a.out "$@"
