#!/bin/bash
clang++ -O3 -Wall -std=c++20 benchmark_blocking_queue.cpp -I/opt/homebrew/include -L/opt/homebrew/lib -lbenchmark && ./a.out "$@"
