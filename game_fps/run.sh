#!/usr/bin/env bash
# 用我們的 layer 跑一個 Vulkan 程式。
# 用法：./run.sh <vulkan程式> [參數...]
#   例：./run.sh vkcube
#       ./run.sh vkcube --c 300     # 跑 300 frame 後結束
set -e
cd "$(dirname "$0")"

if [ $# -lt 1 ]; then
    echo "用法: $0 <vulkan程式> [參數...]"
    echo "例:   $0 vkcube"
    exit 1
fi

# 讓 loader 從當前資料夾找到 manifest；用 layer name 明確啟用
export VK_LAYER_PATH="$(pwd)"
export VK_INSTANCE_LAYERS="VK_LAYER_latency_creater"
# 新版 loader 用這個環境變數（兩個都設保險）
export VK_LOADER_LAYERS_ENABLE="VK_LAYER_latency_creater"

exec "$@"
