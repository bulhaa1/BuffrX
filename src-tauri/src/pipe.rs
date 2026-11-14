use std::path::{Path, PathBuf};
use serde_json::Value;
use std::collections::HashMap;
use std::sync::Arc;
use tokio::io::{AsyncReadExt, AsyncWriteExt, WriteHalf};
#[cfg(unix)]
use tokio::net::UnixStream;
#[cfg(windows)]
use tokio::net::windows::named_pipe::ClientOptions;

#[cfg(unix)]
type EngineStream = UnixStream;
#[cfg(windows)]
type EngineStream = tokio::net::windows::named_pipe::NamedPipeClient;

#[cfg(unix)]
async fn connect_engine() -> std::io::Result<EngineStream> {
    UnixStream::connect(pipe_name()).await
}
#[cfg(windows)]
async fn connect_engine() -> std::io::Result<EngineStream> {
    ClientOptions::new().open(pipe_name())
}
use tokio::sync::{Mutex, oneshot};
use tauri::{AppHandle, Emitter, Manager};

/// The engine's socket: the same per-user runtime path the engine's POSIX pipe server uses.
#[cfg(unix)]
pub(crate) fn pipe_name() -> String {
    extern "C" {
        fn getuid() -> u32;
    }
    let leaf = match std::env::var("BUFFRX_PIPE") {
        Ok(name) if !name.is_empty() => name,
        _ => "buffrx".to_string(),
    };
    let leaf: String = leaf
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() || c == '-' || c == '_' || c == '.' { c } else { '_' })
        .collect();
    let base = std::env::var("XDG_RUNTIME_DIR").unwrap_or_else(|_| "/tmp".to_string());
    let uid = unsafe { getuid() };
    format!("{base}/buffrx-{uid}-{leaf}.sock")
}

#[cfg(windows)]
pub(crate) fn pipe_name() -> String {
    match std::env::var("BUFFRX_PIPE") {
        Ok(name) if !name.is_empty() => format!(r"\\.\pipe\{name}"),
        _ => r"\\.\pipe\buffrx".to_string(),
    }
}

/// One line into ui.log with an engine.log-style timestamp. The shell only logs failures and
/// engine lifetime events; this is the one record of why the engine went away.
pub(crate) fn write_log(data_root: &Path, msg: &str) {
    use std::io::Write;
    let dir = data_root.join("logs");
    let _ = std::fs::create_dir_all(&dir);
    let path = dir.join("ui.log");
    // Failures only, and bounded: a log past 1 MiB is dropped on the next write's open.
    if let Ok(meta) = std::fs::metadata(&path) {
        if meta.len() > 1024 * 1024 {
            let _ = std::fs::remove_file(&path);
        }
    }
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(&path) {
        let _ = writeln!(f, "[{}] {}", timestamp(), msg);
    }
}

/// Local time, second resolution, matching engine.log: SystemTimeToTzSpecificLocalTime does the
/// timezone work, so no calendar or tz crate is pulled in.
#[cfg(windows)]
fn timestamp() -> String {
    #[repr(C)]
    struct SystemTime {
        year: u16,
        month: u16,
        day_of_week: u16,
        day: u16,
        hour: u16,
        minute: u16,
        second: u16,
        milliseconds: u32,
    }
    extern "system" {
        fn SystemTimeToTzSpecificLocalTime(time_zone: *const std::ffi::c_void, utc: *const SystemTime, local: *mut SystemTime) -> i32;
    }
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    // FILETIME is 100 ns units since 1601-01-01.
    let file_time = (now + 11_644_473_600) * 10_000_000;
    let mut utc = SystemTime {
        year: 0,
        month: 0,
        day_of_week: 0,
        day: 0,
        hour: 0,
        minute: 0,
        second: 0,
        milliseconds: 0,
    };
    // Splitting the 64-bit value is what FileTimeToSystemTime does; do it inline to avoid the FFI
    // type dance for a second call.
    let (high, low) = ((file_time >> 32) as u32, file_time as u32);
    unsafe {
        extern "system" {
            fn FileTimeToSystemTime(ft: *const u32, st: *mut SystemTime) -> i32;
        }
        let ft = [low, high];
        if FileTimeToSystemTime(ft.as_ptr(), &mut utc) == 0 {
            return String::new();
        }
    }
    let mut local = SystemTime {
        year: 0,
        month: 0,
        day_of_week: 0,
        day: 0,
        hour: 0,
        minute: 0,
        second: 0,
        milliseconds: 0,
    };
    if unsafe { SystemTimeToTzSpecificLocalTime(std::ptr::null(), &utc, &mut local) } == 0 {
        return String::new();
    }
    format!(
        "{:04}-{:02}-{:02} {:02}:{:02}:{:02}",
        local.year, local.month, local.day, local.hour, local.minute, local.second
    )
}

#[cfg(unix)]
fn timestamp() -> String {
    #[repr(C)]
    struct Tm {
        sec: i32,
        min: i32,
        hour: i32,
        mday: i32,
        mon: i32,
        year: i32,
        wday: i32,
        yday: i32,
        isdst: i32,
        gmtoff: i64,
        zone: *const std::os::raw::c_char,
    }
    extern "C" {
        fn localtime_r(timep: *const i64, result: *mut Tm) -> *mut Tm;
    }
    let seconds = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0);
    let mut tm = Tm {
        sec: 0, min: 0, hour: 0, mday: 0, mon: 0, year: 0, wday: 0, yday: 0, isdst: 0, gmtoff: 0,
        zone: std::ptr::null(),
    };
    if unsafe { localtime_r(&seconds, &mut tm) }.is_null() {
        return String::new();
    }
    format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", tm.year + 1900, tm.mon + 1, tm.mday, tm.hour, tm.min, tm.sec)
}

pub struct PipeClient {
    app: AppHandle,
    data_root: PathBuf,
    engine_path: PathBuf,
    may_spawn: bool,
    supervisor: Arc<Mutex<Supervisor>>,
    pending: Arc<Mutex<HashMap<i64, Pending>>>,
    next_id: Arc<Mutex<i64>>,
    writer: Arc<Mutex<Option<WriteHalf<EngineStream>>>>,
    connected: Arc<Mutex<bool>>,
}

struct Pending {
    tx: oneshot::Sender<String>,
}

/// Engine restart bookkeeping: how many times the pipe was missing in a row, and when the last
/// spawn went out. One successful connection resets both.
struct Supervisor {
    attempts: u32,
    last_attempt: Option<std::time::Instant>,
}

impl Default for Supervisor {
    fn default() -> Self {
        Self { attempts: 0, last_attempt: None }
    }
}

impl PipeClient {
    pub fn new(app: AppHandle, data_root: PathBuf, engine_path: PathBuf, may_spawn: bool) -> Self {
        Self {
            app,
            data_root,
            engine_path,
            may_spawn,
            supervisor: Arc::new(Mutex::new(Supervisor::default())),
            pending: Arc::new(Mutex::new(HashMap::new())),
            next_id: Arc::new(Mutex::new(1)),
            writer: Arc::new(Mutex::new(None)),
            connected: Arc::new(Mutex::new(false)),
        }
    }

    /// Spawns the engine and arms a watcher that records its exit code. Called with backoff while
    /// the pipe is gone: the first miss spawns immediately, a crash loop backs off 1s..30s.
    fn spawn_engine(&self) {
        if !self.may_spawn || !self.engine_path.exists() {
            return;
        }
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            const CREATE_NO_WINDOW: u32 = 0x08000000;
            match std::process::Command::new(&self.engine_path)
                .arg("--minimized")
                .creation_flags(CREATE_NO_WINDOW)
                .spawn()
            {
                Ok(mut child) => {
                    let data_root = self.data_root.clone();
                    std::thread::spawn(move || match child.wait() {
                        Ok(status) => {
                            let code = status
                                .code()
                                .map(|c| c.to_string())
                                .unwrap_or_else(|| "<no code>".to_string());
                            write_log(&data_root, &format!("engine exited: code={code}"));
                        }
                        Err(e) => write_log(&data_root, &format!("engine exit unreadable: {e}")),
                    });
                }
                Err(e) => write_log(&self.data_root, &format!("engine spawn failed: {e}")),
            }
        }
        #[cfg(not(windows))]
        {
            let _ = std::process::Command::new(&self.engine_path).arg("--minimized").spawn();
        }
    }

    /// The backoff decision: spawn only when the current attempt is due. First miss spawns
    /// immediately; a crash loop backs off 1s, 2s, 4s, 8s, 16s, then caps at 30s. Attempts reset on
    /// any successful connection, so a healthy session never accumulates delay.
    async fn maybe_spawn_engine(&self) {
        if !self.may_spawn {
            return;
        }
        const SCHEDULE_MS: [u64; 7] = [0, 1000, 2000, 4000, 8000, 16000, 30000];
        let mut supervisor = self.supervisor.lock().await;
        let wait_ms = SCHEDULE_MS[(supervisor.attempts as usize).min(SCHEDULE_MS.len() - 1)];
        let due = match supervisor.last_attempt {
            None => true,
            Some(at) => at.elapsed().as_millis() as u64 >= wait_ms,
        };
        if due {
            supervisor.attempts = supervisor.attempts.saturating_add(1);
            supervisor.last_attempt = Some(std::time::Instant::now());
            drop(supervisor);
            self.spawn_engine();
        }
    }

    pub fn start(self: Arc<Self>) {
        let client = self.clone();
        tauri::async_runtime::spawn(async move {
            loop {
                if !*client.connected.lock().await {
                    match connect_engine().await {
                        Ok(pipe) => {
                            {
                                let mut supervisor = client.supervisor.lock().await;
                                supervisor.attempts = 0;
                                supervisor.last_attempt = None;
                            }
                            let (mut reader, mut writer) = tokio::io::split(pipe);
                            // The engine routes editor.open by the role a client names in hello. Without
                            // this every Edit click would start a second editor instead of sending the
                            // files to the one already open.
                            let hello = serde_json::json!({"id": 0, "cmd": "hello", "args": {"client": "editor"}});
                            let bytes = hello.to_string().into_bytes();
                            let _ = writer.write_all(&(bytes.len() as u32).to_le_bytes()).await;
                            let _ = writer.write_all(&bytes).await;
                            let _ = writer.flush().await;
                            *client.writer.lock().await = Some(writer);
                            *client.connected.lock().await = true;
                            let _ = client.app.emit("engine_status", "connected");
                            loop {
                                let mut header = [0u8; 4];
                                match reader.read_exact(&mut header).await {
                                    Ok(_) => {}
                                    Err(_) => break,
                                }
                                let len = u32::from_le_bytes(header) as usize;
                                if len == 0 || len > 16 * 1024 * 1024 {
                                    break;
                                }
                                let mut payload = vec![0u8; len];
                                if reader.read_exact(&mut payload).await.is_err() {
                                    break;
                                }
                                let text = String::from_utf8_lossy(&payload).to_string();
                                client.handle_frame(text).await;
                            }
                            *client.connected.lock().await = false;
                            *client.writer.lock().await = None;
                            client.log("engine connection closed; the window goes with it");
                            let _ = client.app.emit("engine_status", "disconnected");
                            let mut pending = client.pending.lock().await;
                            for (_, p) in pending.drain() {
                                let _ = p.tx.send(serde_json::json!({"ok": false, "error": "the connection to the engine was lost"}).to_string());
                            }
                            drop(pending);
                            // The engine and this window are one product: when its pipe closes, this
                            // process ends. Watching for `app.quit` alone was not enough - the server
                            // flushes before it disconnects, which throws a queued frame away, so the
                            // editor and the Timeline preview outlived every quit that raced it.
                            if let Some(shared) = client.app.try_state::<std::sync::Arc<crate::Shared>>() {
                                crate::media::cancel_all(&shared.exports);
                            }
                            client.app.exit(0);
                        }
                        Err(_) => {
                            client.maybe_spawn_engine().await;
                            tokio::time::sleep(std::time::Duration::from_millis(500)).await;
                        }
                    }
                } else {
                    tokio::time::sleep(std::time::Duration::from_millis(100)).await;
                }
            }
        });
    }

    async fn handle_frame(&self, text: String) {
        let parsed: Value = match serde_json::from_str(&text) {
            Ok(v) => v,
            Err(_) => return,
        };
        let obj = match parsed.as_object() {
            Some(o) => o,
            None => return,
        };
        // A response resolves its pending request (the invoke returns it); it is not an event.
        if let Some(engine_id) = obj.get("id").and_then(|v| v.as_i64()) {
            let mut pending = self.pending.lock().await;
            if let Some(p) = pending.remove(&engine_id) {
                let _ = p.tx.send(text.clone());
            }
            return;
        }
        // Window events the shell acts on itself (the tray's quick panel, the editor's files, quit),
        // handled on their own thread.
        if let Some(name) = obj.get("event").and_then(|v| v.as_str()) {
            if matches!(name, "ui.show" | "ui.toggle" | "editor.files" | "app.quit" | "timeline.close") {
                let app = self.app.clone();
                let name = name.to_string();
                let data = obj.get("data").cloned().unwrap_or(Value::Null);
                // Off the event loop: building a window from inside a main-thread callback deadlocks on
                // Windows (the builder waits for the loop it is blocking). Tauri dispatches it itself.
                std::thread::spawn(move || crate::handle_engine_event(&app, &name, &data));
            }
        }
        let _ = self.app.emit("engine_event", text);
    }

    pub fn log(&self, msg: &str) {
        write_log(&self.data_root, msg);
    }

    pub async fn send(&self, frame: String) -> Result<String, String> {
        let parsed: Value = serde_json::from_str(&frame).map_err(|e| format!("bad json: {e}"))?;
        let obj = parsed.as_object().ok_or("not an object")?;
        let cmd = obj.get("cmd").and_then(|v| v.as_str()).unwrap_or("?").to_string();
        let page_id = obj.get("id").map(|v| {
            if let Some(s) = v.as_str() { s.to_string() } else { v.to_string() }
        }).unwrap_or_default();

        let engine_id = {
            let mut n = self.next_id.lock().await;
            let id = *n;
            *n += 1;
            id
        };

        let mut outgoing = parsed.clone();
        if let Some(o) = outgoing.as_object_mut() {
            o.insert("id".to_string(), Value::Number(engine_id.into()));
        }
        let outgoing_text = outgoing.to_string();

        let (tx, rx) = oneshot::channel();
        {
            let mut pending = self.pending.lock().await;
            pending.insert(engine_id, Pending { tx });
        }

        {
            // The engine may still be starting when the page fires its first request: wait for the
            // pipe instead of failing the request the moment the window appears.
            let mut connected = false;
            for _ in 0..50 {
                if *self.connected.lock().await { connected = true; break; }
                tokio::time::sleep(std::time::Duration::from_millis(100)).await;
            }
            if !connected {
                let mut pending = self.pending.lock().await;
                pending.remove(&engine_id);
                self.log("send failed: not connected");
                return Err("the BuffrX engine is not running".to_string());
            }
            let mut write_failed: Option<String> = None;
            {
                let mut w = self.writer.lock().await;
                if let Some(writer) = w.as_mut() {
                    let bytes = outgoing_text.as_bytes();
                    let len = bytes.len() as u32;
                    let result = writer.write_all(&len.to_le_bytes()).await;
                    let result = if result.is_err() { result } else { writer.write_all(bytes).await };
                    let result = if result.is_err() { result } else { writer.flush().await };
                    if let Err(e) = result {
                        write_failed = Some(e.to_string());
                    }
                } else {
                    let mut pending = self.pending.lock().await;
                    pending.remove(&engine_id);
                    self.log("send failed: not connected");
                    return Err("the BuffrX engine is not running".to_string());
                }
            }
            if let Some(e) = write_failed {
                // The connection is gone: stop pretending it is live, retire this request now.
                *self.connected.lock().await = false;
                *self.writer.lock().await = None;
                let mut pending = self.pending.lock().await;
                if let Some(p) = pending.remove(&engine_id) {
                    let _ = p.tx.send(serde_json::json!({"ok": false, "error": "the connection to the engine was lost"}).to_string());
                }
                drop(pending);
                self.log(&format!("write failed: {e}"));
                return Err("the BuffrX engine is not running".to_string());
            }
        }

        // Listing a large library probes every file once; everything else answers in milliseconds.
        // A folder picker waits for the person choosing. A Timeline save at the live edge closes the
        // chunk and copies through the 180 s media lock, and a Timeline read can walk a whole session.
        let timeout_s = match cmd.as_str() {
            "clips.list" | "screenshots.list" | "timeline.get" | "timeline.edit_source" | "timeline.loudness" => 60,
            "timeline.save" => 180,
            "app.pick_folder" => 900,
            _ => 10,
        };
        match tokio::time::timeout(std::time::Duration::from_secs(timeout_s), rx).await {
            Ok(Ok(resp)) => {
                let mut v: Value = serde_json::from_str(&resp).unwrap_or(Value::Null);
                if let Some(o) = v.as_object_mut() {
                    o.insert("id".to_string(), Value::String(page_id));
                }
                Ok(v.to_string())
            }
            Ok(Err(_)) => {
                let mut pending = self.pending.lock().await;
                pending.remove(&engine_id);
                Err("engine connection lost".to_string())
            }
            Err(_) => {
                let mut pending = self.pending.lock().await;
                pending.remove(&engine_id);
                self.log("request timed out");
                Err(format!("{cmd} timed out after {timeout_s} seconds"))
            }
        }
    }
}
