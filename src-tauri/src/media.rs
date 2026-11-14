//! Local media for the page: which folders it may touch, the `media` scheme that streams clips and
//! screenshots to <video>/<img> with byte ranges, and the editor's engine runs (probe, waveform,
//! export with progress and cancel).
//!
//! Nothing here widens on the page's say-so. The allowed folders are read from the engine's own
//! config.json (clips, screenshots, recordings) plus the data root, so a page can never be talked into
//! reading or writing anything else on the disk.

use serde_json::Value;
use std::collections::{HashMap, VecDeque};
use std::io::{BufRead, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};
use tauri::{AppHandle, Emitter};

/// An engine path as a native path. The engine keeps Windows separators by design; the editor's own
/// file calls must use the host's separator.
pub fn native(path: &str) -> PathBuf {
    #[cfg(windows)]
    {
        PathBuf::from(path)
    }
    #[cfg(unix)]
    {
        PathBuf::from(path.replace('\\', "/"))
    }
}

fn native_buf(path: &Path) -> PathBuf {
    native(&path.to_string_lossy())
}


// ---------------------------------------------------------------- local HTTP media (Linux)

#[cfg(unix)]
use std::net::TcpListener;
#[cfg(unix)]
use std::sync::atomic::{AtomicU16, Ordering};
#[cfg(unix)]
use std::sync::OnceLock;

#[cfg(unix)]
static MEDIA_PORT: AtomicU16 = AtomicU16::new(0);
#[cfg(unix)]
static MEDIA_TOKEN: OnceLock<String> = OnceLock::new();

#[cfg(unix)]
fn media_port() -> u16 { MEDIA_PORT.load(Ordering::Relaxed) }

/// The page is handed the loopback port and a per-run secret. Without the secret any local program,
/// and any page loaded in the editor, could read every clip and screenshot the allow-list reaches.
pub fn page_query() -> String {
    #[cfg(unix)]
    {
        let port = media_port();
        if port == 0 { return String::new(); }
        return format!("?media_port={port}&media_token={}", MEDIA_TOKEN.get().map(String::as_str).unwrap_or(""));
    }
    #[cfg(not(unix))]
    { String::new() }
}

/// A fresh 256-bit secret for one editor run. `/dev/urandom` is the source; the fallback still differs
/// per process and run, which is enough for a listener that only lives as long as the window.
#[cfg(unix)]
fn new_media_token() -> String {
    let mut bytes = [0u8; 32];
    let read = std::fs::File::open("/dev/urandom").and_then(|mut file| file.read_exact(&mut bytes));
    if read.is_err() {
        use std::hash::{BuildHasher, Hasher};
        for chunk in bytes.chunks_mut(8) {
            let mut hasher = std::collections::hash_map::RandomState::new().build_hasher();
            hasher.write_u32(std::process::id());
            hasher.write_u128(SystemTime::now().duration_since(UNIX_EPOCH).map(|age| age.as_nanos()).unwrap_or(0));
            chunk.copy_from_slice(&hasher.finish().to_le_bytes()[..chunk.len()]);
        }
    }
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

/// True when the request carries this run's secret. The comparison visits every byte so a caller
/// cannot learn the token a character at a time from how long the answer takes.
#[cfg(unix)]
fn authorised(query: &str) -> bool {
    let Some(expected) = MEDIA_TOKEN.get() else { return false };
    let Some(given) = query.split('&').find_map(|pair| pair.strip_prefix("media_token=")) else { return false };
    expected.len() == given.len() &&
        expected.bytes().zip(given.bytes()).fold(0u8, |mismatch, (left, right)| mismatch | (left ^ right)) == 0
}

/// The editor's WebKitGTK build plays media from a custom scheme unreliably; a loopback HTTP server
/// with byte ranges is the same transport the browser and every WebView agree on. Bound to 127.0.0.1
/// only, every request needs the page's token, and every path passes the same allow-list the custom
/// scheme uses. It runs only on the Linux host; Windows keeps the custom scheme.
#[cfg(unix)]
pub fn start_http_server(data_root: PathBuf) -> u16 {
    if media_port() != 0 {
        return media_port();
    }
    let Ok(listener) = TcpListener::bind(("127.0.0.1", 0)) else { return 0 };
    let Ok(address) = listener.local_addr() else { return 0 };
    let _ = MEDIA_TOKEN.set(new_media_token());
    MEDIA_PORT.store(address.port(), Ordering::Relaxed);
    std::thread::spawn(move || {
        for connection in listener.incoming() {
            let Ok(mut stream) = connection else { continue };
            let root = data_root.clone();
            std::thread::spawn(move || {
                let mut reader = std::io::BufReader::new(&mut stream);
                let mut request = String::new();
                if reader.read_line(&mut request).is_err() { return; }
                let mut range = None;
                let mut header = String::new();
                while reader.read_line(&mut header).is_ok() && !header.trim().is_empty() {
                    if let Some(value) = header.trim().strip_prefix("Range:").or_else(|| header.trim().strip_prefix("range:")) {
                        if let Some(value) = value.trim().strip_prefix("bytes=") {
                            if let Some((start, end)) = value.split_once('-') {
                                range = Some((start.parse::<u64>().unwrap_or(0), end.parse::<u64>().ok()));
                            }
                        }
                    }
                    header.clear();
                }
                let mut parts = request.split_whitespace();
                let _method = parts.next();
                let target = parts.next().unwrap_or("/");
                let (target, query) = target.split_once('?').unwrap_or((target, ""));
                if !authorised(query) {
                    use std::io::Write;
                    let _ = stream.write_all(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    return;
                }
                let (status, headers, body) = http_response(&root, target, range);
                let reason = match status { 200 => "OK", 206 => "Partial Content", 403 => "Forbidden", 404 => "Not Found", _ => "Error" };
                let mut response = format!("HTTP/1.1 {status} {reason}\r\n");
                for (name, value) in &headers {
                    response.push_str(&format!("{name}: {value}\r\n"));
                }
                response.push_str(&format!("Content-Length: {}\r\nConnection: close\r\n\r\n", body.len()));
                use std::io::Write;
                let _ = stream.write_all(response.as_bytes());
                let _ = stream.write_all(&body);
            });
        }
    });
    media_port()
}

#[cfg(not(unix))]
pub fn start_http_server(_data_root: PathBuf) -> u16 { 0 }

/// `path` is `<encoded absolute path>` or begins with `/media/`; `/thumbnails/<name>` reads the cache.
fn http_response(data_root: &Path, target: &str, range: Option<(u64, Option<u64>)>) -> (u16, Vec<(String, String)>, Vec<u8>) {
    let mut cors = vec![("Access-Control-Allow-Origin".to_string(), "*".to_string()),
                        ("Accept-Ranges".to_string(), "bytes".to_string())];
    let raw = target.split('?').next().unwrap_or(target).trim_start_matches('/');
    let (kind, raw) = match raw.split_once('/') {
        Some(("media", rest)) => ("media", rest),
        Some(("thumbnails", rest)) => ("thumbnails", rest),
        _ => ("media", raw),
    };
    let decoded = percent_decode(raw);
    let path = if kind == "thumbnails" {
        let name = decoded.trim_start_matches('/');
        if name.is_empty() || name.contains('/') || name.contains('\\') || name.contains("..") {
            return (404, cors, Vec::new());
        }
        data_root.join("thumbnails").join(name)
    } else {
        let path = native(&decoded);
        if !is_allowed_to_read(data_root, &path) {
            return (403, cors, Vec::new());
        }
        path
    };
    let Ok(mut file) = std::fs::File::open(&path) else { return (404, cors, Vec::new()) };
    let size = file.metadata().map(|m| m.len()).unwrap_or(0);
    let content_type = content_type(&path).to_string();
    cors.push(("Content-Type".to_string(), content_type));
    if let Some((start, end)) = range {
        let start = start.min(size);
        let end = end.unwrap_or(size.saturating_sub(1)).min(size.saturating_sub(1));
        if start > end || size == 0 {
            return (416, cors, Vec::new());
        }
        use std::io::{Read, Seek, SeekFrom};
        let mut body = vec![0u8; (end - start + 1) as usize];
        if file.seek(SeekFrom::Start(start)).is_err() || file.read_exact(&mut body).is_err() {
            return (500, cors, Vec::new());
        }
        cors.push(("Content-Range".to_string(), format!("bytes {start}-{end}/{size}")));
        return (206, cors, body);
    }
    use std::io::Read;
    let mut body = Vec::with_capacity(size as usize);
    if file.read_to_end(&mut body).is_err() {
        return (500, cors, Vec::new());
    }
    (200, cors, body)
}

/// The folders the page may read from and the editor may write into.
pub fn allowed_roots(data_root: &Path) -> Vec<PathBuf> {
    let mut roots = vec![data_root.to_path_buf()];
    #[cfg(windows)]
    let videos = std::env::var("USERPROFILE").map(|home| PathBuf::from(home).join("Videos").join("BuffrX")).ok();
    #[cfg(unix)]
    let videos = std::env::var("HOME").map(|home| PathBuf::from(home).join("Videos").join("BuffrX")).ok();
    let config: Value = std::fs::read_to_string(data_root.join("config.json"))
        .ok()
        .and_then(|text| serde_json::from_str(text.trim_start_matches('\u{feff}')).ok())
        .unwrap_or(Value::Null);
    let clips = config.get("clips_dir").and_then(|v| v.as_str()).filter(|s| !s.is_empty()).map(native).or(videos);
    if let Some(clips) = clips {
        roots.push(clips);
    }
    for key in ["screenshots_dir", "recordings_dir"] {
        if let Some(dir) = config.get(key).and_then(|v| v.as_str()).filter(|s| !s.is_empty()) {
            roots.push(native(dir));
        }
    }
    roots
}

/// The path as text for a prefix test: lower case, `/` as a separator, no trailing separator. No
/// filesystem call: `exists()` on a path a page named would make Windows dial its host (and offer the
/// user's NTLM hash) before the allow-check. A path with `..` has no text form: it is refused.
fn lexical(path: &Path) -> Option<String> {
    if path.components().any(|c| matches!(c, std::path::Component::ParentDir)) {
        return None; // no walking out with ".."
    }
    #[cfg(windows)]
    { Some(path.to_string_lossy().replace('/', "\\").trim_end_matches('\\').to_lowercase()) }
    #[cfg(unix)]
    { Some(path.to_string_lossy().replace('\\', "/").trim_end_matches('/').to_lowercase()) }
}

fn path_under(target: &str, root: &str) -> bool {
    #[cfg(windows)]
    { target.len() > root.len() && target.starts_with(root) && target.as_bytes()[root.len()] == b'\\' }
    #[cfg(unix)]
    { target.len() > root.len() && target.starts_with(root) && target.as_bytes()[root.len()] == b'/' }
}

/// Whether `root` holds `path`. A local path is resolved, so a junction inside the folder cannot reach
/// out. A network path is compared by text only: `exists()` on such a name would make Windows dial that
/// host (and offer the user's NTLM hash) before the allow check, and a folder the user chose on a share
/// is taken by its name.
fn within(root: &Path, path: &Path) -> bool {
    let network = |p: &Path| {
        matches!(p.components().next(),
            Some(std::path::Component::Prefix(prefix))
                if matches!(prefix.kind(), std::path::Prefix::UNC(..) | std::path::Prefix::VerbatimUNC(..)))
    };
    if network(path) {
        let (Some(root_text), Some(target_text)) = (lexical(root), lexical(path)) else { return false };
        return path_under(&target_text, &root_text);
    }
    if network(root) {
        return false;  // a share holds no local file
    }
    match (normalised(root), normalised(path)) {
        (Some(root), Some(target)) => path_under(&target, &root),
        _ => false,
    }
}

fn normalised(path: &Path) -> Option<String> {
    // The engine names files with Windows separators on every platform, and the page sends those names
    // back unchanged. On Unix a backslash is an ordinary byte, so without this conversion the whole
    // path is one long file name and every clip the page names is refused. It is the same rule the
    // path shim applies at the syscall boundary (src/linux/pathshim.c).
    #[cfg(unix)]
    let path = &native(&path.to_string_lossy());
    if path.components().any(|c| matches!(c, std::path::Component::ParentDir)) {
        return None; // no walking out with ".."
    }
    #[cfg(unix)]
    {
        if !path.is_absolute() {
            return None;
        }
        let mut existing = path.to_path_buf();
        let mut rest = Vec::new();
        while !existing.exists() {
            rest.push(existing.file_name()?.to_os_string());
            existing = existing.parent()?.to_path_buf();
        }
        let mut resolved = std::fs::canonicalize(&existing).ok()?;
        for part in rest.iter().rev() {
            resolved.push(part);
        }
        return Some(resolved.to_string_lossy().to_lowercase());
    }
    #[cfg(windows)]
    {
    // Only a local drive path is a library file. `\\host\share` is not: calling `exists()` on it makes
    // Windows connect to that host (and offer the user's NTLM hash to it) before any allow-check.
    let mut drive = false;
    let mut root = false;
    for component in path.components() {
        match component {
            std::path::Component::Prefix(prefix) => match prefix.kind() {
                std::path::Prefix::Disk(_) | std::path::Prefix::VerbatimDisk(_) => drive = true,
                _ => return None,
            },
            std::path::Component::RootDir => root = true,
            std::path::Component::Normal(_) | std::path::Component::CurDir => {}
            std::path::Component::ParentDir => return None,
        }
    }
    if !drive || !root {
        return None;
    }
    // A file or folder that does not exist yet (an export into a new Edits folder) is judged by the
    // nearest ancestor that does, with the rest of the path appended.
    let mut existing = path.to_path_buf();
    let mut rest = Vec::new();
    while !existing.exists() {
        rest.push(existing.file_name()?.to_os_string());
        existing = existing.parent()?.to_path_buf();
    }
    let mut resolved = std::fs::canonicalize(&existing).ok()?;
    for part in rest.iter().rev() {
        resolved.push(part);
    }
    Some(resolved.to_string_lossy().to_lowercase())
    }
}

/// Files the user dropped on the editor: allowed for reading even outside the BuffrX folders, because
/// the user handed them over themselves. Exports still go into the BuffrX folders.
static DROPPED: std::sync::OnceLock<Mutex<std::collections::HashSet<String>>> = std::sync::OnceLock::new();

pub fn allow_dropped(paths: &[PathBuf]) {
    let set = DROPPED.get_or_init(|| Mutex::new(Default::default()));
    let mut set = set.lock().unwrap();
    for path in paths {
        if let Some(key) = lexical(path) {
            set.insert(key);
        }
    }
}

fn was_dropped(target: &str) -> bool {
    DROPPED.get().map_or(false, |set| set.lock().unwrap().contains(target))
}

/// True when `path` lies inside one of the allowed folders (after resolving links), or is a file the
/// user dropped on the editor and `reading` is true.
pub fn is_allowed_to_read(data_root: &Path, path: &Path) -> bool {
    lexical(path).map_or(false, |target| was_dropped(&target)) || is_allowed(data_root, path)
}

pub fn is_allowed(data_root: &Path, path: &Path) -> bool {
    allowed_roots(data_root).iter().any(|root| within(root, path))
}

pub fn content_type(path: &Path) -> &'static str {
    match path.extension().and_then(|e| e.to_str()).map(|e| e.to_ascii_lowercase()).as_deref() {
        Some("mp4") | Some("m4v") => "video/mp4",
        Some("m4a") => "audio/mp4",
        Some("mkv") => "video/x-matroska",
        Some("webm") => "video/webm",
        Some("mov") => "video/quicktime",
        Some("png") => "image/png",
        Some("jpg") | Some("jpeg") => "image/jpeg",
        _ => "application/octet-stream",
    }
}

pub fn percent_decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            // Decode the two hex bytes as bytes: a `%` before a multi-byte UTF-8 character must not
            // slice the string on a character boundary, which panics the worker that serves media.
            if let Ok(hex) = std::str::from_utf8(&bytes[i + 1..i + 3]) {
                if let Ok(b) = u8::from_str_radix(hex, 16) {
                    out.push(b);
                    i += 3;
                    continue;
                }
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// True while a writer holds the file: opening it for read while granting only read sharing fails
/// with a sharing violation against a handle that may write. The engine refuses the same files
/// (`still_being_written`); the editor used to serve a recording that was still being written, and
/// the page then played a moov-less MP4 or hung at its end.
#[cfg(unix)]
fn still_being_written(_path: &Path) -> bool {
    // POSIX has no share-mode open. A recording is served as it grows; its fragmented MP4 index is
    // written as it goes, so the page can read what exists.
    false
}

#[cfg(windows)]
fn still_being_written(path: &Path) -> bool {
    use std::os::windows::fs::OpenOptionsExt;
    match std::fs::OpenOptions::new().read(true).share_mode(0x1).open(path) {
        Ok(_) => false,
        // A missing file is the caller's 404, not a writer.
        Err(error) => error.kind() != std::io::ErrorKind::NotFound,
    }
}

/// `<store>\sessions\<id>\media\<chunk>`: a file of the Timeline's own store, which the Live view reads while it is written.
fn is_timeline_chunk(path: &Path) -> bool {
    let name = |p: Option<&Path>| p.and_then(|p| p.file_name()).map(|n| n.to_string_lossy().to_ascii_lowercase());
    let media = path.parent();
    name(media).as_deref() == Some("media") && name(media.and_then(|m| m.parent()).and_then(|s| s.parent())).as_deref() == Some("sessions")
}

/// `http://media.localhost/<percent-encoded absolute path>`: the file, honouring a byte range so a
/// clip of any size streams into <video> and seeking works.
pub fn serve(data_root: &Path, request: &tauri::http::Request<Vec<u8>>) -> tauri::http::Response<Vec<u8>> {
    let respond = |status: u16| tauri::http::Response::builder()
        .status(status)
        .header("Access-Control-Allow-Origin", "*")
        .body(Vec::new()).unwrap();
    let raw = request.uri().path().trim_start_matches('/');
    let path = native(&percent_decode(raw));
    if !is_allowed_to_read(data_root, &path) {
        return respond(403);
    }
    // A Timeline chunk is read while its writer still holds it: Live tails it and trims to whole fragments itself
    // (player.ts completePrefix). Everything else a writer holds stays refused.
    if !is_timeline_chunk(&path) && still_being_written(&path) {
        return respond(409);
    }
    let Ok(mut file) = std::fs::File::open(&path) else { return respond(404) };
    let size = file.metadata().map(|m| m.len()).unwrap_or(0);
    // A query piece is a simple CORS request; a Range header is not, and the custom scheme has no
    // preflight answer. The Timeline reader asks this way; <video> still sends Range.
    let piece = request
        .uri()
        .query()
        .and_then(|query| {
            query
                .split('&')
                .find_map(|pair| pair.strip_prefix("piece="))
                .and_then(|value| value.parse::<u64>().ok())
        });
    let range = request
        .headers()
        .get("range")
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.strip_prefix("bytes="))
        .and_then(|v| {
            let (start, end) = v.split_once('-')?;
            if start.trim().is_empty() {
                // A suffix range (bytes=-500) asks for the file's tail, not its head: a media reader
                // that opens a clip by its last bytes must get those bytes.
                let length: u64 = end.trim().parse().ok()?;
                if length == 0 || size == 0 {
                    return Some((size, 0));
                }
                return Some((size.saturating_sub(length), size - 1));
            }
            let start: u64 = start.trim().parse().ok()?;
            let end: u64 = end.trim().parse().unwrap_or(u64::MAX);
            Some((start, end))
        });
    // A whole small file in one answer; anything larger in pieces, as a media element asks for them.
    const PIECE: u64 = 8 * 1024 * 1024;
    let (start, end, partial) = if let Some(start) = piece {
        if start >= size {
            return tauri::http::Response::builder()
                .status(416)
                .header("Content-Range", format!("bytes */{size}"))
                .header("Access-Control-Allow-Origin", "*")
                .header("Access-Control-Expose-Headers", "Content-Range")
                .body(Vec::new())
                .unwrap();
        }
        (start, (start + PIECE - 1).min(size - 1), true)
    } else {
        match range {
            Some((start, end)) if start < size => {
                // bytes=10-5 (or an end past the file) must never wrap: end - start + 1 in u64.
                let end = end.max(start).min(size - 1).min(start + PIECE - 1);
                (start, end, true)
            }
            Some(_) => {
                return tauri::http::Response::builder()
                    .status(416)
                    .header("Content-Range", format!("bytes */{size}"))
                    .header("Access-Control-Allow-Origin", "*")
                    .header("Access-Control-Expose-Headers", "Content-Range")
                    .body(Vec::new())
                    .unwrap()
            }
            None if size > PIECE * 8 => (0, PIECE - 1, true),
            None => (0, size.saturating_sub(1), false),
        }
    };
    let length = if size == 0 { 0 } else { end - start + 1 };
    let mut body = vec![0u8; length as usize];
    if file.seek(SeekFrom::Start(start)).is_err() || file.read_exact(&mut body).is_err() {
        return respond(500);
    }
    let mut builder = tauri::http::Response::builder()
        .status(if partial { 206 } else { 200 })
        .header("Content-Type", content_type(&path))
        .header("Accept-Ranges", "bytes")
        .header("Content-Length", length.to_string())
        .header("Cache-Control", "no-store")
        // The page draws a screenshot onto a canvas to save an edit; without this the
        // canvas is tainted and cannot be read back. Only the app's own pages can reach this
        // scheme. Content-Range must be readable by fetch for the preview's piece reads.
        .header("Access-Control-Allow-Origin", "*")
        .header("Access-Control-Expose-Headers", "Content-Range, Content-Length");
    if partial {
        builder = builder.header("Content-Range", format!("bytes {start}-{end}/{size}"));
    }
    builder.body(body).unwrap()
}

fn engine_exe() -> PathBuf { crate::engine_path() }

fn engine_command() -> std::process::Command {
    let mut command = std::process::Command::new(engine_exe());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x08000000); // CREATE_NO_WINDOW: no console flashes over the editor
    }
    command
}

/// One engine run that only reads the clip (`--media-info`, `--media-peaks`, `--media-thumb`,
/// `--media-probe`, and `--media-audio`, which writes one track's copy into the data root's cache):
/// its stdout. Files must lie in the allowed folders, outputs too.
fn allowed_media_read(flag: &str) -> bool {
    ["--media-info", "--media-peaks", "--media-keyframes", "--media-thumb", "--media-probe", "--media-audio"].contains(&flag)
}

pub fn run(data_root: &Path, exports: &Exports, args: Vec<String>, id: Option<String>) -> Result<String, String> {
    let Some(flag) = args.first() else { return Err("nothing to run".into()) };
    if !allowed_media_read(flag) {
        return Err(format!("{flag} is not something the editor may run"));
    }
    for arg in args.iter().skip(1) {
        // A flag is never a file: an argument that is another switch could turn a read-only probe
        // into a second engine run (`--show` and the like).
        if arg.starts_with("--") {
            return Err(format!("{arg} is not a file"));
        }
        // A drive-relative `C:foo` names a path on C: without a slash, and would be read from wherever
        // the process looks; anything with a colon is refused as a path that cannot be checked.
        let looks_like_path = arg.contains('\\') || arg.contains('/') || arg.contains(':');
        if looks_like_path && !is_allowed_to_read(data_root, Path::new(arg)) {
            return Err("that file is outside the BuffrX folders".into());
        }
    }
    let mut child = engine_command()
        .args(&args)
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::null())
        .spawn()
        .map_err(|e| e.to_string())?;
    let stdout = child.stdout.take();
    // The process is held where a cancel can reach it: taking the child out is what ends it, and a
    // read this call still owns is killed and reaped before it answers.
    let held = Arc::new(Mutex::new(Some(child)));
    let name = id.unwrap_or_default();
    if !name.is_empty() {
        if let Err(error) = begin_read(exports, name.clone(), held.clone()) {
            if let Some(mut child) = held.lock().unwrap().take() {
                let _ = child.kill();
                let _ = child.wait();
            }
            return Err(error);
        }
    }
    let (sender, receiver) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        let mut text = String::new();
        if let Some(mut out) = stdout {
            let _ = out.read_to_string(&mut text);
        }
        let _ = sender.send(text);
    });
    // A probe that never finishes used to hold the page's editor on a blank frame with no answer.
    // Peaks walk the whole audio track, so they get minutes; a metadata read gets one.
    let mut limit = std::time::Duration::from_secs(60);
    if flag == "--media-peaks" {
        limit = std::time::Duration::from_secs(600);
    }
    let outcome = receiver.recv_timeout(limit);
    // Whichever way it ended, the child is reaped here (or was already taken and ended by a cancel),
    // and its name is free again: nothing outlives the call.
    if let Some(mut child) = held.lock().unwrap().take() {
        let _ = child.kill();
        let _ = child.wait();
    }
    if !name.is_empty() {
        end_read(exports, &name, &held);
    }
    match outcome {
        Ok(text) => Ok(text),
        Err(_) => Err("the media probe did not finish in time".into()),
    }
}

/// A path in the allowed folders that does not exist yet: `name.mp4`, else `name-2.mp4`, `name-3.mp4`…
pub fn unique_path(data_root: &Path, path: &Path) -> Result<PathBuf, String> {
    // The check below answers engine-shaped paths, but `exists` and `parent` need the host's separator.
    let path = native(&path.to_string_lossy());
    if !is_allowed(data_root, &path) {
        return Err("that file is outside the BuffrX folders".into());
    }
    if !path.exists() {
        return Ok(path);
    }
    let stem = path.file_stem().and_then(|s| s.to_str()).unwrap_or("edit");
    let ext = path.extension().and_then(|e| e.to_str()).map(|e| format!(".{e}")).unwrap_or_default();
    let parent = path.parent().unwrap_or(Path::new("."));
    for n in 2..10_000 {
        let candidate = parent.join(format!("{stem}-{n}{ext}"));
        if !candidate.exists() {
            return Ok(candidate);
        }
    }
    Err("no free name".into())
}

/// Replaces `to` with `from` in one step. A replace that fails half way must not leave a truncated
/// image where the original was, which is what a write straight onto the target does.
pub(crate) fn move_over(from: &Path, to: &Path) -> std::io::Result<()> {
    #[cfg(windows)]
    {
        use std::os::windows::ffi::OsStrExt;
        extern "system" {
            fn MoveFileExW(from: *const u16, to: *const u16, flags: u32) -> i32;
        }
        const MOVEFILE_REPLACE_EXISTING: u32 = 1;
        let wide = |path: &Path| path.as_os_str().encode_wide().chain(std::iter::once(0)).collect::<Vec<u16>>();
        let (from, to) = (wide(from), wide(to));
        if unsafe { MoveFileExW(from.as_ptr(), to.as_ptr(), MOVEFILE_REPLACE_EXISTING) } == 0 {
            return Err(std::io::Error::last_os_error());
        }
        Ok(())
    }
    #[cfg(not(windows))]
    {
        std::fs::rename(from, to)
    }
}

/// The children the editor has running: its exports by id, and its media reads by name, so a cancel
/// can find any of them.
#[derive(Default)]
pub struct Exports {
    next: Mutex<u64>,
    running: Mutex<HashMap<u64, (std::process::Child, String)>>,
    /// Read-only runs still in flight, by the name the page gave each one. A peaks read walks a whole
    /// audio track and can take minutes, so a view that has moved on ends it instead of waiting.
    reads: Mutex<HashMap<String, Arc<Mutex<Option<std::process::Child>>>>>,
    /// Names whose read was cancelled before it could register. The page's run and its cancel are two
    /// separate calls over an asynchronous bridge, so the cancel can arrive first; without this the read
    /// then starts nobody wants and holds a place for its whole limit. Bounded: a cancel of a name that
    /// never runs must not grow it. A name is dropped from here when it is used or when the bound is hit.
    cancelled: Mutex<VecDeque<String>>,
    timeline_files: Mutex<Vec<PathBuf>>,
    timeline_root: Mutex<Option<PathBuf>>,
    closing: std::sync::atomic::AtomicBool,
}

/// No page needs more reads at once than it has places for (two keyframe reads and one peaks read).
/// The bound is what keeps a page that never cancels from leaving a process per name behind.
const MAX_READS: usize = 8;

/// How many early cancels are remembered. The page names each read in order (`media-1`, `media-2`…), so
/// the names it cancels ahead of their run are the few it has just asked for; anything older is a name
/// nothing will run and the oldest is let go.
const MAX_CANCELLED: usize = 4 * MAX_READS;

/// Registers a read under the name its page gave it. A closed editor starts none, and a name that was
/// already cancelled never starts one, however late it arrives.
///
/// The close check, the duplicate check, the bound and the insert all happen under the reads lock, and
/// `cancel_all` sets `closing` before it drains that same map, so the two orders are the only ones:
/// either the read inserts first and the drain ends it, or closing is already set and the read is
/// refused. Lock order is reads, then cancelled, in both this and `cancel_read`.
fn begin_read(exports: &Exports, id: String, child: Arc<Mutex<Option<std::process::Child>>>) -> Result<(), String> {
    if id.is_empty() || id.len() > 64 {
        return Err("that is not a read name".into());
    }
    let mut reads = exports.reads.lock().unwrap();
    if exports.closing.load(std::sync::atomic::Ordering::Acquire) {
        return Err("cancelled".into());
    }
    {
        let mut cancelled = exports.cancelled.lock().unwrap();
        if let Some(at) = cancelled.iter().position(|name| *name == id) {
            cancelled.remove(at);
            return Err("cancelled".into());
        }
    }
    if reads.contains_key(&id) {
        // Overwriting the slot would drop a child nobody holds and nobody ends: the name is taken.
        return Err(format!("the read {id} is already running"));
    }
    if reads.len() >= MAX_READS {
        return Err("too many media reads at once".into());
    }
    reads.insert(id, child);
    Ok(())
}

/// Ends one read by its name: its process is killed and reaped, never merely dropped. Returns whether
/// a read of that name was still running. A cancel that finds nothing to end remembers the name, so a
/// read that arrives after it (registration is a moment behind the page's call) never starts.
pub fn cancel_read(exports: &Exports, id: &str) -> bool {
    let child = {
        let mut reads = exports.reads.lock().unwrap();
        let Some(child) = reads.remove(id) else {
            let mut cancelled = exports.cancelled.lock().unwrap();
            if !cancelled.iter().any(|name| name == id) {
                cancelled.push_back(id.to_owned());
                while cancelled.len() > MAX_CANCELLED { cancelled.pop_front(); }
            }
            return false;
        };
        child
    };
    if let Some(mut child) = child.lock().unwrap().take() {
        let _ = child.kill();
        let _ = child.wait();
    }
    true
}

/// Frees a read's own place when it ends. Only the child this call registered is removed: a name that
/// was cancelled and run again while the old call was still unwinding belongs to the newer read now.
fn end_read(exports: &Exports, id: &str, held: &Arc<Mutex<Option<std::process::Child>>>) {
    let mut reads = exports.reads.lock().unwrap();
    if reads.get(id).map(|entry| Arc::ptr_eq(entry, held)).unwrap_or(false) {
        reads.remove(id);
    }
}

/// Every read still in flight is ended when the editor closes.
pub fn cancel_reads(exports: &Exports) {
    let reads: Vec<_> = exports.reads.lock().unwrap().drain().map(|(_, child)| child).collect();
    for child in reads {
        if let Some(mut child) = child.lock().unwrap().take() {
            let _ = child.kill();
            let _ = child.wait();
        }
    }
}

struct TimelineTemps { paths: Vec<PathBuf>, keep: bool }
impl Drop for TimelineTemps {
    fn drop(&mut self) {
        for (index, path) in self.paths.iter().enumerate() {
            if index != 0 || !self.keep { let _ = std::fs::remove_file(path); }
        }
    }
}

fn is_timeline_cache_name(name: &str) -> bool {
    let Some(rest) = name.strip_prefix("timeline-") else { return false };
    let Some((pid, rest)) = rest.split_once('-') else { return false };
    let Some((id, rest)) = rest.split_once('-') else { return false };
    let Some((stamp, suffix)) = rest.split_once('.') else { return false };
    !pid.is_empty() && !id.is_empty() && !stamp.is_empty() &&
        [pid, id, stamp].iter().all(|part| part.bytes().all(|b| b.is_ascii_digit())) &&
        ["mp4", "join-spec.json", "mp4.join.json", "export.log", "mp4.part", "mp4.part.mp4"].contains(&suffix)
}

/// The editor is single-instance. At startup no older editor can still use these private copies.
pub fn cleanup_timeline_cache(data_root: &Path) {
    let directory = data_root.join("editor-cache");
    let real_dir = |path: &Path| std::fs::symlink_metadata(path).map(|m| m.file_type().is_dir() && !m.file_type().is_symlink()).unwrap_or(false);
    if !real_dir(&directory) { return; }
    for entry in std::fs::read_dir(&directory).into_iter().flatten().flatten() {
        let name = entry.file_name();
        if name.to_str().map(is_timeline_cache_name).unwrap_or(false) && entry.file_type().map(|t| t.is_file()).unwrap_or(false) {
            let _ = std::fs::remove_file(entry.path());
        }
    }
    // A cancelled stitch child cannot run its own cleanup. Only its generated numeric work folders
    // and part files are removed; links, subdirectories and other files are never followed.
    let hidden = directory.join(".buffrx");
    let work = hidden.join("stitch");
    if !real_dir(&hidden) || !real_dir(&work) { return; }
    for folder in std::fs::read_dir(&work).into_iter().flatten().flatten() {
        let name = folder.file_name();
        let valid = name.to_str().and_then(|n| n.split_once('-')).map(|(pid, tick)|
            !pid.is_empty() && !tick.is_empty() && [pid, tick].iter().all(|s| s.bytes().all(|b| b.is_ascii_digit()))
        ).unwrap_or(false);
        if !valid || !folder.file_type().map(|t| t.is_dir()).unwrap_or(false) { continue; }
        for file in std::fs::read_dir(folder.path()).into_iter().flatten().flatten() {
            let name = file.file_name();
            let valid_part = name.to_str().and_then(|n| n.strip_prefix("part-"))
                .and_then(|n| n.split_once('.')).map(|(index, suffix)|
                    !index.is_empty() && index.bytes().all(|b| b.is_ascii_digit()) &&
                    ["mp4", "mp4.part", "mp4.part.mp4"].contains(&suffix)
                ).unwrap_or(false);
            if valid_part && file.file_type().map(|t| t.is_file()).unwrap_or(false) {
                let _ = std::fs::remove_file(file.path());
            }
        }
        let _ = std::fs::remove_dir(folder.path());
    }
    let _ = std::fs::remove_dir(&work);
    let _ = std::fs::remove_dir(directory.join(".buffrx"));
}

fn start_managed(exports: &Exports, id: u64, mut child: std::process::Child, output: String) -> Result<(), String> {
    let mut running = exports.running.lock().unwrap();
    if exports.closing.load(std::sync::atomic::Ordering::Acquire) {
        let _ = child.kill();
        let _ = child.wait();
        return Err("cancelled".into());
    }
    running.insert(id, (child, output));
    Ok(())
}

/// What a busy child's progress file looks like now: its length and when it last changed. Any change
/// counts as progress; a file that cannot be read shows none.
fn progress_stamp(path: &Path) -> Option<(u64, Option<std::time::SystemTime>)> {
    let meta = std::fs::metadata(path).ok()?;
    Some((meta.len(), meta.modified().ok()))
}

/// A media child that has shown no sign of life for this long is ended.
const NO_PROGRESS_LIMIT: std::time::Duration = std::time::Duration::from_secs(15 * 60);

/// Wait for a child without holding the registry lock, so closing the editor can stop it. A media
/// process that never finishes is a hang the page cannot cancel (the editor is waiting on this
/// call): after `quiet` with no sign of life it is killed and the caller gets an error.
///
/// `stall` is the file a working child keeps growing (an export's log): while its size or time of
/// change moves, the deadline starts again, so a whole-session re-encode may run for hours and only a
/// child that has actually stopped is ended. `None` fixes the deadline instead.
fn wait_managed(exports: &Exports, id: u64, stall: Option<&Path>, quiet: std::time::Duration) -> Result<(), String> {
    let mut deadline = std::time::Instant::now() + quiet;
    let mut seen = stall.and_then(progress_stamp);
    loop {
        if let Some(path) = stall {
            let stamp = progress_stamp(path);
            if stamp != seen {
                seen = stamp;
                deadline = std::time::Instant::now() + quiet;
            }
        }
        {
            let mut running = exports.running.lock().unwrap();
            let Some((child, _)) = running.get_mut(&id) else { return Err("cancelled".into()) };
            match child.try_wait() {
                Ok(Some(status)) => {
                    running.remove(&id);
                    return if status.success() { Ok(()) } else { Err("the media process failed".into()) };
                }
                Ok(None) => {}
                Err(error) => {
                    let _ = running.remove(&id);
                    return Err(error.to_string());
                }
            }
            if std::time::Instant::now() >= deadline {
                if let Some((mut child, _)) = running.remove(&id) {
                    let _ = child.kill();
                    let _ = child.wait();
                }
                return Err("the media process did not finish in time".into());
            }
        }
        std::thread::sleep(std::time::Duration::from_millis(100));
    }
}

/// One temporary file for an entire finished Timeline session. Compatible chunks are stream-copied;
/// unlike stream layouts use the editor's existing stitch export, which refuses incompatible audio.
pub fn merge_timeline(data_root: &Path, exports: &Exports, files: Vec<String>) -> Result<String, String> {
    if exports.closing.load(std::sync::atomic::Ordering::Acquire) { return Err("cancelled".into()); }
    if files.is_empty() || files.iter().any(|file| !is_allowed_to_read(data_root, Path::new(file))) {
        return Err("Timeline footage is missing or outside the BuffrX folders".into());
    }
    if files.len() == 1 { return Ok(files[0].clone()); }
    let directory = data_root.join("editor-cache");
    std::fs::create_dir_all(&directory).map_err(|e| e.to_string())?;
    let cache_type = std::fs::symlink_metadata(&directory).map_err(|e| e.to_string())?.file_type();
    if !cache_type.is_dir() || cache_type.is_symlink() { return Err("The Timeline cache is not a local folder".into()); }
    *exports.timeline_root.lock().unwrap() = Some(data_root.to_path_buf());
    let id = { let mut next = exports.next.lock().unwrap(); *next += 1; *next };
    let stamp = SystemTime::now().duration_since(UNIX_EPOCH).map_err(|e| e.to_string())?.as_nanos();
    let output = directory.join(format!("timeline-{}-{id}-{stamp}.mp4", std::process::id()));
    let output_text = output.to_string_lossy().into_owned();
    let spec = output.with_extension("join-spec.json");
    let report = PathBuf::from(format!("{output_text}.join.json"));
    let log = output.with_extension("export.log");
    let mut temps = TimelineTemps { paths: vec![output.clone(), spec.clone(), report.clone(), log.clone()], keep: false };
    let join = serde_json::json!({"files": files, "output": output_text, "start_in_first": 0, "end_in_last": 1.0e12});
    std::fs::write(&spec, join.to_string()).map_err(|e| e.to_string())?;
    let child = engine_command().arg("--media-join").arg(&spec)
        .stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null()).spawn().map_err(|e| e.to_string())?;
    start_managed(exports, id, child, output_text.clone())?;
    if let Err(error) = wait_managed(exports, id, None, NO_PROGRESS_LIMIT) {
        if error == "cancelled" { return Err(error); }
        let reason = std::fs::read_to_string(&report).ok().and_then(|text| serde_json::from_str::<Value>(&text).ok())
            .and_then(|result| result.get("error").and_then(Value::as_str).map(str::to_string));
        return Err(reason.filter(|s| !s.is_empty()).unwrap_or(error));
    }
    let report_text = std::fs::read_to_string(&report).map_err(|_| "The Timeline join did not report its result".to_string())?;
    let result: Value = serde_json::from_str(&report_text).map_err(|e| e.to_string())?;
    if !result.get("ok").and_then(Value::as_bool).unwrap_or(false) {
        return Err(result.get("error").and_then(Value::as_str).unwrap_or("The Timeline could not be joined").into());
    }
    if result.get("dropped_head").and_then(Value::as_u64).unwrap_or(0) > 0 {
        // Never accept the joiner's trailing-only result: it would silently omit earlier footage.
        std::fs::remove_file(&output).map_err(|e| e.to_string())?;
        let inputs: Vec<Value> = files.iter().map(|file| serde_json::json!({"input": file})).collect();
        let job = serde_json::json!({"inputs": inputs, "output": output_text, "fit_pad": true});
        std::fs::write(&spec, job.to_string()).map_err(|e| e.to_string())?;
        let stdout = std::fs::File::create(&log).map_err(|e| e.to_string())?;
        let child = engine_command().arg("--media-export").arg(&spec)
            .stdout(stdout).stderr(std::process::Stdio::null()).spawn().map_err(|e| e.to_string())?;
        start_managed(exports, id, child, output_text.clone())?;
        if let Err(error) = wait_managed(exports, id, Some(&log), NO_PROGRESS_LIMIT) {
            if error == "cancelled" { return Err(error); }
            let reason = std::fs::read_to_string(&log).ok().and_then(|text| text.lines().rev()
                .find_map(|line| line.strip_prefix("EXPORT_FAIL ").map(str::to_string)));
            return Err(reason.unwrap_or(error));
        }
        if !std::fs::read_to_string(&log).unwrap_or_default().lines().any(|line| line.starts_with("EXPORT_OK")) {
            return Err("The Timeline's changed tracks could not be joined into one video".into());
        }
    }
    if !output.is_file() { return Err("The Timeline join produced no video".into()); }
    let mut held = exports.timeline_files.lock().unwrap();
    if exports.closing.load(std::sync::atomic::Ordering::Acquire) { return Err("cancelled".into()); }
    held.push(output);
    temps.keep = true;
    Ok(output_text)
}

/// Starts an export in its own engine process. Progress arrives as `media-progress {id, value}`, the
/// end as `media-done {id, ok, output, encoder, error}`.
pub fn start_export(app: AppHandle, data_root: &Path, exports: Arc<Exports>, job: Value) -> Result<u64, String> {
    let input = job.get("input").and_then(|v| v.as_str()).unwrap_or_default().to_string();
    let output = job.get("output").and_then(|v| v.as_str()).unwrap_or_default().to_string();
    // A join names its clips under `inputs`; a single save keeps using `input`. Every path is checked
    // here, because this is the boundary between the page and the filesystem.
    let inputs: Vec<String> = job
        .get("inputs")
        .and_then(|v| v.as_array())
        .map(|list| {
            list.iter()
                .filter_map(|entry| entry.get("input").and_then(|v| v.as_str()))
                .filter(|path| !path.is_empty())
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default();
    if output.is_empty() || (input.is_empty() && inputs.is_empty()) {
        return Err("an export needs an input and an output".into());
    }
    let inputs_allowed = inputs.iter().all(|path| is_allowed_to_read(data_root, Path::new(path)));
    if (!input.is_empty() && !is_allowed_to_read(data_root, Path::new(&input))) || !inputs_allowed {
        return Err("that file is outside the BuffrX folders".into());
    }
    if !is_allowed(data_root, Path::new(&output)) {
        return Err("that file is outside the BuffrX folders".into());
    }
    if let Some(parent) = native(&output).parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let id = {
        let mut next = exports.next.lock().unwrap();
        *next += 1;
        *next
    };
    let job_path = std::env::temp_dir().join(format!(
        "buffrx-export-{}-{id}-{}.json",
        std::process::id(),
        SystemTime::now().duration_since(UNIX_EPOCH).map(|age| age.as_nanos()).unwrap_or(0)
    ));
    // The name is not guessable and the file is created exclusively: a same-user process cannot
    // pre-place a link at the next name and have this write through it.
    let mut job_file = std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&job_path)
        .map_err(|e| e.to_string())?;
    if let Err(error) = job_file.write_all(job.to_string().as_bytes()) {
        // The file was created by this call and holds nothing usable: the same cleanup the spawn
        // failure below does.
        let _ = std::fs::remove_file(&job_path);
        return Err(error.to_string());
    }
    drop(job_file);
    let mut child = match engine_command()
        .arg("--media-export")
        .arg(&job_path)
        .stdout(std::process::Stdio::piped())
        .spawn()
    {
        Ok(child) => child,
        Err(error) => {
            let _ = std::fs::remove_file(&job_path);
            return Err(error.to_string());
        }
    };
    let stdout = child.stdout.take();
    exports.running.lock().unwrap().insert(id, (child, output.clone()));
    let exports_done = exports.clone();
    std::thread::spawn(move || {
        let mut last = String::new();
        if let Some(stdout) = stdout {
            for line in std::io::BufReader::new(stdout).lines().map_while(Result::ok) {
                if let Some(value) = line.strip_prefix("PROGRESS ").and_then(|v| v.trim().parse::<f64>().ok()) {
                    let _ = app.emit("media-progress", serde_json::json!({"id": id, "value": value}));
                } else if !line.trim().is_empty() {
                    last = line;
                }
            }
        }
        let cancelled = exports_done.running.lock().unwrap().remove(&id).is_none();
        let _ = std::fs::remove_file(&job_path);
        let ok = last.starts_with("EXPORT_OK");
        let encoder = last.split_whitespace().find_map(|w| w.strip_prefix("encoder=")).unwrap_or("").to_string();
        let error = if cancelled {
            "cancelled".to_string()
        } else {
            last.strip_prefix("EXPORT_FAIL ").unwrap_or(if ok { "" } else { "the export stopped" }).to_string()
        };
        let _ = app.emit(
            "media-done",
            serde_json::json!({"id": id, "ok": ok && !cancelled, "output": output, "encoder": encoder, "error": error}),
        );
    });
    Ok(id)
}

/// A preview copy for a file Windows cannot decode in the page: H.264 High, yuv420p, AAC stereo,
/// MP4, long side at most 1920, cached beside the engine's data so the next open is instant. One
/// process, only when direct playback failed.
pub fn preview_proxy(
    app: AppHandle,
    data_root: &Path,
    exports: Arc<Exports>,
    input: &str,
    width: u32,
    height: u32,
    sar: f64,
) -> Result<Value, String> {
    let source = native(input);
    if !is_allowed_to_read(data_root, &source) {
        return Err("that file is outside the BuffrX folders".into());
    }
    let metadata = std::fs::metadata(&source).map_err(|e| e.to_string())?;
    if !metadata.is_file() || metadata.len() == 0 {
        return Err("the file is empty".into());
    }
    let modified = metadata
        .modified()
        .ok()
        .and_then(|time| time.duration_since(UNIX_EPOCH).ok())
        .map(|age| age.as_nanos())
        .unwrap_or(0);
    let key = fnv1a(format!("{}|{}|{}", input.to_ascii_lowercase(), metadata.len(), modified).as_bytes());
    let directory = data_root.join("preview");
    std::fs::create_dir_all(&directory).map_err(|e| e.to_string())?;
    let output = directory.join(format!("{key:016x}.mp4"));
    prune_preview_cache(&directory, &output);
    if output.metadata().map(|m| m.len() > 0).unwrap_or(false) {
        return Ok(serde_json::json!({"output": output.to_string_lossy(), "cached": true}));
    }
    let (out_width, out_height) = preview_size(width, height, sar);
    let job = serde_json::json!({
        "input": input,
        "output": output.to_string_lossy(),
        "quality": 0.9,
        "video_kbps": 8000,
        "audio_kbps": 160,
        "out_width": out_width,
        "out_height": out_height,
        "square_pixels": true
    });
    let id = start_export(app, data_root, exports, job)?;
    Ok(serde_json::json!({"output": output.to_string_lossy(), "id": id, "cached": false}))
}

/// The proxy's frame: the source's displayed aspect at no more than 1920 on its long side, both
/// halves even, written with square pixels.
fn preview_size(width: u32, height: u32, sar: f64) -> (u32, u32) {
    let sar = if sar.is_finite() && sar > 0.01 && sar < 100.0 { sar } else { 1.0 };
    let (mut width, mut height) = if width == 0 || height == 0 {
        (1920, 1080)
    } else {
        (((width as f64 * sar).round() as u32).max(2), height)
    };
    let longest = width.max(height) as f64;
    if longest > 1920.0 {
        width = ((width as f64 * 1920.0 / longest).round() as u32).max(2);
        height = ((height as f64 * 1920.0 / longest).round() as u32).max(2);
    }
    ((width & !1).max(2), (height & !1).max(2))
}

/// FNV-1a, enough for a cache name: the input's identity is not a security boundary.
fn fnv1a(bytes: &[u8]) -> u64 {
    let mut hash = 0xcbf2_9ce4_8422_2325u64;
    for byte in bytes {
        hash ^= *byte as u64;
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    hash
}

/// Keeps the cache small without ever deleting the copy this call is about to use.
fn prune_preview_cache(directory: &Path, keep: &Path) {
    let now = SystemTime::now();
    let mut files: Vec<(PathBuf, u64, SystemTime)> = std::fs::read_dir(directory)
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|entry| {
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) != Some("mp4") || path == keep {
                return None;
            }
            let metadata = entry.metadata().ok()?;
            if !metadata.is_file() {
                return None;
            }
            Some((path, metadata.len(), metadata.modified().ok()?))
        })
        .collect();
    files.sort_by_key(|(_, _, modified)| *modified);
    let mut total: u64 = files.iter().map(|(_, size, _)| *size).sum();
    for (path, size, modified) in files {
        let old = now.duration_since(modified).map(|age| age.as_secs() > 7 * 24 * 60 * 60).unwrap_or(false);
        if old || total > 2 * 1024 * 1024 * 1024 {
            let _ = std::fs::remove_file(&path);
            total = total.saturating_sub(size);
        }
    }
}

/// A replace that was cancelled between the two renames leaves the original at `<output>.old-<engine pid>`
/// and no output. Put the original back; with a new output already in place, the old one is let go.
fn restore_cancelled_replace(output: &str, engine_pid: u32) {
    let base = native(output);
    // shortcut: a stale lower-numbered aside (a reused pid) makes the choice below restore the wrong
    // file; the engine would have to report its aside name to tell them apart.
    for n in 0..100 {
        let name = if n == 0 {
            format!("{output}.old-{engine_pid}")
        } else {
            format!("{output}.old-{engine_pid}-{n}")
        };
        let aside = native(&name);
        if !aside.exists() {
            // The engine takes the first free name, so the lowest one on disk is the one this job set
            // aside.
            continue;
        }
        if base.exists() {
            let _ = std::fs::remove_file(aside);
        } else {
            let _ = std::fs::rename(aside, base);
        }
        return;
    }
}

/// Stops an export and removes what it had written.
pub fn cancel_export(exports: &Exports, id: u64) {
    if let Some((mut child, output)) = exports.running.lock().unwrap().remove(&id) {
        let engine_pid = child.id();
        let _ = child.kill();
        let _ = child.wait();
        restore_cancelled_replace(&output, engine_pid);
        let native_output = native(&output);
        let _ = std::fs::remove_file(format!("{}.part", native_output.display()));
        if let Some(ext) = native_output.extension().and_then(|e| e.to_str()) {
            let _ = std::fs::remove_file(format!("{}.part.{ext}", native_output.display()));
        }
    }
}

/// Every export still running is stopped when the editor closes: closing it frees everything. `closing`
/// is set before the reads are drained, and `begin_read` checks it under the same lock the drain takes,
/// so no child can register after the close and no child that registered before it survives.
pub fn cancel_all(exports: &Exports) {
    exports.closing.store(true, std::sync::atomic::Ordering::Release);
    let ids: Vec<u64> = exports.running.lock().unwrap().keys().copied().collect();
    for id in ids {
        cancel_export(exports, id);
    }
    cancel_reads(exports);
    for path in exports.timeline_files.lock().unwrap().drain(..) {
        let _ = std::fs::remove_file(path);
    }
    if let Some(root) = exports.timeline_root.lock().unwrap().take() { cleanup_timeline_cache(&root); }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn timeline_cache_cleanup_only_matches_generated_names() {
        assert!(is_timeline_cache_name("timeline-123-4-567.mp4"));
        assert!(is_timeline_cache_name("timeline-123-4-567.mp4.join.json"));
        assert!(is_timeline_cache_name("timeline-123-4-567.join-spec.json"));
        assert!(!is_timeline_cache_name("timeline-123-4-567.mp4.backup"));
        assert!(!is_timeline_cache_name("timeline-user-4-567.mp4"));
        assert!(!is_timeline_cache_name("other.mp4"));
    }

    #[test]
    fn timeline_cache_cleanup_preserves_unrelated_files() {
        let root = std::env::temp_dir().join(format!("buffrx-timeline-cache-test-{}-{}", std::process::id(),
            SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos()));
        let cache = root.join("editor-cache");
        let work = cache.join(".buffrx").join("stitch").join("123-456");
        std::fs::create_dir_all(&work).unwrap();
        std::fs::write(cache.join("timeline-123-4-567.mp4"), b"temporary").unwrap();
        std::fs::write(cache.join("important.mp4"), b"keep").unwrap();
        std::fs::write(work.join("part-0.mp4"), b"temporary").unwrap();
        cleanup_timeline_cache(&root);
        assert!(!cache.join("timeline-123-4-567.mp4").exists());
        assert!(!work.exists());
        assert_eq!(std::fs::read(cache.join("important.mp4")).unwrap(), b"keep");
        std::fs::remove_file(cache.join("important.mp4")).unwrap();
        std::fs::remove_dir(cache).unwrap();
        std::fs::remove_dir(root).unwrap();
    }

    #[test]
    fn keyframe_read_is_allowed_but_export_is_not() {
        assert!(allowed_media_read("--media-keyframes"));
        assert!(!allowed_media_read("--media-export"));
    }

    fn request(uri: String) -> tauri::http::Request<Vec<u8>> {
        tauri::http::Request::builder().uri(uri).body(Vec::new()).unwrap()
    }

    #[test]
    fn a_piece_query_is_a_simple_request_and_returns_one_piece() {
        let root = std::env::temp_dir().join(format!("buffrx-media-piece-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let file = root.join("chunk.mp4");
        std::fs::write(&file, (0u8..20).collect::<Vec<_>>()).unwrap();
        let path = file.to_string_lossy().replace('\\', "/");

        let whole = serve(&root, &request(format!("/{path}")));
        assert_eq!(whole.status(), 200);
        assert_eq!(whole.body().as_slice(), &(0u8..20).collect::<Vec<_>>()[..]);

        let piece = serve(&root, &request(format!("/{path}?piece=8")));
        assert_eq!(piece.status(), 206);
        assert_eq!(piece.body().as_slice(), &(8u8..20).collect::<Vec<_>>()[..]);
        assert_eq!(piece.headers().get("Content-Range").unwrap(), "bytes 8-19/20");
        assert_eq!(piece.headers().get("Content-Length").unwrap(), "12");
        assert_eq!(
            piece.headers().get("Access-Control-Expose-Headers").unwrap(),
            "Content-Range, Content-Length"
        );

        let past = serve(&root, &request(format!("/{path}?piece=20")));
        assert_eq!(past.status(), 416);
        assert_eq!(past.headers().get("Access-Control-Allow-Origin").unwrap(), "*");
        assert_eq!(past.headers().get("Access-Control-Expose-Headers").unwrap(), "Content-Range");

        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn a_range_header_still_serves_the_video_element() {
        let root = std::env::temp_dir().join(format!("buffrx-media-range-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let file = root.join("chunk.mp4");
        std::fs::write(&file, (0u8..20).collect::<Vec<_>>()).unwrap();
        let path = file.to_string_lossy().replace('\\', "/");
        let mut request = request(format!("/{path}"));
        request.headers_mut().insert("range", "bytes=4-".parse().unwrap());
        let reply = serve(&root, &request);
        assert_eq!(reply.status(), 206);
        assert_eq!(reply.body().as_slice(), &(4u8..20).collect::<Vec<_>>()[..]);
        assert_eq!(reply.headers().get("Content-Range").unwrap(), "bytes 4-19/20");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn a_suffix_range_serves_the_tail() {
        let root = std::env::temp_dir().join(format!("buffrx-media-tail-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let file = root.join("chunk.mp4");
        std::fs::write(&file, (0u8..20).collect::<Vec<_>>()).unwrap();
        let path = file.to_string_lossy().replace('\\', "/");
        let mut request = request(format!("/{path}"));
        request.headers_mut().insert("range", "bytes=-5".parse().unwrap());
        let reply = serve(&root, &request);
        assert_eq!(reply.status(), 206);
        assert_eq!(reply.body().as_slice(), &(15u8..20).collect::<Vec<_>>()[..]);
        assert_eq!(reply.headers().get("Content-Range").unwrap(), "bytes 15-19/20");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn percent_decoding_survives_a_multibyte_character_after_a_percent() {
        assert_eq!(percent_decode("clip%20one.mp4"), "clip one.mp4");
        assert_eq!(percent_decode("%E2%82%AC"), "€");
        // A bare percent before a multi-byte character is left as it is; slicing the string would panic.
        assert_eq!(percent_decode("%€"), "%€");
    }

    #[test]
    fn a_numbered_aside_is_put_back_too() {
        let root = std::env::temp_dir().join(format!("buffrx-media-restore-n-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let output = root.join("clip.mp4");
        let aside = PathBuf::from(format!("{}.old-77-1", output.to_string_lossy()));
        std::fs::write(&aside, b"original").unwrap();
        restore_cancelled_replace(&output.to_string_lossy(), 77);
        assert_eq!(std::fs::read(&output).unwrap(), b"original");
        assert!(!aside.exists());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn the_text_gate_accepts_a_network_folder_and_refuses_a_parent_dir() {
        let root = lexical(Path::new("//nas/clips")).unwrap();
        let target = lexical(Path::new("//nas/clips/a.mp4")).unwrap();
        assert!(path_under(&target, &root));
        assert!(!path_under(&lexical(Path::new("//nas/clips2/a.mp4")).unwrap(), &root));
        assert!(lexical(Path::new("C:/clips/../Windows/x.exe")).is_none());
    }

    #[test]
    fn a_network_path_is_taken_by_text_and_never_inside_a_local_root() {
        let root = Path::new("//nas/clips");
        let target = Path::new("//nas/clips/a.mp4");
        assert!(within(root, target));
        assert!(!within(Path::new("C:/clips"), target));
        assert!(!within(root, Path::new("C:/clips/a.mp4")));
    }

    #[test]
    fn a_cancelled_replace_puts_the_original_back() {
        let root = std::env::temp_dir().join(format!("buffrx-media-restore-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let output = root.join("clip.mp4");
        let aside = PathBuf::from(format!("{}.old-77", output.to_string_lossy()));
        std::fs::write(&aside, b"original").unwrap();
        restore_cancelled_replace(&output.to_string_lossy(), 77);
        assert_eq!(std::fs::read(&output).unwrap(), b"original");
        assert!(!aside.exists());
        let _ = std::fs::remove_dir_all(&root);
    }

    /// A child that would outlive the test if nothing killed it (about 29 s of pings).
    fn slow_read_child() -> std::process::Child {
        std::process::Command::new("cmd")
            .args(["/c", "ping", "-n", "30", "127.0.0.1"])
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .spawn()
            .expect("a slow child")
    }

    #[test]
    fn a_read_found_by_its_name_is_ended_at_once() {
        let exports = Exports::default();
        let held = Arc::new(Mutex::new(Some(slow_read_child())));
        begin_read(&exports, "media-1".into(), held.clone()).unwrap();
        assert_eq!(exports.reads.lock().unwrap().len(), 1);
        let started = std::time::Instant::now();
        assert!(cancel_read(&exports, "media-1"));
        assert!(started.elapsed() < std::time::Duration::from_secs(5), "the child was killed, not waited out");
        assert!(held.lock().unwrap().is_none(), "the process was taken from its slot and ended");
        assert!(exports.reads.lock().unwrap().is_empty());
        assert!(!cancel_read(&exports, "media-1"), "a second cancel finds nothing to end");
    }

    #[test]
    fn reads_are_bounded_and_a_closed_editor_starts_none() {
        let exports = Exports::default();
        let mut held = Vec::new();
        for n in 0..MAX_READS {
            let child = Arc::new(Mutex::new(Some(slow_read_child())));
            begin_read(&exports, format!("media-{n}"), child.clone()).unwrap();
            held.push(child);
        }
        let extra = Arc::new(Mutex::new(Some(slow_read_child())));
        let refused = begin_read(&exports, "media-extra".into(), extra.clone());
        assert!(refused.is_err(), "no more reads than the registry holds: {refused:?}");
        cancel_reads(&exports);
        assert!(exports.reads.lock().unwrap().is_empty());
        assert!(held.iter().all(|child| child.lock().unwrap().is_none()), "every read was ended");
        exports.closing.store(true, std::sync::atomic::Ordering::Release);
        assert!(begin_read(&exports, "media-after".into(), extra.clone()).is_err(), "a closed editor starts no read");
        let leftover = extra.lock().unwrap().take();
        if let Some(mut child) = leftover { let _ = child.kill(); let _ = child.wait(); }
    }

    /// A child the test still holds, ended here so the suite leaves no process behind.
    fn end(child: &Arc<Mutex<Option<std::process::Child>>>) {
        if let Some(mut child) = child.lock().unwrap().take() { let _ = child.kill(); let _ = child.wait(); }
    }

    #[test]
    fn a_cancel_that_arrives_before_its_read_never_starts_one() {
        // The page's run and its cancel are two calls over an asynchronous bridge: the cancel can be
        // here before the run has registered anything.
        let exports = Exports::default();
        assert!(!cancel_read(&exports, "media-9"), "nothing is registered yet");
        let late = Arc::new(Mutex::new(Some(slow_read_child())));
        let refused = begin_read(&exports, "media-9".into(), late.clone());
        assert!(refused.is_err(), "the read that arrived late started anyway: {refused:?}");
        assert!(exports.reads.lock().unwrap().is_empty(), "and it holds no place");
        assert!(exports.cancelled.lock().unwrap().is_empty(), "the cancellation was used, not kept");
        end(&late);
        // Nothing is remembered against the name for good: asking for it again is a normal read.
        let again = Arc::new(Mutex::new(Some(slow_read_child())));
        begin_read(&exports, "media-9".into(), again.clone()).unwrap();
        assert_eq!(exports.reads.lock().unwrap().len(), 1);
        cancel_reads(&exports);
        assert!(again.lock().unwrap().is_none(), "the read that was asked for again ran and was ended");
    }

    #[test]
    fn an_early_cancel_is_remembered_for_a_bounded_number_of_names() {
        let exports = Exports::default();
        for n in 0..(MAX_CANCELLED + 4) {
            cancel_read(&exports, &format!("media-{n}"));
        }
        assert_eq!(exports.cancelled.lock().unwrap().len(), MAX_CANCELLED, "the memory is bounded");
        let oldest = Arc::new(Mutex::new(Some(slow_read_child())));
        assert!(begin_read(&exports, "media-0".into(), oldest.clone()).is_ok(), "a forgotten name is just a name");
        end(&oldest);
        assert!(begin_read(&exports, "media-999".into(), Arc::new(Mutex::new(None))).is_ok());
        cancel_reads(&exports);
    }

    #[test]
    fn a_second_read_of_a_live_name_is_refused_and_the_first_keeps_its_child() {
        let exports = Exports::default();
        let first = Arc::new(Mutex::new(Some(slow_read_child())));
        begin_read(&exports, "media-1".into(), first.clone()).unwrap();
        let second = Arc::new(Mutex::new(Some(slow_read_child())));
        let refused = begin_read(&exports, "media-1".into(), second.clone());
        assert!(refused.is_err(), "a taken name is refused, never overwritten: {refused:?}");
        assert_eq!(exports.reads.lock().unwrap().len(), 1);
        assert!(first.lock().unwrap().is_some(), "the live child is still registered under its name");
        assert!(cancel_read(&exports, "media-1"), "the name still ends the read that owns it");
        assert!(first.lock().unwrap().is_none());
        end(&second);
    }

    #[test]
    fn a_read_that_ends_does_not_free_a_newer_child_of_its_name() {
        let exports = Exports::default();
        let old = Arc::new(Mutex::new(Some(slow_read_child())));
        begin_read(&exports, "media-1".into(), old.clone()).unwrap();
        cancel_read(&exports, "media-1");
        // A name cancelled and run again while the old call is still unwinding: the newer read owns it.
        let newer = Arc::new(Mutex::new(Some(slow_read_child())));
        begin_read(&exports, "media-1".into(), newer.clone()).unwrap();
        end_read(&exports, "media-1", &old);
        assert_eq!(exports.reads.lock().unwrap().len(), 1, "the newer child kept its place");
        assert!(newer.lock().unwrap().is_some(), "and is still running");
        cancel_reads(&exports);
        assert!(newer.lock().unwrap().is_none());
    }

    #[test]
    fn a_read_cannot_start_after_the_close_and_does_not_survive_one_that_came_first() {
        // The two orders are the only ones, because the close check and the insert share the registry
        // lock: a read that inserted first is drained, and a read that arrives after the close is
        // refused. A registration parked before it takes that lock is the second order exactly.
        let exports = Exports::default();
        let registered = Arc::new(Mutex::new(Some(slow_read_child())));
        begin_read(&exports, "media-1".into(), registered.clone()).unwrap();
        cancel_all(&exports);
        assert!(registered.lock().unwrap().is_none(), "the child the close found was ended");
        let parked = Arc::new(Mutex::new(Some(slow_read_child())));
        let refused = begin_read(&exports, "media-2".into(), parked.clone());
        assert!(refused.is_err(), "a closed editor starts no read: {refused:?}");
        assert!(exports.reads.lock().unwrap().is_empty(), "and nothing survives the close");
        end(&parked);
    }

    #[test]
    fn a_close_racing_a_registration_leaves_no_child_running() {
        for round in 0..16 {
            let exports = Exports::default();
            let held = Arc::new(Mutex::new(Some(slow_read_child())));
            let racer = held.clone();
            let refused = std::thread::scope(|scope| {
                let registering = scope.spawn(|| begin_read(&exports, format!("media-{round}"), racer));
                cancel_all(&exports);
                registering.join().unwrap().is_err()
            });
            assert!(exports.reads.lock().unwrap().is_empty(), "round {round}: a read outlived the close");
            // Either the close found the child and ended it, or it arrived first and the read was
            // refused; in that second order the run that asked kills its own child, as this does.
            assert!(held.lock().unwrap().is_none() || refused, "round {round}: a child was left running after the close");
            end(&held);
        }
    }

    #[test]
    fn a_working_child_keeps_its_deadline_and_a_still_one_is_ended() {
        let root = std::env::temp_dir().join(format!("buffrx-media-progress-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let log = root.join("export.log");
        std::fs::write(&log, b"").unwrap();
        let slow_child = || {
            std::process::Command::new("cmd")
                .args(["/c", "ping", "-n", "3", "127.0.0.1"])
                .stdout(std::process::Stdio::null())
                .stderr(std::process::Stdio::null())
                .spawn()
                .expect("a slow child")
        };

        // A log that keeps growing is progress: the child runs past the quiet limit and is kept.
        let exports = Exports::default();
        start_managed(&exports, 1, slow_child(), log.to_string_lossy().into_owned()).unwrap();
        let writer = log.clone();
        let touching = std::thread::spawn(move || {
            for _ in 0..30 {
                let mut file = std::fs::OpenOptions::new().append(true).open(&writer).unwrap();
                let _ = file.write_all(b"progress\n");
                std::thread::sleep(std::time::Duration::from_millis(100));
            }
        });
        let kept = wait_managed(&exports, 1, Some(&log), std::time::Duration::from_secs(3));
        touching.join().unwrap();
        assert!(kept.is_ok(), "{kept:?}");

        // The same child with a log that never moves is ended once the quiet limit passes.
        let exports = Exports::default();
        start_managed(&exports, 2, slow_child(), log.to_string_lossy().into_owned()).unwrap();
        let ended = wait_managed(&exports, 2, Some(&log), std::time::Duration::from_millis(300));
        assert!(matches!(&ended, Err(why) if why.contains("did not finish")), "{ended:?}");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn an_image_replace_moves_over_the_original() {
        let root = std::env::temp_dir().join(format!("buffrx-media-write-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let target = root.join("shot.png");
        std::fs::write(&target, b"old").unwrap();
        let staged = target.with_extension("png.part");
        std::fs::write(&staged, b"new").unwrap();
        move_over(&staged, &target).unwrap();
        assert_eq!(std::fs::read(&target).unwrap(), b"new", "the edited image is in place");
        assert!(!staged.exists(), "and nothing is left beside it");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn a_file_a_writer_holds_is_not_served() {
        let root = std::env::temp_dir().join(format!("buffrx-media-writing-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let clip = root.join("recording.mp4");
        std::fs::write(&clip, b"prefix").unwrap();
        assert!(!still_being_written(&clip), "a closed file is served");
        let writer = std::fs::OpenOptions::new().write(true).open(&clip).unwrap();
        assert!(still_being_written(&clip), "a recording still being written is refused");
        drop(writer);
        assert!(!still_being_written(&clip), "and served once the writer lets go");
        assert!(!still_being_written(&root.join("missing.mp4")), "a missing file is the caller's 404");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn only_a_timeline_chunk_is_read_while_it_is_written() {
        assert!(is_timeline_chunk(Path::new(r"C:\Videos\BuffrX\sessions\BuffrX 2026-11-11 12-33-35\media\2026-11-11 12-37-11.mp4")));
        assert!(!is_timeline_chunk(Path::new(r"C:\Videos\BuffrX\Recordings\Recording 1.mp4")));
        assert!(!is_timeline_chunk(Path::new(r"C:\Videos\BuffrX\sessions\x\Recording 1.mp4")));
        assert!(!is_timeline_chunk(Path::new(r"C:\Videos\BuffrX\other\x\media\a.mp4")));
    }

    #[test]
    fn a_preview_copy_keeps_the_picture_shape_and_even_halves() {
        assert_eq!(preview_size(1920, 1080, 1.0), (1920, 1080));
        assert_eq!(preview_size(3840, 2160, 1.0), (1920, 1080));
        assert_eq!(preview_size(3440, 1440, 1.0), (1920, 804));
        assert_eq!(preview_size(1080, 1920, 1.0), (1080, 1920));
        assert_eq!(preview_size(1441, 1081, 1.0), (1440, 1080));
        assert_eq!(preview_size(0, 0, 1.0), (1920, 1080));
        assert_eq!(preview_size(1, 1, 1.0), (2, 2));
        // A 4:3 anamorphic frame is a 16:9 picture: the copy is square-pixel 1920x1080.
        assert_eq!(preview_size(1440, 1080, 4.0 / 3.0), (1920, 1080));
        assert_eq!(preview_size(720, 540, 4.0 / 3.0), (960, 540));
    }
}
