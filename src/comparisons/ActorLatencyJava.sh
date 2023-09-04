#!/bin/sh
set -e
javac --release 20 --enable-preview ActorLatencyJava.java
java --enable-preview ActorLatencyJava "$@"
