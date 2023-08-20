#!/bin/bash
swiftc -O actor-latency-swift.swift && ./actor-latency-swift "$@"
