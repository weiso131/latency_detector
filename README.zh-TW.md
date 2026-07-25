# latency_detector

*[English](./README.md)*

量遊戲 frametime / fps，透過共享記憶體傳給另一端做後續分析（之後配 bpf）。

兩個部分：

| 位置 | 是什麼 |
|------|--------|
| 專案根目錄（`src/`, `Cargo.toml`） | **主功能：讀取端（Rust）**。mmap 共享記憶體，發出請求後停在 futex 上等結果，CPU 幾乎不佔。 |
| [`game_fps/`](./game_fps) | Vulkan Layer（C）。攔 `vkQueuePresentKHR` 量 frametime，收到請求才量一段窗口，把 fps/min/max 寫進 POSIX 共享記憶體 `/dev/shm/game_fps`。 |

共享記憶體的路徑與 struct layout 定義在 [`game_fps/fps_shm.h`](./game_fps/fps_shm.h)，兩端共用。
兩端走 **request/response 協議**：讀取端要求量幾秒，layer 量完一段窗口才回一筆結果。
詳見 [`game_fps/README.zh-TW.md`](./game_fps/README.zh-TW.md#requestresponse-協議)。

---

## 環境需求

| 項目 | 說明 |
|------|------|
| Vulkan Loader (runtime) | `libvulkan.so.1`，跑 Vulkan 程式本來就需要 |
| Vulkan headers（build layer 用） | 提供 `vulkan/vulkan.h`、`vulkan/vk_layer.h` |
| C 編譯器 | `gcc`（`-std=gnu11`） |
| Rust toolchain | `cargo`（build reader 用） |
| 測試程式（選用） | `vkcube`（Fedora 在 `vulkan-tools`） |

### Fedora 安裝相依套件

```bash
sudo dnf install -y vulkan-headers vulkan-loader-devel vulkan-tools
```

> 其他發行版套件名可能不同（Debian/Ubuntu：`libvulkan-dev vulkan-tools`；
> Arch：`vulkan-headers vulkan-tools`）。Rust 用 [rustup](https://rustup.rs) 裝。

---

## Build

```bash
# Reader（Rust，專案根目錄）
cargo build --release

# Layer（C）
cd game_fps && ./build.sh          # 產出 liblatency_layer.so
```

---

## 在 Steam 使用

在遊戲的 **內容 → 一般 → 啟動選項** 填入（`%command%` 一定要留在最後）。
把 `/path/to/latency_creater` 換成你 clone 專案的**絕對路徑**（Steam 不吃相對路徑）：

```
VK_LAYER_PATH=/path/to/latency_creater/game_fps VK_LOADER_LAYERS_ENABLE=VK_LAYER_latency_creater %command%
```

- **原生 Linux 遊戲**：直接生效。
- **Proton（Windows 遊戲）**：一樣這樣填。DXVK 把 D3D 轉 Vulkan，present 時 layer 仍攔得到；
  路徑放在 home 目錄下，Proton 容器預設看得到。

想同時看 layer 端的文字輸出，前面再加 `LATENCY_VERBOSE`（寫到檔案，不是 stdout）：

```
VK_LAYER_PATH=/path/to/latency_creater/game_fps VK_LOADER_LAYERS_ENABLE=VK_LAYER_latency_creater LATENCY_VERBOSE=/tmp/latency.log %command%
```

> 非 Steam 的一般執行方式（含 `run.sh`）見 [`game_fps/README.zh-TW.md`](./game_fps/README.zh-TW.md)。

---

## 讀取 fps（reader）

遊戲跑起來後，另開一個終端跑 reader：

```bash
cargo run --release
```

reader 會發出請求，等 layer 量完一段窗口後印出一行 fps / min / max。
有印出數字就代表整條鏈在遊戲裡通了。
也可用 `ls -l /dev/shm/game_fps` 確認共享記憶體有被建立。

reader 支援的環境變數：

| 變數 | 預設 | 作用 |
|------|------|------|
| `LATENCY_SHM_NAME` | `/game_fps` | 共享記憶體名稱（要跟 layer 端一致） |
| `LATENCY_WINDOW_SEC` | `1` | 每次請求要量幾秒。必須 > 0（`0` 在協議裡代表閒置）。越大越平滑、回報越慢。 |

---

## 疑難排解

- **reader 卡住不動、沒印出東西**：正常。沒有遊戲在 present 時沒人回應請求，
  reader 就停在 `FUTEX_WAIT`（不佔 CPU）。遊戲開始 present 後就會有數字。
  第一筆最少要等 `LATENCY_WINDOW_SEC` 秒。
- **完全沒反應 / layer 沒載入**：加 `VK_LOADER_DEBUG=all` 跑，看有沒有
  `Found manifest file .../latency_layer.json`。詳見 [`game_fps/README.zh-TW.md`](./game_fps/README.zh-TW.md) 的疑難排解。
