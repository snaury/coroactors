#!/bin/bash
set -e
go build scheduler-throughput-go.go && ./scheduler-throughput-go "$@"
