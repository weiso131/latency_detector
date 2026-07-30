// Reader side of the fps/frametime IPC buffer written by the Vulkan layer.
//
// Reader-driven: we store the window length in `request_sec`, wake the layer,
// and block on FUTEX_WAIT until that word leaves the value we wrote. The layer
// measures a window of that length, writes the fields, sets request_sec = 0,
// and wakes us. Because the layer won't write again until we re-arm, we own the
// buffer while reading -- no seqlock needed.
//
// The word carries both the request and its parameter: nonzero = "measure this
// many seconds", 0 = idle / result ready.
//
// While no game is running the layer never clears request_sec, so we simply
// stay parked in FUTEX_WAIT at ~0 CPU.
//
// The tracepoint tables live in a second, independent shm object and need no
// handshake at all -- they are read straight after each fps result here purely
// because that is a convenient moment to print them.
//
// Layouts mirror game_fps/fps_shm.h and game_fps/vk_trace.h -- both sides must
// agree on these structs.
//
// Env:
//   LATENCY_SHM_NAME   fps shm name (default "/game_fps")
//   VK_TRACE_SHM_NAME  tracepoint shm name (default "/vk_trace")
//   LATENCY_WINDOW_SEC measurement window in seconds (default 1, must be > 0)

use std::ffi::CString;
use std::sync::atomic::{AtomicU32, Ordering};

// Must match PRESENT_MAX_TIDS / SUBMIT_MAX_TIDS in game_fps/vk_trace.h.
const PRESENT_MAX_TIDS: usize = 4;
const SUBMIT_MAX_TIDS: usize = 32;

// Must match TidSlot in game_fps/vk_trace.h.
#[repr(C)]
struct TidSlot {
    tid: AtomicU32,         // 0 = free slot, else the calling kernel tid
    max_per_sec: AtomicU32, // high-water mark of calls in any one second
}

// Must match VkTraceShm in game_fps/vk_trace.h. A separate object from FpsShm:
// no handshake, always valid to read.
#[repr(C)]
struct VkTraceShm {
    present_tids: [TidSlot; PRESENT_MAX_TIDS],
    // vkQueueSubmit and vkQueueSubmit2 both count here.
    submit_tids: [TidSlot; SUBMIT_MAX_TIDS],
}

// Must match FpsShm in game_fps/fps_shm.h.
#[repr(C)]
struct FpsShm {
    request_sec: AtomicU32, // >0 = window length in seconds, 0 = idle / result ready
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

/// Open the shm object, tolerating either side starting first.
fn open_shm_fd(cname: &CString) -> Result<i32, String> {
    loop {
        // Try to open an existing object without O_CREAT.
        let fd = unsafe { libc::shm_open(cname.as_ptr(), libc::O_RDWR, 0o666) };
        if fd >= 0 {
            return Ok(fd);
        }
        let err = std::io::Error::last_os_error();
        if err.raw_os_error() != Some(libc::ENOENT) {
            return Err(format!("shm_open failed: {err}"));
        }
        // Doesn't exist yet: try to create it exclusively.
        let fd =
            unsafe { libc::shm_open(cname.as_ptr(), libc::O_RDWR | libc::O_CREAT | libc::O_EXCL, 0o666) };
        if fd >= 0 {
            return Ok(fd);
        }
        let err = std::io::Error::last_os_error();
        if err.raw_os_error() == Some(libc::EEXIST) {
            // Someone created it between our open and create: reopen it.
            continue;
        }
        return Err(format!("shm_open (create) failed: {err}"));
    }
}

/// Map the shm read-write (we set the request flag). Either side may start
/// first: whoever opens it first creates it (see `open_shm_fd`). The mapping
/// stays valid for the process lifetime (never unmapped here).
fn map_shm<T>(name: &str) -> Result<*mut T, String> {
    let cname = CString::new(name).map_err(|e| e.to_string())?;
    let fd = open_shm_fd(&cname).map_err(|e| format!("{e} ({name})"))?;
    let size = std::mem::size_of::<T>();
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
    Ok(p as *mut T)
}

/// Read the result fields. Safe to read plainly: the handshake guarantees the
/// layer is not writing while request_sec == 0.
fn read_result(shm: &FpsShm) -> Snapshot {
    Snapshot {
        fps: shm.fps,
        min_frametime_ms: shm.min_frametime_ms,
        max_frametime_ms: shm.max_frametime_ms,
        frame_count: shm.frame_count,
    }
}

/// Read one of the per-thread call tables. Independent of the handshake: every
/// field is atomic and the layer maintains the tables on every call, so this is
/// a plain snapshot taken whenever we like.
///
/// Scan the whole array rather than stopping at the first free slot: a thread
/// that loses the claim CAS moves on to a later index, so an occupied slot can
/// sit past a free one.
fn read_tid_table(slots: &[TidSlot]) -> Vec<(u32, u32)> {
    slots
        .iter()
        .map(|slot| {
            (
                slot.tid.load(Ordering::Acquire),
                slot.max_per_sec.load(Ordering::Acquire),
            )
        })
        .filter(|&(tid, _)| tid != 0)
        .collect()
}

/// Block while `word` still equals `expected`. No timeout: if the layer never
/// clears request_sec (no game running), we stay parked at ~0 CPU.
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
    // EAGAIN (already changed) and spurious wakeups just mean "re-check the word".
}

fn main() {
    let name = env_str("LATENCY_SHM_NAME", "/game_fps");

    // 0 would collide with the idle / result-ready state, so it is not a valid
    // window length.
    let window_sec: u32 = match env_str("LATENCY_WINDOW_SEC", "1").parse() {
        Ok(n) if n > 0 => n,
        _ => {
            eprintln!("fps_reader: LATENCY_WINDOW_SEC must be a positive integer");
            std::process::exit(1);
        }
    };

    let shm_ptr = match map_shm::<FpsShm>(&name) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("fps_reader: {e}");
            std::process::exit(1);
        }
    };
    let shm = unsafe { &*shm_ptr };

    let trace_name = env_str("VK_TRACE_SHM_NAME", "/vk_trace");
    let trace_ptr = match map_shm::<VkTraceShm>(&trace_name) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("fps_reader: {e}");
            std::process::exit(1);
        }
    };
    let trace = unsafe { &*trace_ptr };

    eprintln!("fps_reader: reading {name} (request/response, {window_sec}s window)");

    loop {
        // Arm a request for a window of `window_sec` seconds and wake the layer.
        shm.request_sec.store(window_sec, Ordering::Release);

        // Wait until the layer measures the window and clears the word back to 0.
        while shm.request_sec.load(Ordering::Acquire) == window_sec {
            futex_wait(&shm.request_sec, window_sec);
        }

        let snap = read_result(shm);
        println!(
            "fps={:.1}  min={:.3} ms  max={:.3} ms  (frames={})",
            snap.fps, snap.min_frametime_ms, snap.max_frametime_ms, snap.frame_count
        );

        for (label, slots) in [
            ("present", &trace.present_tids[..]),
            ("submit", &trace.submit_tids[..]),
        ] {
            let tids = read_tid_table(slots);
            if !tids.is_empty() {
                let list: Vec<String> = tids
                    .iter()
                    .map(|(tid, max)| format!("{tid}(max {max}/s)"))
                    .collect();
                println!("  {label} tids: {}", list.join("  "));
            }
        }
    }
}
