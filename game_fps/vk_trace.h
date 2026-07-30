// Shared layout for the Vulkan tracepoint buffer: which threads call each
// intercepted entry point, and how hard.
//
// Deliberately a separate shm object from the fps buffer in fps_shm.h. The two
// answer different questions and share no synchronisation: fps is a
// reader-driven request/response sample, while these tables are maintained on
// every call and are always valid to read. Consumers that only want thread
// activity (scx_teddy) map this and nothing else.
//
// Writer: the Vulkan layer. Readers: any number of processes.
#ifndef VK_TRACE_H
#define VK_TRACE_H

#include <stdint.h>
#include <stdatomic.h>

#define VK_TRACE_DEFAULT_NAME "/vk_trace"   // /dev/shm/vk_trace

// Shm is one flat region, so the thread tables are fixed arrays rather than
// hashmaps. Threads that actually present are few (usually one render thread),
// so a handful of slots covers it. Submits spread wider -- worker threads may
// each submit their own command buffers -- so that table gets more room.
#define PRESENT_MAX_TIDS 4
#define SUBMIT_MAX_TIDS  32

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
    TidSlot present_tids[PRESENT_MAX_TIDS];
    // vkQueueSubmit and vkQueueSubmit2 share this table: both submit work to a
    // queue, and which of the two an app uses is a Vulkan version detail, not a
    // difference worth splitting the counts over.
    TidSlot submit_tids[SUBMIT_MAX_TIDS];
} VkTraceShm;

#endif // VK_TRACE_H
