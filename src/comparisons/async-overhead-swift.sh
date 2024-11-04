#!/bin/bash
set -e
swiftc -swift-version 6 -O async-overhead-swift.swift
./async-overhead-swift
