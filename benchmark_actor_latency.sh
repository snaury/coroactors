#!/bin/bash
clang++ -O3 -Wall -std=c++20 benchmark_actor_latency.cpp -I/opt/homebrew/include -L/opt/homebrew/lib && ./a.out "$@"
