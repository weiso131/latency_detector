# latency_creater

一個最小的 **Vulkan Layer**，攔截 `vkQueuePresentKHR` 量 frametime，
每 1 秒對 stdout 印出 **fps / min frametime / max frametime**。

輸出範例（vkcube，vsync 下鎖 ~60fps）：

```
[latency] fps=59.9  min_frametime=4.506 ms  max_frametime=29.701 ms  (frames=60)
```

原理與 MangoHud 研究筆記見 [`TODO.md`](./TODO.md)。

---

## 環境需求

| 項目 | 說明 |
|------|------|
| Vulkan Loader (runtime) | `libvulkan.so.1`，跑 Vulkan 程式本來就需要 |
| Vulkan headers（build 用） | 提供 `vulkan/vulkan.h`、`vulkan/vk_layer.h` |
| C 編譯器 | `gcc`（build.sh 用 `-std=gnu11`） |
| 測試程式（選用） | `vkcube`（Fedora 在 `vulkan-tools` 套件） |

### Fedora 安裝相依套件

```bash
sudo dnf install -y vulkan-headers vulkan-loader-devel vulkan-tools
```

> 其他發行版套件名可能不同（Debian/Ubuntu：`libvulkan-dev vulkan-tools`；
> Arch：`vulkan-headers vulkan-tools`）。

---

## Build

```bash
./build.sh
```

會用一行 `gcc` 編出 `liblatency_layer.so`：

```bash
gcc -std=gnu11 -O2 -fPIC -shared -o liblatency_layer.so latency_layer.c
```

---

## 執行

`run.sh` 會設好環境變數（讓 loader 從本資料夾找到 layer 並啟用），
然後執行你指定的 Vulkan 程式：

```bash
./run.sh vkcube            # 或任何 Vulkan 遊戲 / 程式
```

或手動設環境變數：

```bash
export VK_LAYER_PATH="$(pwd)"                          # 找 latency_layer.json
export VK_LOADER_LAYERS_ENABLE="VK_LAYER_latency_creater"   # 啟用本 layer
vkcube
```

---

## 檔案

| 檔案 | 作用 |
|------|------|
| `latency_layer.c` | Layer 本體，攔截 `vkQueuePresentKHR` 量 frametime |
| `latency_layer.json` | Layer manifest，讓 Vulkan loader 找到 `.so` |
| `build.sh` | 編譯腳本 |
| `run.sh` | 設好環境變數並執行 Vulkan 程式 |
| `TODO.md` | MangoHud 機制研究筆記 + 實作踩坑 / 取捨 |

---

## 疑難排解

- **沒有任何 `[latency]` 輸出**：確認程式真的有 present（會開視窗畫東西）。
  可加 `VK_LOADER_DEBUG=all` 看 loader 有沒有載入
  `Found manifest file .../latency_layer.json`。
- **程式崩潰 / segfault**：確認 `liblatency_layer.so` 是最新編出來的（重跑 `./build.sh`）。
- **`clock_gettime` 編譯錯誤**：原始碼開頭已 `#define _POSIX_C_SOURCE 200809L`；
  若自行改 build flag 請保留 `-std=gnu11` 或加該 macro。
