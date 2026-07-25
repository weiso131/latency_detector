# latency_detector

*[中文版](./README.zh-TW.md)*

Measures game frametime / fps and hands the numbers to another process over
shared memory for further analysis (bpf integration comes later).

Two parts:

| Location | What it is |
|----------|------------|
| Project root (`src/`, `Cargo.toml`) | **Main feature: the reader (Rust).** mmaps the shared memory, issues a request, then parks on a futex waiting for the result — almost no CPU. |
| [`game_fps/`](./game_fps) | Vulkan layer (C). Hooks `vkQueuePresentKHR` to measure frametime; only on request does it measure a window and write fps/min/max into the POSIX shared memory `/dev/shm/game_fps`. |

The shared-memory path and struct layout live in
[`game_fps/fps_shm.h`](./game_fps/fps_shm.h), shared by both sides.
The two sides speak a **request/response protocol**: the reader asks for a
measurement of N seconds, and the layer replies with one result after measuring
a window that long. See
[`game_fps/README.md`](./game_fps/README.md#requestresponse-protocol) for details.

---

## Requirements

| Item | Notes |
|------|-------|
| Vulkan loader (runtime) | `libvulkan.so.1`; already needed to run any Vulkan program |
| Vulkan headers (to build the layer) | Provides `vulkan/vulkan.h` and `vulkan/vk_layer.h` |
| C compiler | `gcc` (`-std=gnu11`) |
| Rust toolchain | `cargo` (to build the reader) |
| Test program (optional) | `vkcube` (in `vulkan-tools` on Fedora) |

### Installing dependencies on Fedora

```bash
sudo dnf install -y vulkan-headers vulkan-loader-devel vulkan-tools
```

> Package names differ across distros (Debian/Ubuntu: `libvulkan-dev vulkan-tools`;
> Arch: `vulkan-headers vulkan-tools`). Install Rust via [rustup](https://rustup.rs).

---

## Build

```bash
# Reader (Rust, from the project root)
cargo build --release

# Layer (C)
cd game_fps && ./build.sh          # produces liblatency_layer.so
```

---

## Using it with Steam

Set this in the game's **Properties → General → Launch Options** (`%command%`
must stay at the end). Replace `/path/to/latency_creater` with the **absolute
path** to your clone — Steam does not accept relative paths:

```
VK_LAYER_PATH=/path/to/latency_creater/game_fps VK_LOADER_LAYERS_ENABLE=VK_LAYER_latency_creater %command%
```

- **Native Linux games**: works directly.
- **Proton (Windows games)**: same line. DXVK translates D3D to Vulkan, so the
  layer still intercepts the presents; keep the path under your home directory,
  which the Proton container can see by default.

To also get text output from the layer, add `LATENCY_VERBOSE` (it writes to a
file, not stdout):

```
VK_LAYER_PATH=/path/to/latency_creater/game_fps VK_LOADER_LAYERS_ENABLE=VK_LAYER_latency_creater LATENCY_VERBOSE=/tmp/latency.log %command%
```

> For running outside Steam (including `run.sh`), see
> [`game_fps/README.md`](./game_fps/README.md).

---

## Reading the fps (reader)

With the game running, start the reader in another terminal:

```bash
cargo run --release
```

The reader issues a request and prints one fps / min / max line once the layer
has measured a window. Seeing numbers means the whole chain works inside the
game. You can also run `ls -l /dev/shm/game_fps` to confirm the shared memory
was created.

Environment variables the reader understands:

| Variable | Default | Effect |
|----------|---------|--------|
| `LATENCY_SHM_NAME` | `/game_fps` | Shared-memory name (must match the layer's) |
| `LATENCY_WINDOW_SEC` | `1` | How many seconds each request measures. Must be > 0 (`0` means idle in the protocol). Larger is smoother but reports less often. |

---

## Troubleshooting

- **The reader sits there printing nothing**: expected. With no game presenting,
  nobody answers the request and the reader stays in `FUTEX_WAIT` (no CPU cost).
  Numbers appear once the game starts presenting. The first one takes at least
  `LATENCY_WINDOW_SEC` seconds.
- **Nothing happens at all / the layer never loads**: run with
  `VK_LOADER_DEBUG=all` and look for `Found manifest file .../latency_layer.json`.
  See the troubleshooting section in [`game_fps/README.md`](./game_fps/README.md).
