#!/bin/bash
clang++ -O3 -std=c++20 \
    realcheck_relaxed_mailbox_push.cpp \
    && ./a.out
