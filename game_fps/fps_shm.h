// Shared layout for the fps/frametime IPC buffer.
// Writer: the Vulkan layer. Reader: separate process (e.g. Rust).
// Both sides must agree on this struct and the shm name.
#ifndef FPS_SHM_H
#define FPS_SHM_H

#include <stdint.h>
#include <stdatomic.h>

#define FPS_SHM_DEFAULT_NAME "/game_fps"   // /dev/shm/game_fps

// Shm is one flat region, so the thread tables are fixed arrays rather than
// hashmaps. Threads that actually present are few (usually one render thread),
// so a handful of slots covers it. Submits spread wider -- worker threads may
// each submit their own command buffers -- so that table gets more room.
#define PRESENT_MAX_TIDS 4
#define SUBMIT_MAX_TIDS  32

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
// Which threads call an intercepted entry point, and how hard. Independent of
// the fps handshake above: these tables are maintained on every call, whether
// or not a measurement window is open, and share no synchronisation with it.
//
// No handshake is needed because every field is atomic and the tables are not
// time-sensitive -- the set of threads calling a given entry point does not
// change over the life of a process. `tid` doubles as the slot's occupancy
// flag: 0 means free, and a thread claims a slot with a CAS from 0 to its own
// tid. A losing CAS just skips this call; the thread tries again on the next.
//
// `max_per_sec` only ever grows while a slot is held, so a reader can load it
// at any time and get a valid high-water mark. The second boundaries it is
// derived from are tracked on the writer side, not here.
typedef struct {
    _Atomic uint32_t tid;          // 0 = free slot, else the calling kernel tid
    _Atomic uint32_t max_per_sec;  // high-water mark of calls in any one second
} TidSlot;

typedef struct {
    _Atomic uint32_t request_sec;  // >0 = window length in seconds, 0 = idle / ready
    uint32_t _pad;
    double   fps;
    double   min_frametime_ms;
    double   max_frametime_ms;
    uint64_t frame_count;      // frames in the measured window

    TidSlot  present_tids[PRESENT_MAX_TIDS];
    // vkQueueSubmit and vkQueueSubmit2 share this table: both submit work to a
    // queue, and which of the two an app uses is a Vulkan version detail, not a
    // difference worth splitting the counts over.
    TidSlot  submit_tids[SUBMIT_MAX_TIDS];
} FpsShm;

#endif // FPS_SHM_H
