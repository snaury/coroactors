#!/bin/bash
set -e
clang++ -O3 -std=c++20 \
    -I/opt/homebrew/include \
    -L/opt/homebrew/lib \
    -lboost_context-mt \
    -o boost-context-overhead \
    boost-context-overhead.cpp
./boost-context-overhead
