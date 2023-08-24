#!/bin/bash
OPTS=()
OPTS+=(-fsanitize=thread)
#OPTS+=(-fprofile-instr-generate -fcoverage-mapping)
clang++ -g -O3 -Wall -std=c++20 -I. \
    "${OPTS[@]}" \
    -I/opt/homebrew/include \
    -L/opt/homebrew/lib \
    stop_token_test.cpp \
    -lgtest -lgtest_main \
    && ./a.out "$@"
