#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]

//! The BuffrX editor host (`buffrx-ui.exe --editor [files...]`, `--editor --preview <file>`).
//!
//! The panel, its compact layout, Settings and the tray panel are drawn by the engine itself
//! (src/engine/ui): BuffrX is one process while no editor is open. This executable is the editor, and
//! the clip preview that becomes the editor when Edit is pressed, in a process of its own: closing it
//! frees every byte it used, and a crash in it never touches the engine.
//!
//! It is single-instance: a second launch hands its files to the first through the engine
//! (`editor.open`) and exits. A launch without `--editor` (an old shortcut) asks the engine to show its
//! panel, starting the engine when it is not running.

use std::env;
use std::fs;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use tauri::{AppHandle, Emitter, Manager, WebviewUrl, WebviewWindowBuilder};

mod media;
mod pipe;
mod pool;
mod timeline_window;

fn data_root() -> PathBuf {
    if let Ok(v) = env::var("BUFFRX_DATA_ROOT") {
        if !v.is_empty() {
            let p = PathBuf::from(v);
            let _ = fs::create_dir_all(&p);
            return p;
        }
    }
    let exe_dir = env::current_exe().ok().and_then(|p| p.parent().map(PathBuf::from)).unwrap_or_default();
    if exe_dir.join("portable.flag").exists() {
        let p = exe_dir.join("data");
        let _ = fs::create_dir_all(&p);
        return p;
    }
    #[cfg(windows)]
    let p = if let Ok(local) = env::var("LOCALAPPDATA") {
        if local.is_empty() { exe_dir.join("data") } else { PathBuf::from(local).join("BuffrX") }
    } else {
        // No profile to put it in: keep the data beside the binaries instead of resolving a relative
        // "BuffrX" against whatever working directory the launch happened to have.
        exe_dir.join("data")
    };
    #[cfg(unix)]
    let p = if let Ok(xdg) = env::var("XDG_DATA_HOME") {
        if xdg.is_empty() { exe_dir.join("data") } else { PathBuf::from(xdg).join("buffrx") }
    } else if let Ok(home) = env::var("HOME") {
        PathBuf::from(home).join(".local/share/buffrx")
    } else {
        exe_dir.join("data")
    };
    let _ = fs::create_dir_all(&p);
    p
}

pub(crate) fn engine_path() -> PathBuf {
    let directory = env::current_exe().ok().and_then(|p| p.parent().map(PathBuf::from)).unwrap_or_default();
    #[cfg(windows)]
    let name = "buffrx-engine.exe";
    #[cfg(unix)]
    let name = "buffrx-engine";
    directory.join(name)
}

/// The two schemes every window serves: media (byte ranges from the allowed root) and thumbnails (cached
/// names only). One copy, so the editor and the Timeline cannot drift apart.
fn with_media_schemes<R: tauri::Runtime>(
    builder: tauri::Builder<R>,
    media_root: PathBuf,
    thumbnail_root: PathBuf,
) -> tauri::Builder<R> {
    builder
        .register_asynchronous_uri_scheme_protocol("media", move |_ctx, request, responder| {
            let root = media_root.clone();
            pool::run(move || responder.respond(media::serve(&root, &request)));
        })
        .register_asynchronous_uri_scheme_protocol("thumbnails", move |_ctx, request, responder| {
            let root = thumbnail_root.clone();
            pool::run(move || {
                let name = media::percent_decode(request.uri().path().trim_start_matches('/'));
                let body = resolve_thumbnail(&root, &name).and_then(|p| fs::read(p).ok());
                let response = match body {
                    Some(bytes) => tauri::http::Response::builder()
                        .header("Content-Type", media::content_type(std::path::Path::new(&name)))
                        .header("Cache-Control", "max-age=3600")
                        .body(bytes)
                        .unwrap(),
                    None => tauri::http::Response::builder().status(404).body(Vec::new()).unwrap(),
                };
                responder.respond(response);
            });
        })
}

/// What this process was started to be.
#[derive(Clone)]
struct Launch {
    editor: bool,       // false: not an editor launch at all (hand over to the engine's panel)
    view: String,       // "editor" | "preview" | "timeline"
    files: Vec<String>, // the files it was opened with
    timeline: bool,      // `--timeline [session]`: the on-demand Timeline preview
    timeline_session: String, // the session it opens on, empty for its empty state
    parent: isize,
}

fn parse_launch(args: &[String]) -> Launch {
    let mut launch = Launch { editor: false, view: "editor".into(), files: Vec::new(), timeline: false, timeline_session: String::new(), parent: 0 };
    let mut section = "files";
    for arg in args.iter().skip(1) {
        match arg.as_str() {
            "--editor" => launch.editor = true,
            "--parent" => section = "parent",
            "--preview" => {
                launch.editor = true;
                launch.view = "preview".into();
            }
            "--timeline" => {
                launch.timeline = true;
                launch.view = "timeline".into();
                section = "timeline";
            }
            other if !other.starts_with("--") => match section {
                "parent" => {
                    launch.parent = other.parse().unwrap_or(0);
                    section = "files";
                }
                "timeline" => {
                    launch.timeline_session = other.to_string();
                    section = "files";
                }
                _ => {
                    if launch.editor {
                        launch.files.push(other.to_string())
                    }
                }
            },
            _ => {}
        }
    }
    launch
}

/// True when this process is the first holder of `name`: a named mutex, one per role.
fn acquire_instance(name: &str) -> bool {
    #[cfg(windows)]
    {
        use std::ffi::CString;
        extern "system" {
            fn CreateMutexA(attributes: *mut std::ffi::c_void, owner: i32, name: *const i8) -> *mut std::ffi::c_void;
            fn GetLastError() -> u32;
        }
        let name = CString::new(name).unwrap();
        let handle = unsafe { CreateMutexA(std::ptr::null_mut(), 0, name.as_ptr()) };
        // ERROR_ALREADY_EXISTS (183): another process holds the mutex.
        return handle.is_null() || unsafe { GetLastError() } != 183;
    }
    #[cfg(unix)]
    {
        use std::os::unix::io::AsRawFd;
        extern "C" {
            fn getuid() -> u32;
            fn flock(fd: i32, operation: i32) -> i32;
        }
        const LOCK_EX: i32 = 2;
        const LOCK_NB: i32 = 4;
        let base = env::var("XDG_RUNTIME_DIR").unwrap_or_else(|_| "/tmp".to_string());
        let uid = unsafe { getuid() };
        let safe: String = name.chars().map(|c| if c.is_ascii_alphanumeric() || c == '-' || c == '_' || c == '.' { c } else { '_' }).collect();
        let path = format!("{base}/buffrx-ui-{uid}-{safe}.lock");
        let Ok(file) = fs::OpenOptions::new().create(true).write(true).open(&path) else { return true };
        let taken = unsafe { flock(file.as_raw_fd(), LOCK_EX | LOCK_NB) } == 0;
        if taken {
            // The lock lives as long as the descriptor: keep it for the process's life.
            std::mem::forget(file);
        }
        return taken;
    }
}

/// True when this process is the first Timeline preview. A mutex of its own: the preview may run beside
/// the editor, but there is only ever one preview.
fn acquire_timeline_instance() -> bool { acquire_instance("Local\\BuffrX.timeline.SingleInstance") }

/// Sends one command to the engine and waits for its answer: how a second launch hands its request to
/// the first instance.
#[cfg(windows)]
fn engine_once(cmd: &str, args: serde_json::Value) -> bool {
    use std::io::{Read, Write};
    use std::time::{Duration, Instant};
    let Ok(mut pipe) = fs::OpenOptions::new().read(true).write(true).open(pipe::pipe_name()) else { return false };
    let mut send = |frame: String| -> bool {
        let bytes = frame.into_bytes();
        pipe.write_all(&(bytes.len() as u32).to_le_bytes()).is_ok() && pipe.write_all(&bytes).is_ok()
    };
    let ok = send(serde_json::json!({"id": 1, "cmd": "hello", "args": {"client": "forward"}}).to_string())
        && send(serde_json::json!({"id": 2, "cmd": cmd, "args": args}).to_string());
    if !ok {
        return false;
    }
    // A hung engine must not leave an invisible process waiting forever: wait for a whole frame, never
    // block inside a read that may never return.
    #[cfg(windows)]
    extern "system" {
        fn PeekNamedPipe(handle: *mut std::ffi::c_void, buffer: *mut std::ffi::c_void, size: u32,
                        read: *mut u32, available: *mut u32, left: *mut u32) -> i32;
    }
    let deadline = Instant::now() + Duration::from_secs(5);
    #[cfg(windows)]
    fn ready(pipe: &std::fs::File, needed: usize, deadline: Instant) -> bool {
        use std::os::windows::io::AsRawHandle;
        loop {
            let mut available = 0u32;
            let ok = unsafe {
                PeekNamedPipe(pipe.as_raw_handle(), std::ptr::null_mut(), 0, std::ptr::null_mut(), &mut available, std::ptr::null_mut())
            };
            if ok == 0 {
                return false;
            }
            if available as usize >= needed {
                return true;
            }
            if Instant::now() >= deadline {
                return false;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
    }
    // Read until the second answer (events may come first).
    for _ in 0..16 {
        if !ready(&pipe, 4, deadline) {
            return false;
        }
        let mut header = [0u8; 4];
        if pipe.read_exact(&mut header).is_err() {
            return false;
        }
        let length = u32::from_le_bytes(header) as usize;
        if !ready(&pipe, length, deadline) {
            return false;
        }
        let mut body = vec![0u8; length];
        if pipe.read_exact(&mut body).is_err() {
            return false;
        }
        if String::from_utf8_lossy(&body).contains("\"id\":2") {
            return true;
        }
    }
    false
}

#[cfg(unix)]
fn engine_once(cmd: &str, args: serde_json::Value) -> bool {
    use std::io::{Read, Write};
    use std::os::unix::net::UnixStream;
    use std::time::Duration;
    let Ok(mut stream) = UnixStream::connect(pipe::pipe_name()) else { return false };
    let _ = stream.set_read_timeout(Some(Duration::from_secs(5)));
    let mut send = |frame: String| -> bool {
        let bytes = frame.into_bytes();
        stream.write_all(&(bytes.len() as u32).to_le_bytes()).is_ok() && stream.write_all(&bytes).is_ok()
    };
    let ok = send(serde_json::json!({"id": 1, "cmd": "hello", "args": {"client": "forward"}}).to_string())
        && send(serde_json::json!({"id": 2, "cmd": cmd, "args": args}).to_string());
    if !ok {
        return false;
    }
    for _ in 0..16 {
        let mut header = [0u8; 4];
        if stream.read_exact(&mut header).is_err() {
            return false;
        }
        let length = u32::from_le_bytes(header) as usize;
        if length == 0 || length > 16 * 1024 * 1024 {
            return false;
        }
        let mut body = vec![0u8; length];
        if stream.read_exact(&mut body).is_err() {
            return false;
        }
        if String::from_utf8_lossy(&body).contains("\"id\":2") {
            return true;
        }
    }
    false
}

fn run_self_test() {
    use std::io::{Read, Write};
    #[cfg(unix)]
    let mut file = None;
    #[cfg(unix)]
    for _ in 0..50 {
        if let Ok(f) = std::os::unix::net::UnixStream::connect(pipe::pipe_name()) {
            file = Some(f);
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(100));
    }
    #[cfg(windows)]
    let mut file = None;
    #[cfg(windows)]
    for _ in 0..50 {
        if let Ok(f) = fs::OpenOptions::new().read(true).write(true).open(pipe::pipe_name()) {
            file = Some(f);
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(100));
    }
    let Some(mut file) = file else {
        eprintln!("SELF_TEST_FAIL no engine");
        std::process::exit(1);
    };
    let frame = br#"{"id":1,"cmd":"hello"}"#;
    if file.write_all(&(frame.len() as u32).to_le_bytes()).is_err() || file.write_all(frame).is_err() {
        eprintln!("SELF_TEST_FAIL write");
        std::process::exit(1);
    }
    let mut header = [0u8; 4];
    if file.read_exact(&mut header).is_err() {
        eprintln!("SELF_TEST_FAIL read header");
        std::process::exit(1);
    }
    let mut buf = vec![0u8; u32::from_le_bytes(header) as usize];
    if file.read_exact(&mut buf).is_err() {
        eprintln!("SELF_TEST_FAIL read body");
        std::process::exit(1);
    }
    println!("SELF_TEST_OK {}", String::from_utf8_lossy(&buf));
    std::process::exit(0);
}

fn view_script(view: &str) -> String {
    format!("window.__BUFFRX_VIEW={view:?};")
}

/// Events the engine sends to a running editor.
pub fn handle_engine_event(app: &AppHandle, name: &str, data: &serde_json::Value) {
    match name {
        "editor.files" => {
            let files = data.get("files").cloned().unwrap_or_default();
            if let Some(list) = files.as_array() {
                let paths: Vec<PathBuf> = list.iter().filter_map(|f| f.as_str()).map(PathBuf::from).collect();
                media::allow_dropped(&paths);
            }
            if let Some(window) = app.get_webview_window("editor") {
                let _ = window.unminimize();
                let _ = window.show();
                let _ = window.set_focus();
                // A clip clicked in the panel plays; "Trim in editor" edits.
                let preview = data.get("preview").and_then(|v| v.as_bool()).unwrap_or(false);
                let _ = window.emit(if preview { "preview-files" } else { "editor-files" }, files);
            }
        }
        "app.quit" | "timeline.close" => {
            pipe::write_log(&data_root(), &format!("shell: the engine sent {name}; the window goes with it"));
            if let Some(shared) = app.try_state::<Arc<Shared>>() { media::cancel_all(&shared.exports); }
            app.exit(0)
        }
        _ => {}
    }
}

struct Shared {
    data_root: PathBuf,
    launch: Launch,
    exports: Arc<media::Exports>,
}

/// The main panel's design-unit sizes at its 1.2 UI scale, expressed in logical pixels.
// The editor's start size, minimum and maximum, 35% above what they were (1152x816, 940.8x604.8, 1440x1008).
const EDITOR_SIZE: (f64, f64) = (1555.2, 1101.6);
const EDITOR_MIN: (f64, f64) = (1270.08, 816.48);
const EDITOR_MAX: (f64, f64) = (1944.0, 1360.8);

/// The editor's size, minimum and maximum for a monitor's work area, in logical pixels.
///
/// At 200% the default and minimum can exceed a 1920x1040 work area. All three sizes are clamped to
/// the work area in physical pixels and turned back into logical ones, so the editor still fits a
/// 1024x600 screen.
fn editor_sizes(work: (u32, u32), scale: f64) -> ((f64, f64), (f64, f64), (f64, f64)) {
    let scale = if scale.is_finite() && scale > 0.1 { scale } else { 1.0 };
    let fit = |(width, height): (f64, f64)| {
        ((width * scale).min(work.0 as f64) / scale, (height * scale).min(work.1 as f64) / scale)
    };
    (fit(EDITOR_SIZE), fit(EDITOR_MIN), fit(EDITOR_MAX))
}

/// The limits last given to the editor window, so a move that changes nothing writes nothing.
static EDITOR_MIN_SET: std::sync::OnceLock<std::sync::Mutex<Option<(u64, u32, u32, u32, u32)>>> = std::sync::OnceLock::new();

/// While the preview is fullscreen the window owns the monitor; the editor's design max must not
/// shrink it back. Set by the preview's own command, cleared when it leaves.
static PREVIEW_FULLSCREEN: AtomicBool = AtomicBool::new(false);

/// Fits the editor to the monitor it is on. Called when it opens, and again when it moves to another
/// monitor or its scale changes: a window carries the minimum it was given on the first, so one moved
/// from a 100% monitor to a 200% one keeps a minimum larger than the screen.
fn clamp_editor(
    label: &str,
    monitor: Option<&tauri::Monitor>,
    current: Option<(u32, u32)>,
    set_min: impl FnOnce(f64, f64),
    set_max: impl FnOnce(f64, f64),
    set_size: impl FnOnce(f64, f64),
) {
    if label != "editor" {
        return;
    }
    if PREVIEW_FULLSCREEN.load(Ordering::Relaxed) {
        return;
    }
    let Some(monitor) = monitor else { return };
    let scale = monitor.scale_factor();
    let work = monitor.work_area().size;
    let (_, min, max) = editor_sizes((work.width, work.height), scale);
    let key = (scale.to_bits(), min.0.round() as u32, min.1.round() as u32, max.0.round() as u32, max.1.round() as u32);
    let known = EDITOR_MIN_SET.get_or_init(Default::default);
    let mut known = known.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    let write_min = *known != Some(key);
    if write_min {
        *known = Some(key);
    }
    drop(known); // never held across a call that can post another window event
    if write_min {
        set_min(min.0, min.1);
        set_max(max.0, max.1);
    }
    // Only shrink dimensions outside the new limits; keep the other dimension the person chose.
    if let Some((width, height)) = current {
        let fit = ((max.0 * scale).round() as u32, (max.1 * scale).round() as u32);
        if width > fit.0 || height > fit.1 {
            set_size((width as f64 / scale).min(max.0), (height as f64 / scale).min(max.1));
        }
    }
}

/// Starts the engine and asks it to show its panel (an old shortcut to buffrx-ui.exe still opens BuffrX).
fn hand_over_to_engine() {
    if engine_once("ui.show", serde_json::json!({"view": "main"})) {
        return;
    }
    let engine = engine_path();
    if engine.exists() {
        let mut command = std::process::Command::new(engine);
        command.arg("--show");
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            command.creation_flags(0x08000000);  // CREATE_NO_WINDOW: the engine is a console binary
        }
        let _ = command.spawn();
    }
}

/// A failed start must not be a line in a log file only. The panel has already stepped aside for the editor, so a box is
/// the one thing a person will still see. `BUFFRX_TEST_NO_DIALOGS=1` answers at once, so a probe never waits on a click.
fn start_failed(title: &str, text: &str) {
    let _ = (title, text);
    #[cfg(windows)]
    {
        if std::env::var_os("BUFFRX_TEST_NO_DIALOGS").is_some() {
            return;
        }
        use std::ffi::OsStr;
        use std::os::windows::ffi::OsStrExt;
        extern "system" {
            fn MessageBoxW(hwnd: *mut std::ffi::c_void, text: *const u16, caption: *const u16, kind: u32) -> i32;
        }
        let wide = |s: &str| -> Vec<u16> { OsStr::new(s).encode_wide().chain(std::iter::once(0)).collect() };
        let body = wide(text);
        let caption = wide(title);
        // An error icon, no buttons beyond OK (0), and the foreground: this is the only window at that moment.
        const MB_ICONERROR: u32 = 0x0000_0010;
        const MB_SETFOREGROUND: u32 = 0x0001_0000;
        unsafe { MessageBoxW(std::ptr::null_mut(), body.as_ptr(), caption.as_ptr(), MB_ICONERROR | MB_SETFOREGROUND) };
    }
}

fn main() {
    let args: Vec<String> = env::args().collect();
    // WebView2 reads WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS from our environment; a leftover from a
    // debugging shell can turn on Chromium logging and a console window, so the value is replaced, not
    // inherited. The one flag kept is the autoplay policy: the Timeline preview opens on footage and
    // Live plays the chunk the writer holds, and Chromium otherwise blocks that unmuted play until a
    // trusted click. The panel's own clip page sets the same flag for the same reason.
    #[cfg(windows)]
    {
        extern "system" {
            fn SetEnvironmentVariableA(name: *const u8, value: *const u8) -> i32;
        }
        unsafe {
            SetEnvironmentVariableA(
                b"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS\0".as_ptr(),
                b"--autoplay-policy=no-user-gesture-required\0".as_ptr(),
            )
        };
    }
    if args.iter().any(|a| a == "--self-test") {
        run_self_test();
        return;
    }
    let launch = parse_launch(&args);
    if launch.timeline {
        run_timeline(launch);
        return;
    }
    if !launch.editor {
        hand_over_to_engine();
        return;
    }
    if launch.parent == 0 && !acquire_instance("Local\\BuffrX.editor.SingleInstance") {
        // The first editor does the work; this one only passes the files along.
        engine_once("editor.open", serde_json::json!({"files": launch.files, "preview": launch.view == "preview"}));
        return;
    }

    let data = data_root();
    media::start_http_server(data.clone());
    let embedded = launch.parent != 0;
    media::cleanup_timeline_cache(&data);
    let shared = Arc::new(Shared { data_root: data.clone(), launch: launch.clone(), exports: Arc::new(media::Exports::default()) });
    let media_root = data.clone();
    let thumbnail_root = data.clone();
    let builder = tauri::Builder::default()
        .manage(shared.clone())
        .invoke_handler(tauri::generate_handler![
            engine_request,
            get_launch,
            editor_ready,
            editor_leaving,
            parent_drag,
            parent_minimise,
            get_data_root,
            preview_fullscreen,
            preview_fullscreen_state,
            preview_proxy,
            media_run,
            media_read_cancel,
            timeline_merge,
            media_export,
            media_cancel,
            unique_path,
            write_image,
            pick_clips
        ]);
    let result = with_media_schemes(builder, media_root, thumbnail_root)
        .on_window_event(|window, event| match event {
            // Files dropped on the editor may be read from wherever they are: the user handed them over.
            // The editor hears of the drop from here as well as from the page's own listener, so a drop is
            // never lost to the page's side failing to register; the page ignores the repeat.
            tauri::WindowEvent::DragDrop(tauri::DragDropEvent::Drop { paths, .. }) => {
                media::allow_dropped(paths);
                if window.label() == "editor" {
                    let files: Vec<String> = paths.iter().map(|path| path.to_string_lossy().into_owned()).collect();
                    let _ = window.emit("editor-files", files);
                }
            }
            // Another monitor, or another scale on this one: the window and its minimum follow it.
            tauri::WindowEvent::Moved(_) | tauri::WindowEvent::ScaleFactorChanged { .. } if window.app_handle().try_state::<Arc<Shared>>().map(|s| s.launch.parent == 0).unwrap_or(true) => clamp_editor(
                window.label(),
                window.current_monitor().ok().flatten().as_ref(),
                window.inner_size().ok().map(|size| (size.width, size.height)),
                |width, height| {
                    let _ = window.set_min_size(Some(tauri::LogicalSize::new(width, height)));
                },
                |width, height| {
                    let _ = window.set_max_size(Some(tauri::LogicalSize::new(width, height)));
                },
                |width, height| {
                    let _ = window.set_size(tauri::LogicalSize::new(width, height));
                },
            ),
            // The process ends with its window, and takes its exports with it.
            tauri::WindowEvent::Destroyed => {
                pipe::write_log(&data_root(), &format!("shell: window {} was destroyed; the process ends", window.label()));
                if let Some(shared) = window.app_handle().try_state::<Arc<Shared>>() {
                    media::cancel_all(&shared.exports);
                    // Opening the editor stepped the panel aside: closing it brings the panel back, in its full form.
                    // A clip preview never hid the panel, and the Timeline preview is not an editor.
                    if window.label() == "editor" && shared.launch.view == "editor" && shared.launch.parent == 0 {
                        engine_once("ui.show", serde_json::json!({"view": "main"}));
                    }
                }
                window.app_handle().exit(0);
            }
            _ => {}
        })
        .setup(move |app| {
            let handle = app.handle().clone();
            // Files the editor was opened with are as much the user's choice as a drop.
            media::allow_dropped(&launch.files.iter().map(PathBuf::from).collect::<Vec<_>>());
            // The editor never starts the engine: it may run without one.
            let pipe = Arc::new(pipe::PipeClient::new(handle.clone(), data.clone(), engine_path(), false));
            app.manage(pipe.clone());
            pipe.start();
            // The editor opens inside the work area of the monitor it is on: at 200% its own size and
            // minimum are larger than a 1080p screen.
            let (start, min, max) = app
                .primary_monitor()
                .ok()
                .flatten()
                .map(|monitor| editor_sizes((monitor.work_area().size.width, monitor.work_area().size.height), monitor.scale_factor()))
                .unwrap_or((EDITOR_SIZE, EDITOR_MIN, EDITOR_MAX));
            let mut editor = WebviewWindowBuilder::new(app, "editor", WebviewUrl::App(format!("index.html{}", media::page_query()).into()))
                .title(if launch.view == "preview" { "BuffrX Preview" } else { "BuffrX Editor" })
                .inner_size(start.0, start.1)
                .maximizable(false)
                .decorations(false)
                .shadow(false)
                .theme(Some(tauri::Theme::Dark))
                // 4G: the window's own ground, the default theme's (Clutch, the page's first paint), so the frame never shows
                // WebView2's white before the page has painted. The editor and the preview are opaque; only the Timeline is not.
                .background_color(tauri::window::Color(11, 12, 13, 255))
                .initialization_script(&format!("{}window.__BUFFRX_EMBEDDED={};", view_script(&launch.view), launch.parent != 0));
            #[cfg(windows)]
            if launch.parent != 0 {
                editor = editor.parent_raw(windows::Win32::Foundation::HWND(launch.parent as *mut _)).visible(false);
            } else {
                editor = editor.min_inner_size(min.0, min.1).max_inner_size(max.0, max.1);
            }
            // A probe drives the page over the DevTools protocol instead of guessing at pixels: the port is opened
            // only when the variable is set, so a packaged BuffrX never listens for one.
            // 3.2: the browser features the page never shows (the Office and PDF bars, SmartScreen lookups) are off from the start,
            // with or without the debug port.
            #[cfg(windows)]
            {
                let mut browser_args = String::from("--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection");
                if let Ok(port) = std::env::var("BUFFRX_WEBVIEW_DEBUG_PORT") {
                    if !port.is_empty() {
                        browser_args.push_str(&format!(" --remote-debugging-port={port}"));
                    }
                }
                editor = editor.additional_browser_args(&browser_args);
            }
            let editor = editor.build()?;
            #[cfg(windows)]
            if let Ok(hwnd) = editor.hwnd() {
                square_window_corners(hwnd.0 as isize);
            }
            // The monitor the window actually landed on, which is not always the primary one.
            if launch.parent == 0 { clamp_editor(
                editor.label(),
                editor.current_monitor().ok().flatten().as_ref(),
                editor.inner_size().ok().map(|size| (size.width, size.height)),
                |width, height| {
                    let _ = editor.set_min_size(Some(tauri::LogicalSize::new(width, height)));
                },
                |width, height| {
                    let _ = editor.set_max_size(Some(tauri::LogicalSize::new(width, height)));
                },
                |width, height| {
                    let _ = editor.set_size(tauri::LogicalSize::new(width, height));
                },
            ); }
            // The page's text is the size of the engine's panel text. ImGui's "14 px" is the line height of the face (1.32 of
            // its em), so a CSS 14 px is 1.32 times bigger than the panel's; the WebView zoom of 0.91 brings the em to the
            // same 12.7 px. It is a zoom, so pointer positions and element rectangles stay in step and the trim and crop
            // handles still land.
            let _ = editor.set_zoom(0.91);
            Ok(())
        })
        .build(tauri::generate_context!());
    match result {
        Ok(app) => app.run(|_app, _event| {}),
        Err(e) => {
            pipe::write_log(&data_root(), &format!("editor could not start: {e}"));
            if embedded { std::process::exit(1); }
            // The engine hid its panel for this window, so the box is the only way the person learns why it never came.
            start_failed(
                "BuffrX",
                &format!(
                    "The editor did not open.\n\nRecording, clips, screenshots and the panel keep working.\n\nThe details are in this file:\n{}",
                    data_root().join("logs").join("engine.log").display()
                ),
            );
        }
    }
}

/// The Timeline preview (`--timeline <session>`): the engine's Alt+4 starts it when playable footage
/// exists. Its own process, so closing the window gives the player's memory back. It shows the 75% slide
/// window (timeline_window) with the preview page (ui/src/timeline), which reads `timeline.get` through
/// the pipe and ends the process when it closes.
fn run_timeline(launch: Launch) {
    if !acquire_timeline_instance() {
        // One preview runs. The engine already tracks that one; a second launch has nothing to do.
        return;
    }
    let data = data_root();
    media::start_http_server(data.clone());
    let media_root = data.clone();
    let thumbnail_root = data.clone();
    let shared = Arc::new(Shared { data_root: data.clone(), launch: launch.clone(), exports: Arc::new(media::Exports::default()) });
    let builder = tauri::Builder::default()
        .manage(shared.clone())
        .invoke_handler(tauri::generate_handler![engine_request, get_launch, timeline_log, timeline_exit, timeline_height, timeline_hide, timeline_ready, timeline_fade, timeline_leave, timeline_backdrop, preview_proxy, media_run, media_read_cancel, media_cancel]);
    let result = with_media_schemes(builder, media_root, thumbnail_root)
        .on_window_event(|window, event| {
            // The process ends with its window, and takes the player with it.
            if let tauri::WindowEvent::Destroyed = event {
                pipe::write_log(&data_root(), &format!("shell: window {} was destroyed; the process ends", window.label()));
                if let Some(shared) = window.app_handle().try_state::<Arc<Shared>>() { media::cancel_all(&shared.exports); }
                window.app_handle().exit(0);
            }
        })
        .setup(move |app| {
            let handle = app.handle().clone();
            // The preview never starts the engine: it was started by it.
            let pipe = Arc::new(pipe::PipeClient::new(handle, data.clone(), engine_path(), false));
            app.manage(pipe.clone());
            pipe.start();
            let mut preview = WebviewWindowBuilder::new(app, "timeline", WebviewUrl::App(format!("timeline.html{}", media::page_query()).into()))
                .title("BuffrX Timeline")
                .inner_size(1440.0, 900.0)
                // The slide preview: no frame, no shadow (an undecorated window keeps 8 white pixels around the page),
                // always above the game, out of the taskbar. Created hidden; timeline_window shows it where it belongs.
                .decorations(false)
                .shadow(false)
                .resizable(false)
                .always_on_top(true)
                .skip_taskbar(true)
                .focused(false)
                .theme(Some(tauri::Theme::Dark))
                // Transparent: the page paints a blurred copy of the screen behind it (timeline_backdrop).
                .transparent(true)
                .visible(false);
            // Same dev-only door as the editor's: a probe drives this page over the DevTools protocol, and a
            // packaged BuffrX never listens for one.
            #[cfg(windows)]
            if let Ok(port) = std::env::var("BUFFRX_WEBVIEW_DEBUG_PORT") {
                if !port.is_empty() {
                    preview = preview.additional_browser_args(&format!(
                        "--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection --remote-debugging-port={port}"
                    ));
                }
            }
            let window = preview.build()?;
            #[cfg(windows)]
            if let Ok(hwnd) = window.hwnd() {
                // Three quarters of the monitor under the game, dropped onto the top of it: the frame removed, the
                // window shown, the slide, then the foreground, so typing in it reaches the preview.
                timeline_window::apply_preview_window(hwnd.0 as isize);
            }
            Ok(())
        })
        .build(tauri::generate_context!());
    match result {
        Ok(app) => app.run(|_app, _event| {}),
        Err(e) => {
            pipe::write_log(&data_root(), &format!("timeline preview could not start: {e}"));
            start_failed(
                "BuffrX",
                &format!(
                    "The Timeline preview did not open.\n\nFootage and recording keep working.\n\nThe details are in this file:\n{}",
                    data_root().join("logs").join("engine.log").display()
                ),
            );
        }
    }
}

/// A line from the Timeline page for ui.log: how far it got when something did not appear.
#[tauri::command]
fn timeline_log(message: String) {
    pipe::write_log(&data_root(), &format!("timeline page: {}", message.chars().take(300).collect::<String>()));
}

/// Hide video: the window shrinks to the page's top `height` CSS pixels, so the game shows under the
/// strip; 0 gives the whole work area back.
#[tauri::command]
fn timeline_height(window: tauri::WebviewWindow, height: f64) {
    #[cfg(windows)]
    if let Ok(hwnd) = window.hwnd() {
        let scale = window.scale_factor().unwrap_or(1.0);
        let physical = if height > 0.0 { Some((height * scale).ceil() as i32) } else { None };
        timeline_window::set_preview_height(hwnd.0 as isize, physical);
    }
    #[cfg(not(windows))]
    let _ = (window, height);
}

/// The screen behind the preview, small, for the page to blur (Steam's overlay look). None off Windows or on failure.
#[tauri::command]
async fn timeline_backdrop() -> Option<String> {
    #[cfg(windows)]
    return timeline_window::backdrop();
    #[cfg(not(windows))]
    None
}

/// The page has its first frame ready: the window is shown now (it was created hidden so no unpainted frame ever reaches the screen).
#[tauri::command]
async fn timeline_ready(window: tauri::WebviewWindow) {
    #[cfg(windows)]
    if let Ok(hwnd) = window.hwnd() {
        timeline_window::reveal(hwnd.0 as isize);
    }
    #[cfg(not(windows))]
    let _ = window;
}

/// The page's arrival has started: the window fades in (it was shown at alpha 0).
#[tauri::command]
fn timeline_fade(window: tauri::WebviewWindow) {
    #[cfg(windows)]
    if let Ok(hwnd) = window.hwnd() {
        timeline_window::fade_in(hwnd.0 as isize);
    }
    #[cfg(not(windows))]
    let _ = window;
}

/// The page is on its way out: the window fades over 90 ms while the page lets go of its work.
#[tauri::command]
fn timeline_leave(window: tauri::WebviewWindow) {
    #[cfg(windows)]
    if let Ok(hwnd) = window.hwnd() {
        timeline_window::fade_out(hwnd.0 as isize);
    }
    #[cfg(not(windows))]
    let _ = window;
}

/// Save and back to game: the window goes at once while the page finishes the save behind it.
#[tauri::command]
fn timeline_hide(window: tauri::WebviewWindow) {
    let _ = window.hide();
}

/// The Timeline preview's close button: the process ends, and its window with it.
#[tauri::command]
fn timeline_exit(app: AppHandle) {
    pipe::write_log(&data_root(), "shell: the page asked the Timeline to exit; the process ends");
    if let Some(shared) = app.try_state::<Arc<Shared>>() { media::cancel_all(&shared.exports); }
    #[cfg(windows)]
    timeline_window::finish_leave();
    app.exit(0);
}

#[tauri::command]
async fn engine_request(frame: String, pipe: tauri::State<'_, Arc<pipe::PipeClient>>) -> Result<String, String> {
    pipe.send(frame).await
}

#[tauri::command]
fn get_launch(shared: tauri::State<'_, Arc<Shared>>) -> serde_json::Value {
    let launch = &shared.launch;
    let role = if launch.timeline { "timeline" } else { "editor" };
    serde_json::json!({"role": role, "view": launch.view, "files": launch.files,
        "session": launch.timeline_session})
}

#[tauri::command]
fn editor_ready(window: tauri::WebviewWindow, shared: tauri::State<'_, Arc<Shared>>) {
    if shared.launch.parent != 0 { let _ = window.show(); }
}

#[tauri::command]
fn editor_leaving(window: tauri::WebviewWindow, shared: tauri::State<'_, Arc<Shared>>) {
    #[cfg(windows)]
    if shared.launch.parent != 0 {
        extern "system" { fn PostMessageW(hwnd: isize, msg: u32, wp: usize, lp: isize) -> i32; }
        let _ = window.hide();
        unsafe { PostMessageW(shared.launch.parent, 0x8000 + 0x425, 0, 0); }
    }
}

#[tauri::command]
fn parent_drag(shared: tauri::State<'_, Arc<Shared>>) {
    #[cfg(windows)]
    if shared.launch.parent != 0 {
        extern "system" { fn ReleaseCapture() -> i32; fn PostMessageW(hwnd: isize, msg: u32, wp: usize, lp: isize) -> i32; }
        unsafe { ReleaseCapture(); PostMessageW(shared.launch.parent, 0x00a1, 2, 0); }
    }
}

#[tauri::command]
fn parent_minimise(shared: tauri::State<'_, Arc<Shared>>) {
    #[cfg(windows)]
    if shared.launch.parent != 0 {
        extern "system" { fn ShowWindow(hwnd: isize, command: i32) -> i32; }
        unsafe { ShowWindow(shared.launch.parent, 6); }
    }
}

#[tauri::command]
fn preview_fullscreen(window: tauri::WebviewWindow, on: bool) -> bool {
    if on {
        PREVIEW_FULLSCREEN.store(true, Ordering::SeqCst);
        let _ = window.set_max_size(None::<tauri::LogicalSize<f64>>);
        let _ = window.set_fullscreen(true);
    } else {
        let _ = window.set_fullscreen(false);
        if let Ok(Some(monitor)) = window.current_monitor() {
            let (_, min, max) = editor_sizes((monitor.work_area().size.width, monitor.work_area().size.height), monitor.scale_factor());
            let _ = window.set_min_size(Some(tauri::LogicalSize::new(min.0, min.1)));
            let _ = window.set_max_size(Some(tauri::LogicalSize::new(max.0, max.1)));
        }
        if let Some(known) = EDITOR_MIN_SET.get() {
            *known.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) = None;
        }
        PREVIEW_FULLSCREEN.store(false, Ordering::SeqCst);
    }
    window.is_fullscreen().unwrap_or(on)
}

#[tauri::command]
fn preview_fullscreen_state(window: tauri::WebviewWindow) -> bool {
    window.is_fullscreen().unwrap_or(false)
}

#[tauri::command]
fn preview_proxy(
    app: AppHandle,
    input: String,
    width: u32,
    height: u32,
    sar: f64,
    shared: tauri::State<'_, Arc<Shared>>,
) -> Result<serde_json::Value, String> {
    media::preview_proxy(app, &shared.data_root, shared.exports.clone(), &input, width, height, sar)
}

#[tauri::command]
fn get_data_root(shared: tauri::State<'_, Arc<Shared>>) -> String {
    shared.data_root.to_string_lossy().to_string()
}

#[tauri::command]
async fn media_run(args: Vec<String>, id: Option<String>, shared: tauri::State<'_, Arc<Shared>>) -> Result<String, String> {
    let root = shared.data_root.clone();
    let exports = shared.exports.clone();
    tauri::async_runtime::spawn_blocking(move || media::run(&root, &exports, args, id)).await.map_err(|e| e.to_string())?
}

/// Ends one of the page's media reads by the name it was given: the editor kills its process, so a
/// peaks walk of a clip nobody is looking at does not hold the place the clip on screen needs.
#[tauri::command]
fn media_read_cancel(id: String, shared: tauri::State<'_, Arc<Shared>>) {
    media::cancel_read(&shared.exports, &id);
}

#[tauri::command]
async fn timeline_merge(files: Vec<String>, shared: tauri::State<'_, Arc<Shared>>) -> Result<String, String> {
    let root = shared.data_root.clone();
    let exports = shared.exports.clone();
    tauri::async_runtime::spawn_blocking(move || media::merge_timeline(&root, &exports, files))
        .await.map_err(|e| e.to_string())?
}

#[tauri::command]
fn media_export(app: AppHandle, job: serde_json::Value, shared: tauri::State<'_, Arc<Shared>>) -> Result<u64, String> {
    media::start_export(app, &shared.data_root, shared.exports.clone(), job)
}

#[tauri::command]
fn unique_path(path: String, shared: tauri::State<'_, Arc<Shared>>) -> Result<String, String> {
    media::unique_path(&shared.data_root, std::path::Path::new(&path)).map(|p| p.to_string_lossy().to_string())
}

#[tauri::command]
fn media_cancel(id: u64, shared: tauri::State<'_, Arc<Shared>>) {
    media::cancel_export(&shared.exports, id);
}

/// The native multi-file picker: the OS's own open dialog, many files at once, videos first. The files
/// are added to the readable set, as a drop's are, so the page can play them at once.
#[tauri::command]
async fn pick_clips() -> Result<Vec<String>, String> {
    #[cfg(windows)]
    {
        let paths = tauri::async_runtime::spawn_blocking(open_clips_dialog)
            .await
            .map_err(|e| e.to_string())??;
        let list: Vec<PathBuf> = paths.iter().map(PathBuf::from).collect();
        media::allow_dropped(&list);
        Ok(paths)
    }
    #[cfg(not(windows))]
    {
        let output = std::process::Command::new("zenity")
            .args(["--file-selection", "--multiple", "--separator=\n", "--title=Choose files"])
            .output()
            .map_err(|e| format!("the file picker could not open: {e}"))?;
        if !output.status.success() {
            return Ok(Vec::new());
        }
        let text = String::from_utf8_lossy(&output.stdout);
        let paths: Vec<String> = text.lines().map(str::trim).filter(|line| !line.is_empty()).map(str::to_string).collect();
        let list: Vec<PathBuf> = paths.iter().map(|p| media::native(p)).collect();
        media::allow_dropped(&list);
        Ok(paths)
    }
}

/// `OPENFILENAMEW`, in the order the SDK declares it: `repr(C)` gives it the same layout.
#[cfg(windows)]
#[repr(C)]
struct OpenFileNameW {
    l_struct_size: u32,
    hwnd_owner: *mut core::ffi::c_void,
    h_instance: *mut core::ffi::c_void,
    lpstr_filter: *const u16,
    lpstr_custom_filter: *mut u16,
    n_max_cust_filter: u32,
    n_filter_index: u32,
    lpstr_file: *mut u16,
    n_max_file: u32,
    lpstr_file_title: *mut u16,
    n_max_file_title: u32,
    lpstr_initial_dir: *const u16,
    lpstr_title: *const u16,
    flags: u32,
    n_file_offset: u16,
    n_file_extension: u16,
    lpstr_def_ext: *const u16,
    l_cust_data: isize,
    lpfn_hook: *mut core::ffi::c_void,
    lp_template_name: *const u16,
    pv_reserved: *mut core::ffi::c_void,
    dw_reserved: u32,
    flags_ex: u32,
}

/// The open dialog itself, on its own thread with its own message loop, as the dialog requires.
#[cfg(windows)]
fn open_clips_dialog() -> Result<Vec<String>, String> {
    #[link(name = "comdlg32")]
    extern "system" {
        fn GetOpenFileNameW(ofn: *mut OpenFileNameW) -> i32;
        fn CommDlgExtendedError() -> u32;
    }
    #[link(name = "user32")]
    extern "system" {
        fn GetForegroundWindow() -> isize;
    }
    const OFN_ALLOWMULTISELECT: u32 = 0x0000_0200;
    const OFN_EXPLORER: u32 = 0x0008_0000;
    const OFN_FILEMUSTEXIST: u32 = 0x0000_1000;
    const OFN_PATHMUSTEXIST: u32 = 0x0000_0800;
    const OFN_NOCHANGEDIR: u32 = 0x0000_0008;
    const OFN_HIDEREADONLY: u32 = 0x0000_0004;
    // Videos first, because that is what the editor joins; "All files" is there for a container the
    // filter forgot, and the page decides what it can read.
    let filter: Vec<u16> = "Videos and pictures\0*.mp4;*.mkv;*.mov;*.webm;*.m4v;*.avi;*.png;*.jpg;*.jpeg\0All files\0*.*\0\0"
        .encode_utf16()
        .collect();
    let title: Vec<u16> = "Choose files\0".encode_utf16().collect();
    // This button is for files that are not in BuffrX's library, which the editor already lists: start in the
    // user's Videos folder, not wherever the last dialog left off (usually the library's own folder).
    let start_dir: Option<Vec<u16>> = env::var_os("USERPROFILE")
        .map(|home| PathBuf::from(home).join("Videos"))
        .filter(|dir| dir.is_dir())
        .map(|dir| dir.to_string_lossy().encode_utf16().chain(std::iter::once(0)).collect());
    // The dialog writes a folder and then one name a file, so the buffer holds many paths.
    let mut buffer = vec![0u16; 65_536];
    let mut ofn = OpenFileNameW {
        l_struct_size: std::mem::size_of::<OpenFileNameW>() as u32,
        hwnd_owner: unsafe { GetForegroundWindow() } as *mut core::ffi::c_void,
        h_instance: std::ptr::null_mut(),
        lpstr_filter: filter.as_ptr(),
        lpstr_custom_filter: std::ptr::null_mut(),
        n_max_cust_filter: 0,
        n_filter_index: 1,
        lpstr_file: buffer.as_mut_ptr(),
        n_max_file: buffer.len() as u32,
        lpstr_file_title: std::ptr::null_mut(),
        n_max_file_title: 0,
        lpstr_initial_dir: start_dir.as_ref().map_or(std::ptr::null(), |dir| dir.as_ptr()),
        lpstr_title: title.as_ptr(),
        flags: OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR
            | OFN_HIDEREADONLY,
        n_file_offset: 0,
        n_file_extension: 0,
        lpstr_def_ext: std::ptr::null(),
        l_cust_data: 0,
        lpfn_hook: std::ptr::null_mut(),
        lp_template_name: std::ptr::null(),
        pv_reserved: std::ptr::null_mut(),
        dw_reserved: 0,
        flags_ex: 0,
    };
    let picked = unsafe { GetOpenFileNameW(&mut ofn) };
    if picked == 0 {
        let code = unsafe { CommDlgExtendedError() };
        // Cancelled: no error and no files. Anything else is the dialog failing, said out loud.
        return if code == 0 { Ok(Vec::new()) } else { Err(format!("the file picker failed ({code})")) };
    }
    // One file is its whole path; several are a folder followed by one name each.
    let mut parts: Vec<String> = Vec::new();
    let mut start = 0;
    for i in 0..buffer.len() {
        if buffer[i] != 0 {
            continue;
        }
        if i == start {
            break;
        }
        parts.push(String::from_utf16_lossy(&buffer[start..i]));
        start = i + 1;
    }
    if parts.len() <= 1 {
        return Ok(parts);
    }
    let folder = parts.remove(0);
    Ok(parts.iter().map(|name| format!("{folder}\\{name}")).collect())
}

/// Strict base64 for the screenshot editor's picture: one string, not one JSON number per byte.
fn decode_base64(text: &str) -> Result<Vec<u8>, String> {
    let mut out = Vec::with_capacity(text.len() * 3 / 4);
    let mut buffer = 0u32;
    let mut bits = 0u32;
    for byte in text.bytes() {
        let value = match byte {
            b'A'..=b'Z' => byte - b'A',
            b'a'..=b'z' => byte - b'a' + 26,
            b'0'..=b'9' => byte - b'0' + 52,
            b'+' => 62,
            b'/' => 63,
            b'=' => break,
            b'\r' | b'\n' => continue,
            _ => return Err("the image data is not valid base64".into()),
        };
        buffer = (buffer << 6) | value as u32;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((buffer >> bits) as u8);
        }
    }
    Ok(out)
}

/// Writes an edited image (PNG or JPEG) into the allowed folders only.
#[tauri::command]
fn write_image(path: String, bytes_base64: String, shared: tauri::State<'_, Arc<Shared>>) -> Result<(), String> {
    // A page-chosen image has no business being unbounded: the editor encodes a 4K/8K picture, but a
    // runaway payload must not allocate twice its size on the IPC thread.
    const MAX_IMAGE_BYTES: usize = 128 * 1024 * 1024;
    if bytes_base64.len() > MAX_IMAGE_BYTES * 4 / 3 + 8 {
        return Err("the image is too large to write".into());
    }
    let bytes = decode_base64(&bytes_base64)?;
    let native = media::native(&path);
    let target = native.as_path();
    let image = matches!(
        target.extension().and_then(|e| e.to_str()).map(|e| e.to_ascii_lowercase()).as_deref(),
        Some("png") | Some("jpg") | Some("jpeg")
    );
    if !image || !media::is_allowed(&shared.data_root, target) {
        return Err("images can only be written inside the BuffrX folders".into());
    }
    if let Some(parent) = target.parent() {
        let _ = fs::create_dir_all(parent);
    }
    // Written beside the target and moved over it, as the export is: "Replace" writes onto the
    // original, and a save that is closed half way used to leave it truncated.
    let staged = target.with_extension(format!("{}.part", target.extension().and_then(|e| e.to_str()).unwrap_or("png")));
    fs::write(&staged, &bytes).map_err(|e| e.to_string())?;
    media::move_over(&staged, target).map_err(|e| {
        let _ = fs::remove_file(&staged);
        e.to_string()
    })
}

/// A thumbnail by name, confined to the engine's thumbnails folder.
fn resolve_thumbnail(data: &std::path::Path, name: &str) -> Option<PathBuf> {
    if name.is_empty() || name.contains('/') || name.contains('\\') || name.contains(':') || name.contains("..") {
        return None;
    }
    if std::path::Path::new(name).components().count() != 1 {
        return None;
    }
    let path = data.join("thumbnails").join(name);
    path.exists().then_some(path)
}

/// Windows 11 rounds every window's corners; BuffrX is square, and the preview's accent line has to
/// meet the window edge without a second rounded border under it. No-op before Windows 11.
#[cfg(windows)]
fn square_window_corners(hwnd: isize) {
    #[link(name = "dwmapi")]
    extern "system" {
        fn DwmSetWindowAttribute(hwnd: isize, attribute: u32, value: *const core::ffi::c_void, size: u32) -> i32;
    }
    const DWMWA_WINDOW_CORNER_PREFERENCE: u32 = 33;
    const DWMWCP_DONOTROUND: i32 = 1;
    unsafe {
        DwmSetWindowAttribute(
            hwnd,
            DWMWA_WINDOW_CORNER_PREFERENCE,
            &DWMWCP_DONOTROUND as *const _ as *const core::ffi::c_void,
            std::mem::size_of::<i32>() as u32,
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn launch_args(list: &[&str]) -> Vec<String> {
        std::iter::once("buffrx-ui.exe").chain(list.iter().copied()).map(str::to_string).collect()
    }

    #[test]
    fn embedded_parent_is_not_treated_as_a_file() {
        let launch = parse_launch(&launch_args(&["--editor", "--parent", "12345", "clip.mp4"]));
        assert_eq!(launch.parent, 12345);
        assert_eq!(launch.files, vec!["clip.mp4"]);
    }

    #[test]
    fn parse_launch_reads_a_timeline_session() {
        let launch = parse_launch(&launch_args(&["--timeline", "abc"]));
        assert!(launch.timeline, "a --timeline launch is the Timeline preview");
        assert_eq!(launch.timeline_session, "abc");
        assert!(!launch.editor, "it is not the editor");
    }

    #[test]
    fn a_timeline_launch_without_a_session_opens_the_empty_state() {
        let launch = parse_launch(&launch_args(&["--timeline"]));
        assert!(launch.timeline, "the preview still starts");
        assert!(launch.timeline_session.is_empty(), "and has no session to show");
    }

    #[test]
    fn the_editor_opens_inside_the_monitor_it_is_on() {
        // A big desktop at 100%: its own size, and its own minimum and maximum.
        assert_eq!(editor_sizes((2560, 1400), 1.0), (EDITOR_SIZE, EDITOR_MIN, EDITOR_MAX));
        // 1080p at 100%: the work area is 1920x1040, shorter than the editor's own height, so every height is clamped.
        assert_eq!(editor_sizes((1920, 1040), 1.0), ((1555.2, 1040.0), (1270.08, 816.48), (1920.0, 1040.0)));
        // 1080p at 200%: the work area is 960x520 logical; the minimum is clamped too, or
        // the window could not shrink back inside the screen.
        assert_eq!(editor_sizes((1920, 1040), 2.0), ((960.0, 520.0), (960.0, 520.0), (960.0, 520.0)));
        // 4K at 100% is 3840x2080: the editor's own size.
        assert_eq!(editor_sizes((3840, 2080), 1.0), (EDITOR_SIZE, EDITOR_MIN, EDITOR_MAX));
        // A small screen: the editor fits it exactly instead of hanging past its edges.
        assert_eq!(editor_sizes((1024, 560), 1.0), ((1024.0, 560.0), (1024.0, 560.0), (1024.0, 560.0)));
    }

    #[test]
    fn a_scale_factor_of_zero_does_not_divide_by_it() {
        assert_eq!(editor_sizes((1920, 1040), 0.0), editor_sizes((1920, 1040), 1.0));
        assert_eq!(editor_sizes((1920, 1040), f64::NAN), editor_sizes((1920, 1040), 1.0));
    }
}
