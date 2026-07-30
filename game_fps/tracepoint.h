// Tracepoints: per-thread call tables. A tracepoint answers "which tids call
// this entry point, and how often". It is maintained on every call and shares
// no synchronisation with the fps handshake in fps_shm.h.
//
// Each tracepoint pairs an in-process counter array with a shm slot array,
// one-to-one: entry i counts calls from the tid published in slots[i]. The
// second boundary is per tracepoint, so each one's counts cover its own
// interval; whichever thread first notices that second has elapsed flushes
// every slot of that tracepoint.
#ifndef TRACEPOINT_H
#define TRACEPOINT_H

#include <stdint.h>
#include <stdatomic.h>

#include "fps_shm.h"

// Fixed id per tracepoint; indexes the per-thread slot cache. Every tracepoint
// needs its own, and TP_COUNT sizes the cache.
enum { TP_PRESENT, TP_COUNT };

typedef struct {
    int              id;              // TP_*, indexes the per-thread slot cache
    TidSlot         *slots;           // the shm array this publishes to
    _Atomic uint32_t counts[PRESENT_MAX_TIDS];  // calls in the current second
    _Atomic uint64_t sec_start_ns;    // start of the second being counted
    _Atomic int      flushing;        // 1 while a thread is flushing
} Tracepoint;

// Point a tracepoint at its shm array and start its second. Call once the shm
// mapping exists -- the array has no address before that.
void tp_bind(Tracepoint *tp, TidSlot *slots);

// Count one call against the calling thread. `now` is a now_ns() timestamp;
// callers that already have one pass it rather than taking the clock twice.
void tp_record(Tracepoint *tp, uint64_t now);

#endif // TRACEPOINT_H
