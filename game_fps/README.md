# game_fps — Vulkan Layer

*[中文版](./README.zh-TW.md)*

Hooks `vkQueuePresentKHR` to measure frametime. It measures a window only
**when the reader requests one**, and writes the result into the POSIX shared
memory `/dev/shm/game_fps`. The shared path and struct definition live in
[`fps_shm.h`](./fps_shm.h).

> The project overview, requirements, build steps, **using it with Steam**, and
> the reader are all in [`../README.md`](../README.md) one level up. This file
> only covers running things from this directory plus the environment variables.

---

## request/response protocol

`request_sec` in the shared memory is the **only synchronization point**, and it
doubles as the futex word for both sides. It carries the request and the
request's parameter in a single value:

| `request_sec` | Meaning |
|---------------|---------|
| `0` | Idle, or "result ready" |
| `N > 0` | The reader wants a window **N seconds** long |

The flow:

1. The reader stores the window length `N` into `request_sec`, `FUTEX_WAKE`s the
   layer, then `FUTEX_WAIT`s for the value to leave `N`.
2. On the next present the layer sees a nonzero value and opens a window, and
   starts accumulating frametimes.
3. After `N` seconds it writes fps/min/max/frame_count, stores `0` back into
   `request_sec` (store-release), and wakes the reader.
4. The layer will not touch this memory again until the reader issues a new
   request.

Because the layer only writes during the `N -> 0` transition and will not write
again until the reader re-arms, the reader owns the buffer exclusively while
reading it — **no seqlock needed**.

`0` is not a valid window length (it already means idle/ready), so the reader
rejects it. While no game is running the layer never clears `request_sec`, so
the reader simply stays parked in `FUTEX_WAIT` at almost no CPU cost.

The window length is chosen by the **reader**; see `LATENCY_WINDOW_SEC` in
[`../README.md`](../README.md).

---

## Running it directly (without Steam)

`run.sh` sets up the layer's environment variables and then runs the Vulkan
program you name:

```bash
./run.sh vkcube            # or any Vulkan program
```

Or set the variables yourself:

```bash
export VK_LAYER_PATH="$(pwd)"                              # where latency_layer.json is
export VK_LOADER_LAYERS_ENABLE="VK_LAYER_latency_creater"  # enable this layer
vkcube
```

---

## Environment variables

| Variable | Default | Effect |
|----------|---------|--------|
| `LATENCY_SHM_NAME` | `/game_fps` | Shared-memory name (maps to `/dev/shm/<name>`). The reader must use the same name. |
| `LATENCY_VERBOSE` | (unset) | **Text output only when set**: appends one line to this file path every time a window is measured. Unset means shm only. |

To follow the verbose output live:

```bash
export LATENCY_VERBOSE=/tmp/latency.log
./run.sh vkcube
tail -f /tmp/latency.log      # in another terminal
```

One line per window (appended, not stdout):

```
[latency] fps=59.9  min_frametime=4.506 ms  max_frametime=29.701 ms  (frames=60)
```

---

## Files

| File | Purpose |
|------|---------|
| `latency_layer.c` | The layer itself: hooks `vkQueuePresentKHR`, measures frametime, writes shm |
| `fps_shm.h` | Shared-memory path, struct definition, and the request/response protocol (shared by writer and reader) |
| `latency_layer.json` | Layer manifest, so the Vulkan loader can find the `.so` |
| `build.sh` | Build script |
| `run.sh` | Sets the environment variables and runs a Vulkan program |

---

## Troubleshooting

- **No verbose output at all**: check the program really presents (it opens a
  window and draws something), and that `LATENCY_VERBOSE` is set. You can add
  `VK_LOADER_DEBUG=all` to see whether the loader picked it up
  (`Found manifest file .../latency_layer.json`).
- **Crash / segfault**: make sure `liblatency_layer.so` is freshly built (re-run
  `./build.sh`).
- **`clock_gettime` compile error**: the source already starts with
  `#define _POSIX_C_SOURCE 200809L`; if you change the build flags yourself,
  keep `-std=gnu11` or add that macro.
