// Shared layout for the fps/frametime IPC buffer.
// Writer: the Vulkan layer. Reader: separate process (e.g. Rust).
// Both sides must agree on this struct and the shm name.
#ifndef FPS_SHM_H
#define FPS_SHM_H

#include <stdint.h>
#include <stdatomic.h>

#define FPS_SHM_DEFAULT_NAME "/game_fps"   // /dev/shm/game_fps

// Reader-driven, request/response handshake. `request` is the only sync point
// and doubles as the futex word for both sides:
//
//   request = 1 : reader asked for a sample. It sets 1, FUTEX_WAKEs the layer,
//                 then FUTEX_WAITs for request to leave 1.
//   layer       : parked on FUTEX_WAIT(request==0). On wake it measures one
//                 window, writes the fields, sets request = 0, wakes the reader.
//   request = 0 : idle, or "result ready". The layer will not write again until
//                 the reader sets request = 1 once more.
//
// Because the layer only writes during a 1->0 transition and cannot write again
// until the reader re-arms, the reader owns the data while reading it -- no
// seqlock needed.
typedef struct {
    _Atomic uint32_t request;  // 1 = sample requested, 0 = idle / result ready
    uint32_t _pad;
    double   fps;
    double   min_frametime_ms;
    double   max_frametime_ms;
    uint64_t frame_count;      // frames in the measured window
} FpsShm;

#endif // FPS_SHM_H
