// Reader side of the fps/frametime IPC buffer written by the Vulkan layer.
//
// Reader-driven: we set `request = 1`, wake the layer, and block on
// FUTEX_WAIT(request == 1). The layer measures one window, writes the fields,
// sets request = 0, and wakes us. Because the layer won't write again until we
// re-arm, we own the buffer while reading -- no seqlock needed.
//
// While no game is running the layer never clears request, so we simply stay
// parked in FUTEX_WAIT at ~0 CPU.
//
// Layout mirrors game_fps/fps_shm.h -- both sides must agree on this struct.
//
// Env:
//   LATENCY_SHM_NAME   shm name (default "/game_fps")

use std::ffi::CString;
use std::sync::atomic::{AtomicU32, Ordering};

// Must match FpsShm in game_fps/fps_shm.h.
#[repr(C)]
struct FpsShm {
    request: AtomicU32, // 1 = sample requested, 0 = idle / result ready
    _pad: u32,
    fps: f64,
    min_frametime_ms: f64,
    max_frametime_ms: f64,
    frame_count: u64,
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

/// Map the shm read-write (we set the request flag). Uses O_CREAT so either
/// side may start first: whoever opens it first creates it. The mapping stays
/// valid for the process lifetime (never unmapped here).
fn map_shm(name: &str) -> Result<*mut FpsShm, String> {
    let cname = CString::new(name).map_err(|e| e.to_string())?;
    let fd = unsafe { libc::shm_open(cname.as_ptr(), libc::O_CREAT | libc::O_RDWR, 0o666) };
    if fd < 0 {
        return Err(format!("shm_open({name}) failed: {}", std::io::Error::last_os_error()));
    }
    let size = std::mem::size_of::<FpsShm>();
    // Size the object; harmless if it already exists at this size.
    if unsafe { libc::ftruncate(fd, size as libc::off_t) } != 0 {
        unsafe { libc::close(fd) };
        return Err(format!("ftruncate failed: {}", std::io::Error::last_os_error()));
    }
    unsafe { libc::fchmod(fd, 0o666) };
    let p = unsafe {
        libc::mmap(
            std::ptr::null_mut(),
            size,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_SHARED,
            fd,
            0,
        )
    };
    unsafe { libc::close(fd) };
    if p == libc::MAP_FAILED {
        return Err(format!("mmap failed: {}", std::io::Error::last_os_error()));
    }
    Ok(p as *mut FpsShm)
}

/// Read the result fields. Safe to read plainly: the handshake guarantees the
/// layer is not writing while request == 0.
fn read_result(shm: &FpsShm) -> Snapshot {
    Snapshot {
        fps: shm.fps,
        min_frametime_ms: shm.min_frametime_ms,
        max_frametime_ms: shm.max_frametime_ms,
        frame_count: shm.frame_count,
    }
}

/// Block while `word` still equals `expected`. No timeout: if the layer never
/// clears request (no game running), we stay parked at ~0 CPU.
fn futex_wait(word: &AtomicU32, expected: u32) {
    unsafe {
        libc::syscall(
            libc::SYS_futex,
            word as *const AtomicU32,
            libc::FUTEX_WAIT,
            expected,
            std::ptr::null::<libc::timespec>(),
        );
    }
    // EAGAIN (already changed) and spurious wakeups just mean "re-check request".
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

    eprintln!("fps_reader: reading {name} (request/response)");

    loop {
        // Arm a request and wake the layer.
        shm.request.store(1, Ordering::Release);

        // Wait until the layer measures a window and clears request back to 0.
        while shm.request.load(Ordering::Acquire) == 1 {
            futex_wait(&shm.request, 1);
        }

        let snap = read_result(shm);
        println!(
            "fps={:.1}  min={:.3} ms  max={:.3} ms  (frames={})",
            snap.fps, snap.min_frametime_ms, snap.max_frametime_ms, snap.frame_count
        );
    }
}
