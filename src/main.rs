// Reader side of the fps/frametime IPC buffer written by the Vulkan layer.
//
// Opens the POSIX shared memory (default /game_fps), mmaps it, and blocks on a
// FUTEX_WAIT on the seqlock's `seq` word. The writer does FUTEX_WAKE after each
// publish, so the reader consumes ~0 CPU while idle and wakes on new data. The
// wait has a timeout so we can still detect a writer that stopped (stale).
//
// Layout mirrors game_fps/fps_shm.h -- both sides must agree on this struct.
//
// Env:
//   LATENCY_SHM_NAME   shm name (default "/game_fps")
//   READER_STALE_MS    treat data older than this as no-writer, and the futex
//                      wait timeout (default 2000)

use std::ffi::CString;
use std::sync::atomic::{AtomicU32, Ordering, fence};

// Must match FpsShm in game_fps/fps_shm.h.
#[repr(C)]
struct FpsShm {
    seq: AtomicU32,
    _pad: u32,
    fps: f64,
    min_frametime_ms: f64,
    max_frametime_ms: f64,
    frame_count: u64,
    update_ns: u64,
}

#[derive(Debug)]
struct Snapshot {
    fps: f64,
    min_frametime_ms: f64,
    max_frametime_ms: f64,
    frame_count: u64,
    update_ns: u64,
}

fn env_str(key: &str, default: &str) -> String {
    std::env::var(key).unwrap_or_else(|_| default.to_string())
}

fn env_u64(key: &str, default: u64) -> u64 {
    std::env::var(key).ok().and_then(|v| v.parse().ok()).unwrap_or(default)
}

fn now_ns() -> u64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}

/// Map the shm read-only. Returns a pointer to the shared struct.
/// The mapping stays valid for the process lifetime (never unmapped here).
fn map_shm(name: &str) -> Result<*const FpsShm, String> {
    let cname = CString::new(name).map_err(|e| e.to_string())?;
    let fd = unsafe { libc::shm_open(cname.as_ptr(), libc::O_RDONLY, 0) };
    if fd < 0 {
        return Err(format!(
            "shm_open({name}) failed: {} (writer not started yet?)",
            std::io::Error::last_os_error()
        ));
    }
    let size = std::mem::size_of::<FpsShm>();
    let p = unsafe {
        libc::mmap(std::ptr::null_mut(), size, libc::PROT_READ, libc::MAP_SHARED, fd, 0)
    };
    unsafe { libc::close(fd) };
    if p == libc::MAP_FAILED {
        return Err(format!("mmap failed: {}", std::io::Error::last_os_error()));
    }
    Ok(p as *const FpsShm)
}

/// seqlock read: retry until stable. Returns the seq value the snapshot was
/// taken at (even), so the caller can FUTEX_WAIT for it to change.
fn read_snapshot(shm: &FpsShm) -> (u32, Snapshot) {
    loop {
        let s1 = shm.seq.load(Ordering::Acquire);
        if s1 & 1 != 0 {
            std::hint::spin_loop();
            continue;
        }
        // Volatile reads so the compiler can't hoist them past the seq checks.
        let snap = unsafe {
            Snapshot {
                fps: std::ptr::read_volatile(&shm.fps),
                min_frametime_ms: std::ptr::read_volatile(&shm.min_frametime_ms),
                max_frametime_ms: std::ptr::read_volatile(&shm.max_frametime_ms),
                frame_count: std::ptr::read_volatile(&shm.frame_count),
                update_ns: std::ptr::read_volatile(&shm.update_ns),
            }
        };
        fence(Ordering::Acquire);
        let s2 = shm.seq.load(Ordering::Acquire);
        if s1 == s2 {
            return (s1, snap);
        }
    }
}

/// Block until seq differs from `expected`, or the timeout elapses.
/// Returns immediately if it already differs.
fn futex_wait(seq: &AtomicU32, expected: u32, timeout_ms: u64) {
    let ts = libc::timespec {
        tv_sec: (timeout_ms / 1000) as libc::time_t,
        tv_nsec: ((timeout_ms % 1000) * 1_000_000) as libc::c_long,
    };
    unsafe {
        libc::syscall(
            libc::SYS_futex,
            seq as *const AtomicU32,
            libc::FUTEX_WAIT,
            expected,
            &ts as *const libc::timespec,
        );
    }
    // Ignore the return value: EAGAIN (value already changed), ETIMEDOUT, and
    // spurious wakeups all just mean "re-read the snapshot".
}

fn main() {
    let name = env_str("LATENCY_SHM_NAME", "/game_fps");
    let stale_ms = env_u64("READER_STALE_MS", 2000);
    let stale_ns = stale_ms * 1_000_000;

    let shm_ptr = match map_shm(&name) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("fps_reader: {e}");
            std::process::exit(1);
        }
    };
    let shm = unsafe { &*shm_ptr };

    eprintln!("fps_reader: reading {name} (futex, stale={stale_ms}ms)");

    let mut last_update = 0u64;
    loop {
        let (seq, snap) = read_snapshot(shm);

        if snap.update_ns == 0 {
            // never written yet
            println!("waiting for writer...");
        } else if now_ns().saturating_sub(snap.update_ns) > stale_ns {
            println!("stale (no writer): last fps={:.1}", snap.fps);
        } else if snap.update_ns != last_update {
            // only print on a fresh update
            println!(
                "fps={:.1}  min={:.3} ms  max={:.3} ms  (frames={})",
                snap.fps, snap.min_frametime_ms, snap.max_frametime_ms, snap.frame_count
            );
            last_update = snap.update_ns;
        }

        // Sleep until the writer publishes (seq changes) or the stale timeout
        // fires, so we can still report a writer that went away.
        futex_wait(&shm.seq, seq, stale_ms);
    }
}
