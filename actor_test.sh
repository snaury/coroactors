#!/bin/bash
clang++ -g -O3 -Wall -std=c++20 -I. \
    -fsanitize=address \
    -I/opt/homebrew/include \
    -L/opt/homebrew/lib \
    actor_test.cpp \
    -lgtest -lgtest_main \
    && ./a.out "$@"