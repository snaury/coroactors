#!/bin/bash
set -e
swiftc -O async-overhead-swift.swift
./async-overhead-swift
