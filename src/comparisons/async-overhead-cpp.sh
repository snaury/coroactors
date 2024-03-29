#!/bin/bash
set -e
clang++ -O3 -std=c++20 -I../../ -o async-overhead-cpp async-overhead-cpp.cpp
./async-overhead-cpp
