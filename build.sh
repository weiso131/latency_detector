#!/usr/bin/env bash
# 編譯最小 Vulkan layer
set -e
cd "$(dirname "$0")"

gcc -std=gnu11 -O2 -fPIC -shared \
    -o liblatency_layer.so \
    latency_layer.c

echo "built: $(pwd)/liblatency_layer.so"
