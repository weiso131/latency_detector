// Reader side of the fps/frametime IPC buffer written by the Vulkan layer.
//
// Opens the POSIX shared memory (default /game_fps), mmaps it, and polls the
// seqlock for the latest snapshot. Sleeps between polls to keep CPU near zero.
//
// Layout mirrors game_fps/fps_shm.h -- both sides must agree on this struct.
//
// Env:
//   LATENCY_SHM_NAME   shm name (default "/game_fps")
//   READER_POLL_MS     poll interval in ms (default 500)
//   READER_STALE_MS    treat data older than this as no-writer (default 2000)

use std::ffi::CString;
use std::sync::atomic::{AtomicU32, Ordering, fence};
use std::thread::sleep;
use std::time::Duration;

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

/// seqlock read: retry until we get a stable (even, unchanged) snapshot.
fn read_snapshot(shm: &FpsShm) -> Snapshot {
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
            return snap;
        }
    }
}

fn main() {
    let name = env_str("LATENCY_SHM_NAME", "/game_fps");
    let poll = Duration::from_millis(env_u64("READER_POLL_MS", 500));
    let stale_ns = env_u64("READER_STALE_MS", 2000) * 1_000_000;

    let shm_ptr = match map_shm(&name) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("fps_reader: {e}");
            std::process::exit(1);
        }
    };
    let shm = unsafe { &*shm_ptr };

    eprintln!("fps_reader: reading {name}, poll={}ms", poll.as_millis());

    let mut last_update = 0u64;
    loop {
        let snap = read_snapshot(shm);

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

        sleep(poll);
    }
}
