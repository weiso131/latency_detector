// Shared layout for the fps/frametime IPC buffer.
// Writer: the Vulkan layer. Reader: separate process (e.g. Rust).
// Both sides must agree on this struct and the shm name.
#ifndef FPS_SHM_H
#define FPS_SHM_H

#include <stdint.h>
#include <stdatomic.h>

#define FPS_SHM_DEFAULT_NAME "/game_fps"   // /dev/shm/game_fps

// Reader-driven, request/response handshake. `request_sec` is the only sync
// point and doubles as the futex word for both sides. It carries the request
// and its parameter at once: any nonzero value means "measure a window this
// many seconds long", and zero means idle / result ready.
//
//   request_sec = N : reader asked for an N-second sample. It stores N,
//                     FUTEX_WAKEs the layer, then FUTEX_WAITs for the word to
//                     leave N.
//   layer           : on seeing nonzero it latches N, measures a window of that
//                     length, writes the fields, stores 0, wakes the reader.
//   request_sec = 0 : idle, or "result ready". The layer will not write again
//                     until the reader stores a nonzero value once more.
//
// Because the layer only writes during a nonzero->0 transition and cannot write
// again until the reader re-arms, the reader owns the data while reading it --
// no seqlock needed.
typedef struct {
    _Atomic uint32_t request_sec;  // >0 = window length in seconds, 0 = idle / ready
    uint32_t _pad;
    double   fps;
    double   min_frametime_ms;
    double   max_frametime_ms;
    uint64_t frame_count;      // frames in the measured window
} FpsShm;

#endif // FPS_SHM_H
