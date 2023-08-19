#!/bin/bash
clang++ -O3 -Wall -std=c++20 with_continuation_test.cpp -I/opt/homebrew/include -L/opt/homebrew/lib -lgtest -lgtest_main && ./a.out "$@"
