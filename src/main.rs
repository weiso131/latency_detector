// Reader side of the fps/frametime IPC buffer written by the Vulkan layer.
//
// Opens the POSIX shared memory (default /game_fps), mmaps it, and blocks on a
// FUTEX_WAIT on the seqlock's `seq` word. The writer does FUTEX_WAKE after each
// publish, so the reader wakes only on new data and stays parked at ~0 CPU
// while no game is running (no timeout -> no writer means no wakeups).
//
// The seqlock read still guards against a torn read; with 1s-aligned publishing
// a race is not expected, so it doubles as a data-loss hint if it ever trips.
//
// Layout mirrors game_fps/fps_shm.h -- both sides must agree on this struct.
//
// Env:
//   LATENCY_SHM_NAME   shm name (default "/game_fps")

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
}

fn env_str(key: &str, default: &str) -> String {
    std::env::var(key).unwrap_or_else(|_| default.to_string())
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
            }
        };
        fence(Ordering::Acquire);
        let s2 = shm.seq.load(Ordering::Acquire);
        if s1 == s2 {
            return (s1, snap);
        }
        // We read while the writer was mid-publish. With 1s-aligned publishing
        // this should never happen; if it does, flag it as a data-loss hint.
        eprintln!("fps_reader: warning: read raced the writer (retrying)");
    }
}

/// Block until seq differs from `expected`. No timeout: if no writer ever wakes
/// us (no game running), we stay asleep forever at ~0 CPU.
fn futex_wait(seq: &AtomicU32, expected: u32) {
    unsafe {
        libc::syscall(
            libc::SYS_futex,
            seq as *const AtomicU32,
            libc::FUTEX_WAIT,
            expected,
            std::ptr::null::<libc::timespec>(),
        );
    }
    // Ignore the return value: EAGAIN (value already changed) and spurious
    // wakeups both just mean "re-read the snapshot".
}

fn main() {
    let name = env_str("LATENCY_SHM_NAME", "/game_fps");

    let shm_ptr = match map_shm(&name) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("fps_reader: {e}");
            std::process::exit(1);
        }
    };
    let shm = unsafe { &*shm_ptr };

    eprintln!("fps_reader: reading {name} (futex, blocking)");

    // Wait-first: block until the writer publishes, then read. With no writer
    // we stay parked here forever -- no timeout, no spurious prints.
    loop {
        let seq = shm.seq.load(Ordering::Acquire);
        futex_wait(&shm.seq, seq);

        let (_, snap) = read_snapshot(shm);
        println!(
            "fps={:.1}  min={:.3} ms  max={:.3} ms  (frames={})",
            snap.fps, snap.min_frametime_ms, snap.max_frametime_ms, snap.frame_count
        );
    }
}
