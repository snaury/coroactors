#!/bin/bash
clang++ -O3 -std=c++20 \
    realcheck_relaxed.cpp \
    && ./a.out
