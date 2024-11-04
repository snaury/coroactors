#!/bin/bash
set -e
clang++ -O3 -std=c++20 -I../../ -o coro-overhead-cpp coro-overhead-cpp.cpp
./coro-overhead-cpp
