#!/bin/bash
#export LIBDISPATCH_COOPERATIVE_POOL_STRICT=1
swiftc -O actor-latency-swift.swift && ./actor-latency-swift "$@"
