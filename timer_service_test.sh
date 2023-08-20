#!/bin/bash
clang++ -O3 -Wall -std=c++20 \
    -I/opt/homebrew/include \
    -L/opt/homebrew/lib \
    timer_service_test.cpp \
    -labsl_synchronization \
    && ./a.out "$@"
