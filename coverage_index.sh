#!/bin/bash
set -e
xcrun llvm-profdata merge -sparse default.profraw -o default.profdata
