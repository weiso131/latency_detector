# latency_creater

量遊戲 frametime / fps，透過共享記憶體傳給另一端做後續分析（之後配 bpf）。

兩個部分：

| 位置 | 是什麼 |
|------|--------|
| 專案根目錄（`src/`, `Cargo.toml`） | **主功能：讀取端（Rust）**。mmap 共享記憶體，輪詢讀最新快照，兩次輪詢間睡覺，CPU 幾乎不佔。 |
| [`game_fps/`](./game_fps) | Vulkan Layer（C）。攔 `vkQueuePresentKHR` 量 frametime，每秒用 seqlock 把 fps/min/max 寫進 POSIX 共享記憶體 `/dev/shm/game_fps`。 |

共享記憶體的路徑與 struct layout 定義在 [`game_fps/fps_shm.h`](./game_fps/fps_shm.h)，兩端共用。

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

想同時看每秒文字輸出，前面再加 `LATENCY_VERBOSE`（寫到檔案，不是 stdout）：

```
VK_LAYER_PATH=/path/to/latency_creater/game_fps VK_LOADER_LAYERS_ENABLE=VK_LAYER_latency_creater LATENCY_VERBOSE=/tmp/latency.log %command%
```

> 非 Steam 的一般執行方式（含 `run.sh`）見 [`game_fps/README.md`](./game_fps/README.md)。

---

## 讀取 fps（reader）

遊戲跑起來後，另開一個終端跑 reader：

```bash
cargo run --release
```

會每秒印出目前的 fps / min / max。有印出數字就代表整條鏈在遊戲裡通了。
也可用 `watch -n1 ls -l /dev/shm/game_fps` 確認共享記憶體有被建立、更新。

reader 支援的環境變數：

| 變數 | 預設 | 作用 |
|------|------|------|
| `LATENCY_SHM_NAME` | `/game_fps` | 共享記憶體名稱（要跟 layer 端一致） |
| `READER_POLL_MS` | `500` | 輪詢間隔（ms）。越大越省 CPU、越不即時。 |
| `READER_STALE_MS` | `2000` | 資料超過這麼久沒更新就當作「沒有寫端」 |

---

## 疑難排解

- **reader 印 `waiting for writer...`**：shm 已建但 layer 還沒寫第一筆；遊戲開始 present 後就會有數字。
- **reader 印 `stale (no writer)`**：遊戲關了 / 沒在 present。共享記憶體會留住最後一筆值。
- **完全沒反應 / layer 沒載入**：加 `VK_LOADER_DEBUG=all` 跑，看有沒有
  `Found manifest file .../latency_layer.json`。詳見 [`game_fps/README.md`](./game_fps/README.md) 的疑難排解。
