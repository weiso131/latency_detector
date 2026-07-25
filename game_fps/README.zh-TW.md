# game_fps — Vulkan Layer

*[English](./README.md)*

攔截 `vkQueuePresentKHR` 量 frametime，**由讀取端發出請求**才量一段窗口，
結果寫進 POSIX 共享記憶體 `/dev/shm/game_fps`。
共享路徑與 struct 定義見 [`fps_shm.h`](./fps_shm.h)。

> 專案總覽、環境需求、build、**在 Steam 使用**、reader 都在
> 上層的 [`../README.zh-TW.md`](../README.zh-TW.md)。這份只講本目錄怎麼直接跑 + 環境變數。

---

## request/response 協議

共享記憶體裡的 `request_sec` 是**唯一的同步點**，同時也是兩端的 futex word。
它把「請求」和「請求的參數」合在同一個值裡：

| `request_sec` | 意義 |
|---------------|------|
| `0` | 閒置，或「結果已就緒」 |
| `N > 0` | 讀取端要求量一段 **N 秒**的窗口 |

流程：

1. 讀取端把窗口長度 `N` 寫進 `request_sec`，`FUTEX_WAKE` 叫醒 layer，
   然後 `FUTEX_WAIT` 等這個值離開 `N`。
2. layer 在下一次 present 看到非 0，就開一個窗口開始累積 frametime。
3. 累積滿 `N` 秒後寫入 fps/min/max/frame_count，把 `request_sec` 設回 `0`
   （store-release），再叫醒讀取端。
4. 在讀取端重新發出請求之前，layer 不會再碰這塊記憶體。

因為 layer 只在 `N -> 0` 這個轉換寫入，且在讀取端重新發請求前不會再寫，
讀取端在讀資料時獨佔這塊 buffer —— **不需要 seqlock**。

`0` 不是合法的窗口長度（它已經被拿來表示閒置/就緒），讀取端會拒絕 `0`。
沒有遊戲在跑時 layer 不會清掉 `request_sec`，讀取端就一直停在 `FUTEX_WAIT`，
CPU 幾乎不佔。

窗口長度由**讀取端**決定，見 [`../README.zh-TW.md`](../README.zh-TW.md) 的 `LATENCY_WINDOW_SEC`。

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
| `LATENCY_VERBOSE` | （未設） | **設了才輸出文字**：每量完一個窗口就 append 一行到這個檔案路徑。沒設就只寫 shm。 |

verbose 想即時看：

```bash
export LATENCY_VERBOSE=/tmp/latency.log
./run.sh vkcube
tail -f /tmp/latency.log      # 另一個終端
```

每個窗口輸出一行（append，不是 stdout）：

```
[latency] fps=59.9  min_frametime=4.506 ms  max_frametime=29.701 ms  (frames=60)
```

---

## 檔案

| 檔案 | 作用 |
|------|------|
| `latency_layer.c` | Layer 本體，攔截 `vkQueuePresentKHR` 量 frametime，寫 shm |
| `fps_shm.h` | 共享記憶體路徑、struct 定義與 request/response 協議（寫端 / 讀端共用） |
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
