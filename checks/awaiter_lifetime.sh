#!/bin/bash
clang++ -g -O3 -Wall -std=c++20 \
    awaiter_lifetime.cpp \
    && ./a.out "$@"
