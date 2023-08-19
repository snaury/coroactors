#!/bin/bash
clang++ -g -O3 -Wall -std=c++20 \
    -I/opt/homebrew/include \
    -L/opt/homebrew/lib \
    with_continuation_test.cpp \
    -lgtest -lgtest_main \
    && ./a.out "$@"
