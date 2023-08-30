#!/bin/bash
set -e
xcrun llvm-cov show --show-line-counts-or-regions --show-branches=count ./a.out -instr-profile=default.profdata
