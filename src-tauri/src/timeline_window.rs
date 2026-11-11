//! The Timeline preview's window (`buffrx-ui.exe --timeline [session]`): the whole work area,
//! sliding down onto the game from above the screen in 180 ms.
//!
//! The geometry is a pure function so it can be checked without a window: the monitor's work rect (not
//! its full rect) decides both the width and the top, which is what keeps the preview clear of the
//! taskbar on every monitor, at every DPI. The Win32 half is the shell: it takes the frame away, makes
//! the window a tool window that stays out of Alt+Tab, places it, animates it, and keeps it out of
//! every capture.
//!
//! Everything here is physical pixels: the shell is DPI aware (its manifest is PerMonitorV2), so a
//! work rect from `GetMonitorInfoW` and a `SetWindowPos` speak the same units at 100% and at 150%.

/// A rectangle in physical pixels. The same shape as Win32's `RECT`, with width and height instead of
/// right and bottom, because every number this module reasons about is a size.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rect {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
}

/// The preview's rectangle: the whole work area. The picture inside is contain-fit, so a 4:3 or an
/// ultrawide recording is letterboxed on the ground colour rather than cropped, and the strip keeps its
/// full width. A negative or zero work rect gives a zero size rather than a negative one.
pub fn preview_bounds(work: Rect) -> Rect {
    Rect { x: work.x, y: work.y, width: work.width.max(0), height: work.height.max(0) }
}

#[cfg(windows)]
#[repr(C)]
#[derive(Default, Clone, Copy)]
struct WinRect {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
}

#[cfg(windows)]
#[repr(C)]
#[derive(Default)]
struct MonitorInfo {
    size: u32,
    monitor: WinRect,
    work: WinRect,
    flags: u32,
}

/// Takes the window whose handle this is and makes it the Timeline preview: no frame, no taskbar
/// entry, always on top, interactive, above the game it was opened over, and invisible to capture.
///
/// Called once, right after the window is created (still hidden), from the app's setup. The page owns
/// the short entrance animation, so opening the window does not block WebView's event loop.
#[cfg(windows)]
pub fn apply_preview_window(hwnd: isize) {
    if hwnd == 0 {
        return;
    }
    unsafe {
        // The frame goes first: the styles decide the client area, and the window is sized to exactly
        // its page, so a caption would eat 30 pixels of the picture.
        const GWL_STYLE: i32 = -16;
        const GWL_EXSTYLE: i32 = -20;
        const WS_CAPTION: isize = 0x00C0_0000;
        const WS_THICKFRAME: isize = 0x0004_0000;
        const WS_SYSMENU: isize = 0x0008_0000;
        const WS_EX_TOOLWINDOW: isize = 0x0000_0080;
        const WS_EX_NOACTIVATE: isize = 0x0800_0000;
        const SWP_NOSIZE: u32 = 0x0001;
        const SWP_NOMOVE: u32 = 0x0002;
        const SWP_NOZORDER: u32 = 0x0004;
        const SWP_NOACTIVATE: u32 = 0x0010;
        const SWP_FRAMECHANGED: u32 = 0x0020;
        const HWND_TOPMOST: isize = -1;

        let style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        SetWindowLongPtrW(hwnd, GWL_STYLE, style & !(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU));
        // A tool window keeps it out of Alt+Tab and off the taskbar. NOACTIVATE is cleared rather than
        // added: the preview is meant to be used, so the game's keys must be able to reach it.
        let extended = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (extended | WS_EX_TOOLWINDOW) & !WS_EX_NOACTIVATE);
        SetWindowPos(hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

        let work = work_rect();
        let bounds = preview_bounds(work);
        log(&format!(
            "timeline window: work {}x{} at ({}, {}), preview {}x{} at ({}, {})",
            work.width, work.height, work.x, work.y, bounds.width, bounds.height, bounds.x, bounds.y
        ));

        // WDA_EXCLUDEFROMCAPTURE: Windows leaves this window out of every screenshot and recording,
        // BuffrX's included, so the preview never ends up inside a clip of the game it is covering.
        // BUFFRX_TIMELINE_CAPTURABLE=1 leaves it in, so a test can see it.
        const WDA_EXCLUDEFROMCAPTURE: u32 = 0x0000_0011;
        let capturable = std::env::var_os("BUFFRX_TIMELINE_CAPTURABLE").is_some();
        let excluded = !capturable && SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE) != 0;
        log(&format!("timeline window: display affinity excluded={excluded}"));

        // Placed and topmost but NOT shown: a window shown before its page has painted flashes whatever the browser has at that
        // moment (a pale bar at the top for a few frames). The page calls `timeline_ready` once it can paint its first frame, and
        // `reveal` shows it then; a page that never answers is shown after 3 s, so a broken page is never an invisible process.
        SetWindowPos(hwnd, HWND_TOPMOST, bounds.x, bounds.y, bounds.width, bounds.height, SWP_NOACTIVATE);
        // Layered at alpha 0: the window can be shown the moment the page runs (so the page has frames to paint with) without a pixel
        // of it reaching the screen. It is faded in by fade_in when the page's arrival starts. Everything a half-ready window
        // draws (a pale Windows title bar from the window's own surface, caught in a frame capture) is behind that alpha.
        const WS_EX_LAYERED: isize = 0x0008_0000;
        let ex_now = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_now | WS_EX_LAYERED);
        SetLayeredWindowAttributes(hwnd, 0, 0, 2);
        // Shown now, at alpha 0, so the page is a visible page from its first frame and paints without waiting for a later show.
        reveal(hwnd);
        // The screen behind is read now, while the page loads, so the first copy is ready the moment the page asks for it.
        std::thread::spawn(|| {
            if let Some(copy) = capture_backdrop() {
                if let Ok(mut slot) = PREFETCH.lock() {
                    *slot = Some(copy);
                }
            }
        });
        std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_millis(3000));
            if CLOSING.load(std::sync::atomic::Ordering::Acquire) {
                return;
            }
            reveal(hwnd);
            fade_in(hwnd);
        });
    }
}

static PREFETCH: std::sync::Mutex<Option<String>> = std::sync::Mutex::new(None);
static REVEALED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
static CLOSING: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// Shows the preview window (once) and takes the foreground for it. Called by the page when it is ready to paint, or by the 3 s fallback.
#[cfg(windows)]
pub fn reveal(hwnd: isize) {
    if hwnd == 0 || REVEALED.swap(true, std::sync::atomic::Ordering::SeqCst) {
        return;
    }
    unsafe {
        if IsWindow(hwnd) == 0 {
            return;
        }
        const SW_SHOWNOACTIVATE: i32 = 4;
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);

        // Off the message loop, because the first tries are refused: Windows keeps the foreground for a
        // fullscreen game, then hands it to the desktop (or a console) when the game leaves, and a window of
        // a background process may not take it from there either (`AttachThreadInput` was tried and does not
        // help). A process that has just received an input event may: a bare Alt tap is that event and does
        // nothing else. It is only tapped once the game had its 300 ms to leave the screen, and it is never
        // needed on a windowed or borderless game, which the first try takes.
        std::thread::spawn(move || {
            for attempt in 0..8 {
                // The preview can close before an attempt lands (a person presses Escape at once), and Windows
                // hands a dead window's number to whatever is created next: a handle that is gone must stop the
                // loop, or the tap below would put a stranger's window in front and send it a keystroke.
                if IsWindow(hwnd) == 0 {
                    log("timeline window: gone before it took the foreground");
                    break;
                }
                if SetForegroundWindow(hwnd) != 0 {
                    log(&format!("timeline window: took the foreground on attempt {attempt}"));
                    break;
                }
                if attempt >= 2 {
                    if IsWindow(hwnd) == 0 {
                        log("timeline window: gone before the Alt tap");
                        break;
                    }
                    keybd_event(0x12, 0, 0, 0);
                    let taken = SetForegroundWindow(hwnd);
                    keybd_event(0x12, 0, 2, 0);
                    if taken != 0 {
                        log("timeline window: took the foreground with an Alt tap");
                        break;
                    }
                }
                std::thread::sleep(std::time::Duration::from_millis(150));
            }
        });
    }
}

static FADED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// Fades the window from alpha 0 to opaque over the length of the page's arrival (420 ms, ease-out), once. The page calls it when
/// its first frames are out; the 3 s fallback calls it for a page that never does.
#[cfg(windows)]
pub fn fade_in(hwnd: isize) {
    if hwnd == 0 || FADED.swap(true, std::sync::atomic::Ordering::SeqCst) {
        return;
    }
    std::thread::spawn(move || unsafe {
        const TOTAL_MS: f64 = 420.0;
        let start = std::time::Instant::now();
        loop {
            if IsWindow(hwnd) == 0 {
                return;
            }
            let t = (start.elapsed().as_secs_f64() * 1000.0 / TOTAL_MS).min(1.0);
            let eased = 1.0 - (1.0 - t).powi(4);
            SetLayeredWindowAttributes(hwnd, 0, (eased * 255.0).round() as u8, 2);
            if t >= 1.0 {
                return;
            }
            std::thread::sleep(std::time::Duration::from_millis(6));
        }
    });
}

static LEAVING_SINCE: std::sync::Mutex<Option<std::time::Instant>> = std::sync::Mutex::new(None);
const LEAVE_MS: f64 = 90.0;

/// Fades the window out over 90 ms (ease-in: it leaves faster than it arrived). The page calls it as the first thing it does on the
/// way out; `finish_leave` makes the process wait for the end of it before it exits.
#[cfg(windows)]
pub fn fade_out(hwnd: isize) {
    if hwnd == 0 {
        return;
    }
    if let Ok(mut since) = LEAVING_SINCE.lock() {
        if since.is_some() {
            return;
        }
        *since = Some(std::time::Instant::now());
    }
    CLOSING.store(true, std::sync::atomic::Ordering::Release);    std::thread::spawn(move || unsafe {
        let start = std::time::Instant::now();
        loop {
            if IsWindow(hwnd) == 0 {
                return;
            }
            let t = (start.elapsed().as_secs_f64() * 1000.0 / LEAVE_MS).min(1.0);
            let alpha = 255.0 * (1.0 - t * t * t);
            SetLayeredWindowAttributes(hwnd, 0, alpha.round() as u8, 2);
            if t >= 1.0 {
                return;
            }
            std::thread::sleep(std::time::Duration::from_millis(5));
        }
    });
}

/// Waits for what is left of the exit fade, so the window is never cut off half-way.
pub fn finish_leave() {
    let since = LEAVING_SINCE.lock().ok().and_then(|g| *g);
    if let Some(started) = since {
        let elapsed = started.elapsed().as_secs_f64() * 1000.0;
        if elapsed < LEAVE_MS {
            std::thread::sleep(std::time::Duration::from_millis((LEAVE_MS - elapsed) as u64 + 5));
        }
    }
}

/// What is on the screen behind the window, as a small BMP data URL (about 320 px wide): the page blurs and dims
/// it, which is the Steam overlay's look. The window is excluded from capture, so this sees the game and the
/// desktop and never the preview itself, and it can be asked again every half second to stay live. GDI's
/// halftone stretch averages while it shrinks, so the cost is a few milliseconds. A game that GDI cannot read
/// (some exclusive-fullscreen ones) gives black, which the page's dark ground already is.
#[cfg(windows)]
pub fn backdrop() -> Option<String> {
    if let Ok(mut slot) = PREFETCH.lock() {
        if let Some(copy) = slot.take() {
            return Some(copy);
        }
    }
    capture_backdrop()
}

#[cfg(windows)]
fn capture_backdrop() -> Option<String> {
    const WIDTH: i32 = 320;
    const HALFTONE: i32 = 4;
    const SRCCOPY: u32 = 0x00CC_0020;
    unsafe {
        let work = work_rect();
        if work.width <= 0 || work.height <= 0 {
            return None;
        }
        let height = (WIDTH as i64 * work.height as i64 / work.width as i64).max(1) as i32;
        let screen = GetDC(0);
        if screen == 0 {
            return None;
        }
        let memory = CreateCompatibleDC(screen);
        let info = BitmapInfoHeader {
            size: 40,
            width: WIDTH,
            height: -height,
            planes: 1,
            bit_count: 32,
            compression: 0,
            size_image: 0,
            x_pels: 0,
            y_pels: 0,
            used: 0,
            important: 0,
        };
        let mut bits: *mut std::ffi::c_void = std::ptr::null_mut();
        let bitmap = CreateDIBSection(screen, &info, 0, &mut bits, 0, 0);
        let mut out = None;
        if memory != 0 && bitmap != 0 && !bits.is_null() {
            let old = SelectObject(memory, bitmap);
            SetStretchBltMode(memory, HALFTONE);
            if StretchBlt(memory, 0, 0, WIDTH, height, screen, work.x, work.y, work.width, work.height, SRCCOPY) != 0 {
                let len = (WIDTH * height * 4) as usize;
                let mut pixels = std::slice::from_raw_parts(bits as *const u8, len).to_vec();
                for alpha in pixels.iter_mut().skip(3).step_by(4) {
                    *alpha = 255;
                }
                let mut bmp = Vec::with_capacity(54 + len);
                bmp.extend_from_slice(b"BM");
                bmp.extend_from_slice(&((54 + len) as u32).to_le_bytes());
                bmp.extend_from_slice(&[0; 4]);
                bmp.extend_from_slice(&54u32.to_le_bytes());
                for field in [40u32, WIDTH as u32, (-height) as u32, 1 | (32 << 16), 0, 0, 0, 0, 0, 0] {
                    bmp.extend_from_slice(&field.to_le_bytes());
                }
                bmp.extend_from_slice(&pixels);
                if std::env::var_os("BUFFRX_TIMELINE_DUMP").is_some() {
                    let _ = std::fs::write(crate::data_root().join("backdrop.bmp"), &bmp);
                }
                out = Some(format!("data:image/bmp;base64,{}", base64(&bmp)));
            }
            SelectObject(memory, old);
        }
        if bitmap != 0 {
            DeleteObject(bitmap);
        }
        if memory != 0 {
            DeleteDC(memory);
        }
        ReleaseDC(0, screen);
        out
    }
}

#[cfg(windows)]
fn base64(bytes: &[u8]) -> String {
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let n = (chunk[0] as u32) << 16 | (*chunk.get(1).unwrap_or(&0) as u32) << 8 | *chunk.get(2).unwrap_or(&0) as u32;
        out.push(TABLE[(n >> 18) as usize & 63] as char);
        out.push(TABLE[(n >> 12) as usize & 63] as char);
        out.push(if chunk.len() > 1 { TABLE[(n >> 6) as usize & 63] as char } else { '=' });
        out.push(if chunk.len() > 2 { TABLE[n as usize & 63] as char } else { '=' });
    }
    out
}

#[cfg(windows)]
#[repr(C)]
struct BitmapInfoHeader {
    size: u32,
    width: i32,
    height: i32,
    planes: u16,
    bit_count: u16,
    compression: u32,
    size_image: u32,
    x_pels: i32,
    y_pels: i32,
    used: u32,
    important: u32,
}

/// Hide video and back: the preview keeps its place and width and takes `height` physical pixels from the
/// top of the work area, or all of it with `None`. Never taller than the work area, never zero.
#[cfg(windows)]
pub fn set_preview_height(hwnd: isize, height: Option<i32>) {
    const SWP_NOZORDER: u32 = 0x0004;
    const SWP_NOACTIVATE: u32 = 0x0010;
    unsafe {
        let bounds = preview_bounds(work_rect());
        let h = height.map(|h| h.clamp(1, bounds.height.max(1))).unwrap_or(bounds.height);
        SetWindowPos(hwnd, 0, bounds.x, bounds.y, bounds.width, h, SWP_NOACTIVATE | SWP_NOZORDER);
    }
}

/// The work rect (the monitor's rectangle without the taskbar) of the monitor the foreground window is
/// on, else of the primary monitor, else the whole primary screen.
#[cfg(windows)]
unsafe fn work_rect() -> Rect {
    const MONITOR_DEFAULTTONEAREST: u32 = 2;
    let foreground = GetForegroundWindow();
    let mut info = MonitorInfo { size: std::mem::size_of::<MonitorInfo>() as u32, ..Default::default() };
    let monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    if monitor != 0 && GetMonitorInfoW(monitor, &mut info) != 0 {
        let work = info.work;
        return Rect { x: work.left, y: work.top, width: work.right - work.left, height: work.bottom - work.top };
    }
    Rect { x: 0, y: 0, width: GetSystemMetrics(0), height: GetSystemMetrics(1) }
}

/// One bounded line into ui.log: the numbers only, never a session id or a path.
#[cfg(windows)]
fn log(message: &str) {
    crate::pipe::write_log(&crate::data_root(), message);
}

#[cfg(windows)]
extern "system" {
    fn GetWindowLongPtrW(hwnd: isize, index: i32) -> isize;
    fn SetWindowLongPtrW(hwnd: isize, index: i32, value: isize) -> isize;
    fn SetWindowPos(hwnd: isize, after: isize, x: i32, y: i32, cx: i32, cy: i32, flags: u32) -> i32;
    fn SetWindowDisplayAffinity(hwnd: isize, affinity: u32) -> i32;
    fn GetForegroundWindow() -> isize;
    fn MonitorFromWindow(hwnd: isize, flags: u32) -> isize;
    fn GetMonitorInfoW(monitor: isize, info: *mut MonitorInfo) -> i32;
    fn GetSystemMetrics(index: i32) -> i32;
    fn SetForegroundWindow(hwnd: isize) -> i32;
    fn ShowWindow(hwnd: isize, command: i32) -> i32;
    fn SetLayeredWindowAttributes(hwnd: isize, key: u32, alpha: u8, flags: u32) -> i32;
    fn IsWindow(hwnd: isize) -> i32;
    fn keybd_event(key: u8, scan: u8, flags: u32, extra: usize);
}

#[cfg(windows)]
#[link(name = "gdi32")]
extern "system" {
    fn CreateCompatibleDC(hdc: isize) -> isize;
    fn CreateDIBSection(hdc: isize, info: *const BitmapInfoHeader, usage: u32, bits: *mut *mut std::ffi::c_void, section: isize, offset: u32) -> isize;
    fn SelectObject(hdc: isize, object: isize) -> isize;
    fn DeleteObject(object: isize) -> i32;
    fn DeleteDC(hdc: isize) -> i32;
    fn SetStretchBltMode(hdc: isize, mode: i32) -> i32;
    fn StretchBlt(dest: isize, x: i32, y: i32, w: i32, h: i32, src: isize, sx: i32, sy: i32, sw: i32, sh: i32, rop: u32) -> i32;
}

#[cfg(windows)]
extern "system" {
    fn GetDC(hwnd: isize) -> isize;
    fn ReleaseDC(hwnd: isize, hdc: isize) -> i32;
}

#[cfg(test)]
mod tests {
    use super::*;

    const WORK: Rect = Rect { x: 0, y: 0, width: 1920, height: 1040 };

    #[test]
    #[cfg(windows)]
    fn base64_pads_like_the_standard() {
        assert_eq!(base64(b""), "");
        assert_eq!(base64(b"f"), "Zg==");
        assert_eq!(base64(b"fo"), "Zm8=");
        assert_eq!(base64(b"foo"), "Zm9v");
    }

    #[test]
    fn the_preview_is_the_whole_work_area() {
        assert_eq!(preview_bounds(WORK), WORK);
    }

    #[test]
    fn a_second_monitor_keeps_its_own_rect() {
        let right = Rect { x: 1920, ..WORK };
        assert_eq!(preview_bounds(right), right, "the preview covers the monitor it is on");
        let below = Rect { x: 0, y: 1040, ..WORK };
        assert_eq!(preview_bounds(below), below, "and hangs from that monitor's own top");
    }

    #[test]
    fn every_scale_is_covered() {
        let rects = [WORK, Rect { x: 0, y: 0, width: 2560, height: 1400 }, Rect { x: 0, y: 0, width: 3840, height: 2080 }];
        for work in rects {
            assert_eq!(preview_bounds(work), work, "{}x{} is the whole room the picture gets", work.width, work.height);
        }
    }

    #[test]
    fn a_tiny_or_broken_work_rect_gives_what_there_is() {
        let tiny = Rect { x: 0, y: 0, width: 200, height: 120 };
        assert_eq!(preview_bounds(tiny), tiny, "a small work area is filled, not padded");
        assert_eq!(preview_bounds(Rect { x: 0, y: 0, width: 0, height: 0 }), Rect { x: 0, y: 0, width: 0, height: 0 });
        assert_eq!(preview_bounds(Rect { x: -1920, y: -200, width: 1920, height: -50 }).height, 0, "negative height");
    }

    #[test]
    fn an_ultrawide_is_covered_too() {
        for work in [Rect { x: 0, y: 0, width: 3440, height: 1400 }, Rect { x: 0, y: 0, width: 5120, height: 2760 }] {
            assert_eq!(preview_bounds(work), work, "the strip and the picture share the width");
        }
    }

}
