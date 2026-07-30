// Tracepoint implementation. See tracepoint.h for what a tracepoint is and why
// it needs no handshake with the reader.

#define _GNU_SOURCE           // syscall(), clock_gettime, CLOCK_MONOTONIC

#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "tracepoint.h"
#include "helper.h"

// Cached slot index per tracepoint, so the scan happens once per thread rather
// than once per call. -1 = not claimed yet. A tid can sit at different indices
// in different tracepoints, hence one entry each.
static __thread int t_slots[TP_COUNT] = { [0 ... TP_COUNT - 1] = -1 };

// Seeding the second at bind time keeps the first call from seeing the whole
// uptime as elapsed and flushing an empty table.
void tp_bind(Tracepoint *tp, TidSlot *slots) {
    tp->slots = slots;
    atomic_store_explicit(&tp->sec_start_ns, now_ns(), memory_order_relaxed);
}

// Claim a slot for `tid`, or find the one we already own. Returns -1 if the
// table is full or every free slot was taken from under us.
static int claim_tid_slot(Tracepoint *tp, uint32_t tid) {
    for (int i = 0; i < PRESENT_MAX_TIDS; i++) {
        uint32_t owner = atomic_load_explicit(&tp->slots[i].tid,
                                              memory_order_acquire);
        if (owner == tid)
            return i;
        if (owner == 0) {
            uint32_t expected = 0;
            // Losing the CAS means another thread claimed this slot; keep
            // scanning instead of retrying, since the winner is not us.
            if (atomic_compare_exchange_strong_explicit(
                    &tp->slots[i].tid, &expected, tid,
                    memory_order_acq_rel, memory_order_acquire))
                return i;
        }
    }
    return -1;
}

static int tid_is_alive(uint32_t tid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%u", tid);
    return access(path, F_OK) == 0;
}

// Hand a slot back to the pool. `tid` is the occupancy flag, so it is stored
// last: clearing it first would let a new thread claim the slot and then have
// its max_per_sec wiped by the store that follows.
static void release_tid_slot(TidSlot *slot) {
    atomic_store_explicit(&slot->max_per_sec, 0, memory_order_relaxed);
    atomic_store_explicit(&slot->tid, 0, memory_order_release);
}

// Publish each slot's count for the second that just ended, then start the next
// one from zero. Only ever runs on one thread at a time per tracepoint.
//
// A slot that counted nothing all second is either idle or owned by a thread
// that has exited; only the latter should give up its slot, so ask /proc which
// it is. The check costs a stat, but only for silent slots.
static void flush_tid_counts(Tracepoint *tp) {
    for (int i = 0; i < PRESENT_MAX_TIDS; i++) {
        uint32_t n = atomic_exchange_explicit(&tp->counts[i], 0,
                                              memory_order_acq_rel);
        TidSlot *slot = &tp->slots[i];

        if (n == 0) {
            uint32_t tid = atomic_load_explicit(&slot->tid, memory_order_acquire);
            if (tid != 0 && !tid_is_alive(tid))
                release_tid_slot(slot);
            continue;
        }

        if (n > atomic_load_explicit(&slot->max_per_sec, memory_order_relaxed))
            atomic_store_explicit(&slot->max_per_sec, n, memory_order_release);
    }
}

void tp_record(Tracepoint *tp, uint64_t now) {
    int slot = t_slots[tp->id];
    if (slot < 0) {
        slot = claim_tid_slot(tp, (uint32_t)syscall(SYS_gettid));
        if (slot < 0)
            return;             // table full: this thread goes unreported
        t_slots[tp->id] = slot;
    }

    atomic_fetch_add_explicit(&tp->counts[slot], 1, memory_order_relaxed);

    if (now - atomic_load_explicit(&tp->sec_start_ns, memory_order_relaxed) < 1000000000ull)
        return;

    // Elapsed. One thread takes the flush; the rest carry on counting into the
    // next second, which costs at most one call's worth of accuracy.
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&tp->flushing, &expected, 1,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire))
        return;

    flush_tid_counts(tp);
    atomic_store_explicit(&tp->sec_start_ns, now, memory_order_release);
    atomic_store_explicit(&tp->flushing, 0, memory_order_release);
}
