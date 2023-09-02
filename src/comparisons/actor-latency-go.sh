#!/bin/bash
set -e
go build actor-latency-go.go && ./actor-latency-go "$@"
