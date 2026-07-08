// Shared layout for the fps/frametime IPC buffer.
// Writer: the Vulkan layer. Reader: separate process (e.g. Rust).
// Both sides must agree on this struct and the shm name.
#ifndef FPS_SHM_H
#define FPS_SHM_H

#include <stdint.h>
#include <stdatomic.h>

#define FPS_SHM_DEFAULT_NAME "/game_fps"   // /dev/shm/game_fps

// seqlock: writer does seq++ (odd) -> write fields -> seq++ (even).
// reader reads seq, copies fields, re-reads seq; retry if odd or changed.
typedef struct {
    _Atomic uint32_t seq;      // odd = write in progress, even = stable
    uint32_t _pad;
    double   fps;
    double   min_frametime_ms;
    double   max_frametime_ms;
    uint64_t frame_count;      // frames in the reported window
    uint64_t update_ns;        // CLOCK_MONOTONIC timestamp of last write
} FpsShm;

#endif // FPS_SHM_H
