#!/bin/bash
clang++ -g -O3 -Wall -std=c++20 -I. \
    -fsanitize=thread \
    -I/opt/homebrew/include \
    -L/opt/homebrew/lib \
    task_group_test.cpp \
    -lgtest -lgtest_main \
    && ./a.out "$@"
