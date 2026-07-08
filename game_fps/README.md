# game_fps — Vulkan Layer

攔截 `vkQueuePresentKHR` 量 frametime，每秒用 seqlock 把 fps/min/max 寫進
POSIX 共享記憶體 `/dev/shm/game_fps`。共享路徑與 struct 定義見 [`fps_shm.h`](./fps_shm.h)。

> 專案總覽、環境需求、build、**在 Steam 使用**、reader 都在
> 上層的 [`../README.md`](../README.md)。這份只講本目錄怎麼直接跑 + 環境變數。

---

## 直接執行（非 Steam）

`run.sh` 會設好 layer 相關環境變數，然後執行你指定的 Vulkan 程式：

```bash
./run.sh vkcube            # 或任何 Vulkan 程式
```

或手動設環境變數：

```bash
export VK_LAYER_PATH="$(pwd)"                              # 找 latency_layer.json
export VK_LOADER_LAYERS_ENABLE="VK_LAYER_latency_creater"  # 啟用本 layer
vkcube
```

---

## 環境變數

| 變數 | 預設 | 作用 |
|------|------|------|
| `LATENCY_SHM_NAME` | `/game_fps` | 共享記憶體名稱（對應 `/dev/shm/<name>`）。讀端要用同一個名稱。 |
| `LATENCY_VERBOSE` | （未設） | **設了才輸出文字**：把每秒統計 append 到這個檔案路徑。沒設就只寫 shm。 |

verbose 想即時看：

```bash
export LATENCY_VERBOSE=/tmp/latency.log
./run.sh vkcube
tail -f /tmp/latency.log      # 另一個終端
```

輸出每秒一行（append，不是 stdout）：

```
[latency] fps=59.9  min_frametime=4.506 ms  max_frametime=29.701 ms  (frames=60)
```

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

- **沒有任何 verbose 輸出**：確認程式真的有 present（會開視窗畫東西）；並確認有設 `LATENCY_VERBOSE`。
  可加 `VK_LOADER_DEBUG=all` 看 loader 有沒有載入 `Found manifest file .../latency_layer.json`。
- **程式崩潰 / segfault**：確認 `liblatency_layer.so` 是最新編出來的（重跑 `./build.sh`）。
- **`clock_gettime` 編譯錯誤**：原始碼開頭已 `#define _POSIX_C_SOURCE 200809L`；
  若自行改 build flag 請保留 `-std=gnu11` 或加該 macro。
