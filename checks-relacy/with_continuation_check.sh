#!/bin/bash
# note: relacy crashes on arm
arch -x86_64 clang++ -O3 -std=c++20 \
    -I$(echo ~/tmp/src.github/relacy) \
    with_continuation_check.cpp \
    && ./a.out
