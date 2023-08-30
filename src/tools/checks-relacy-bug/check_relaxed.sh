#!/bin/bash
# note: relacy crashes on arm
arch -x86_64 clang++ -DRL_DO_ASSERT=1 -O3 -std=c++20 \
    -I$(echo ~/tmp/src.github/relacy) \
    check_relaxed.cpp \
    && ./a.out
