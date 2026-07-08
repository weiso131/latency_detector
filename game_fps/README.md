# latency_creater

一個最小的 **Vulkan Layer**，攔截 `vkQueuePresentKHR` 量 frametime，
每 1 秒把 **fps / min frametime / max frametime** 用 seqlock 寫進一塊
POSIX 共享記憶體（`/dev/shm/game_fps`），供另一端（Rust）讀取。
共用的路徑與 struct 定義在 [`fps_shm.h`](./fps_shm.h)。

另外可選開啟 **verbose**，把每秒統計以文字輸出到指定檔案（見下方）：

```
[latency] fps=59.9  min_frametime=4.506 ms  max_frametime=29.701 ms  (frames=60)
```

共享 struct 與 seqlock 用法見 [`fps_shm.h`](./fps_shm.h)。

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

## 環境變數

| 變數 | 預設 | 作用 |
|------|------|------|
| `LATENCY_SHM_NAME` | `/game_fps` | 共享記憶體名稱（對應 `/dev/shm/<name>`）。讀端要用同一個名稱。 |
| `LATENCY_VERBOSE` | （未設） | **設了才輸出文字**：把每秒統計 append 到這個檔案路徑。沒設就只寫 shm、不輸出文字。 |

### verbose 怎麼用

預設**不會**有任何文字輸出（只寫共享記憶體）。想看每秒的 fps/frametime，
設 `LATENCY_VERBOSE` 指向一個檔案：

```bash
export VK_LAYER_PATH="$(pwd)"
export VK_LOADER_LAYERS_ENABLE="VK_LAYER_latency_creater"
export LATENCY_VERBOSE=/tmp/latency.log     # 輸出寫到這個檔
vkcube

# 另一個終端即時看：
tail -f /tmp/latency.log
```

輸出是 append（不覆蓋），每秒一行：

```
[latency] fps=59.9  min_frametime=4.506 ms  max_frametime=29.701 ms  (frames=60)
```

> 注意：verbose 輸出的是**檔案**，不是 stdout。因為 layer 跑在遊戲程序內，
> 從 Steam 之類的啟動器不好接 stdout，寫檔比較可靠。

---

## 檔案

| 檔案 | 作用 |
|------|------|
| `latency_layer.c` | Layer 本體，攔截 `vkQueuePresentKHR` 量 frametime，寫 shm |
| `fps_shm.h` | 共享記憶體路徑與 struct 定義（寫端 / 讀端共用） |
| `latency_layer.json` | Layer manifest，讓 Vulkan loader 找到 `.so` |
| `build.sh` | 編譯腳本 |
| `run.sh` | 設好環境變數並執行 Vulkan 程式 |

---

## 疑難排解

- **沒有任何 `[latency]` 輸出**：確認程式真的有 present（會開視窗畫東西）。
  可加 `VK_LOADER_DEBUG=all` 看 loader 有沒有載入
  `Found manifest file .../latency_layer.json`。
- **程式崩潰 / segfault**：確認 `liblatency_layer.so` 是最新編出來的（重跑 `./build.sh`）。
- **`clock_gettime` 編譯錯誤**：原始碼開頭已 `#define _POSIX_C_SOURCE 200809L`；
  若自行改 build flag 請保留 `-std=gnu11` 或加該 macro。
