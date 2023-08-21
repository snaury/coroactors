#!/bin/bash
set -e
cd examples
for src in *.cpp; do
    echo Compiling $src...
    clang++ -c -O3 -Wall -std=c++20 -I.. \
        $src
done
