//! buffrx-share: BuffrX's app-to-app clip sharing, between two BuffrX installs.
//!
//! The engine starts this process only while a link is live or a clip is arriving, and talks to it
//! over stdin/stdout, one JSON object per line; it ends when the engine closes its stdin, so it can
//! never outlive the engine. All networking is iroh (QUIC): a peer is dialled by its public key, the
//! connection is end-to-end encrypted and authenticated to that key, it goes direct when hole punching
//! works and through n0's free public relays when it does not. Nothing listens on a port of ours.
//!
//! What a link grants, and nothing more: the holder may download one file, while the link has not
//! expired, within the link's viewer limits. The request is a fixed 36 bytes; an unknown link and a
//! wrong token get the same answer; a peer that keeps guessing is banned; connections, transfers and
//! the upload rate are capped. The file is chosen by the engine (library files only), never by a peer.
//!
//! Engine -> helper (stdin):
//!   {"op":"init","secret":"<64 hex>","rate_kbps":N,"proxy":"http://host:port or empty"}
//!     first line, once
//!   {"op":"serve","share":"<32 hex>","token":"<32 hex>","path":..,"name":..,"size":N,"expires":unix,
//!    "max_active":N,"max_total":N}
//!   {"op":"unserve","share":..}  {"op":"rate","kbps":N}
//!   {"op":"fetch","id":..,"endpoint":"<64 hex>","relay":"url or empty","share":..,"token":..,
//!    "dest":path,"max_bytes":N,"auto_bytes":N,"resume_tag":".."}
//!   {"op":"answer","id":..,"accept":bool}  {"op":"cancel","id":..}  {"op":"network"}  {"op":"quit"}
//! Helper -> engine (stdout):
//!   {"ev":"ready","endpoint":"<64 hex>","relay":url,"online":bool}  {"ev":"online","relay":url}
//!   {"ev":"error","msg":..}  {"ev":"log","msg":..}
//!   {"ev":"viewer","share":..,"state":"start|done|failed","bytes":N}  {"ev":"expired","share":..}
//!   {"ev":"meta","id":..,"name":..,"size":N,"expires":unix,"tag":..,"offset":N}
//!   {"ev":"progress","id":..,"bytes":N,"size":N}
//!   {"ev":"done","id":..,"bytes":N,"path":"direct|relay"}  {"ev":"failed","id":..,"reason":code}
//!
//! A failed fetch leaves its partial file where it is: the engine deletes it, or hands it back with the
//! `resume_tag` of the meta it came from, and the next fetch asks the sharer for the rest. The tag is the
//! file's size and modification time on the sharer's PC, so a clip changed in between starts again. The
//! header also carries the file's SHA-256, read once when the link is offered and cached while the file
//! keeps its size and modification time: the receiver hashes what it wrote and deletes it on a mismatch,
//! so a clip whose bytes changed on the way is never filed. A sender older than this carries no `sha256`,
//! and its file is taken as it always was.

use sha2::{Digest, Sha256};
use std::{
    collections::HashMap,
    future::IntoFuture,
    io::Write,
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, AtomicUsize, Ordering},
        Arc, Mutex,
    },
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use iroh::{
    endpoint::{presets, ConnectionError, QuicTransportConfig, ReadError, VarInt},
    Endpoint, EndpointAddr, EndpointId, RelayUrl, SecretKey,
};
use serde_json::{json, Value};
use subtle::ConstantTimeEq;
use tokio::{
    io::{AsyncBufReadExt, AsyncReadExt, AsyncSeekExt, AsyncWriteExt, BufReader},
    sync::{oneshot, Notify, OnceCell},
    time::timeout,
};

const ALPN: &[u8] = b"buffrx/share/2";
const MAGIC: &[u8; 4] = b"BXS2";
const CHUNK: usize = 64 * 1024;
/// Connections being served at once, across every link.
const MAX_CONNECTIONS: usize = 16;
/// Requests one peer may make in a minute, and wrong guesses before it is banned for a while.
const ATTEMPTS_PER_MINUTE: u32 = 20;
const STRIKES_BEFORE_BAN: u32 = 5;
const BAN: Duration = Duration::from_secs(15 * 60);
/// Wrong guesses from all peers together in a minute before every new request is turned away.
const GLOBAL_STRIKES_PER_MINUTE: u32 = 120;
const MAX_HEADER: usize = 60000;
/// A peer's guard row is remembered for this long after its last request, and for as long as its ban
/// runs, whichever is longer. Nothing else is worth remembering about a peer.
const GUARD_QUIET: Duration = Duration::from_secs(60);
/// How often the guard table is swept. A request that is not the first after this period pays for the
/// sweep, so a handshake costs the same whether one peer or ten thousand have asked.
const GUARD_SWEEP: Duration = Duration::from_secs(30);
/// Rows above this many peers send the sweep early, so a flood of fresh identities cannot grow the
/// table between two scheduled sweeps.
const GUARD_CEILING: usize = 4096;
/// The window a peer's requests are counted in.
const RATE_WINDOW: Duration = Duration::from_secs(60);
/// How long a download whose slot was given back while its user was deciding waits for a link that
/// filled up meanwhile to free a slot, and how often it looks. Under the receiver's 30 s read timeout,
/// so the FULL answer arrives before the wait is read as a stalled wire.
const SLOT_WAIT: Duration = Duration::from_secs(15);
const SLOT_POLL: Duration = Duration::from_millis(200);

/// How long a serve waits at each of its two waiting points: how long a slot is held while the
/// receiver's user decides (`idle_hold`), and how long a download that gave its slot back waits for a
/// link that filled up meantime to free one (`slot_wait`, looked at every `slot_poll`). The product runs
/// with `WAITS`; a test shortens them so a whole scenario is over in a second.
struct Waits {
    idle_hold: Duration,
    slot_wait: Duration,
    slot_poll: Duration,
}

const WAITS: Waits = Waits { idle_hold: Duration::from_secs(10), slot_wait: SLOT_WAIT, slot_poll: SLOT_POLL };

// Answers to a request. An unknown link and a wrong token are both REFUSED: the answer never tells a
// guesser which half was wrong.
const OK: u8 = 0;
const EXPIRED: u8 = 1;
const FULL: u8 = 2;
const REFUSED: u8 = 3;
const SLOW_DOWN: u8 = 4;
const GONE: u8 = 5;

fn emit(value: Value) {
    let mut out = std::io::stdout().lock();
    let _ = writeln!(out, "{value}");
    let _ = out.flush();
}

fn log(message: impl Into<String>) {
    emit(json!({"ev": "log", "msg": message.into()}));
}

fn now_unix() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

/// What a resumed download is checked against: the file's size and modification time. Two reads of the
/// same unchanged file give the same tag; a rewritten one (the editor's Replace) gives another.
fn file_tag(path: &std::path::Path, size: u64) -> String {
    let modified = std::fs::metadata(path)
        .and_then(|m| m.modified())
        .ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    format!("{size}-{modified}")
}

fn hex_array<const N: usize>(text: &str) -> Option<[u8; N]> {
    let bytes = hex::decode(text).ok()?;
    bytes.try_into().ok()
}

/// A received file is kept only when it is the one the sender promised. `None` is an old sender's
/// header, which carries no hash: nothing can be checked, so it is taken as it always was. A file that
/// does not match is deleted here, so the engine never files it and never resumes against it. Progress
/// goes to the engine every ~200 ms while the hash runs: its stall watchdog is shorter than a large
/// clip takes to read, and it must not see a finished download go quiet.
async fn check_received(path: &std::path::Path, expected: Option<&str>, id: &str, size: u64) -> Result<(), String> {
    let Some(expected) = expected else { return Ok(()) };
    // `checking` marks these lines: the bytes hashed start again from zero, and the engine's stall clock
    // only moves on new bytes, so without the mark a slow hash looked like a dead wire.
    let actual = file_sha256(path, |read| {
        emit(json!({"ev": "progress", "id": id, "bytes": read, "size": size, "checking": true}));
    })
    .await;
    if actual.is_ok_and(|actual| actual.eq_ignore_ascii_case(expected)) {
        return Ok(());
    }
    let _ = tokio::fs::remove_file(path).await;
    Err("corrupt".into())
}

/// The file's SHA-256 as 64 lower-case hex characters, read in chunks so a 4 GB clip costs 64 KB of
/// memory. `on_progress` is called with the bytes hashed so far, about five times a second.
async fn file_sha256(path: &std::path::Path, mut on_progress: impl FnMut(u64)) -> std::io::Result<String> {
    let mut file = tokio::fs::File::open(path).await?;
    let mut hasher = Sha256::new();
    let mut buffer = vec![0u8; CHUNK];
    let mut total = 0u64;
    let mut last = Instant::now();
    loop {
        let read = file.read(&mut buffer).await?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
        total += read as u64;
        if last.elapsed() >= Duration::from_millis(200) {
            last = Instant::now();
            on_progress(total);
        }
    }
    Ok(hex::encode(hasher.finalize()))
}

/// The hash a sender's header promises: `Ok(None)` for an old sender that carries none, `Err("protocol")`
/// for a header that carries something that is not a SHA-256. `Result` so the caller can tell the two
/// apart with `?`, since a file must never be taken against a hash that cannot be checked.
fn expected_sha256(header: &Value) -> Result<Option<&str>, String> {
    match header.get("sha256") {
        None => Ok(None),
        Some(value) => value
            .as_str()
            .filter(|hash| hash.len() == 64 && hash.bytes().all(|b| b.is_ascii_hexdigit()))
            .map(Some)
            .ok_or_else(|| "protocol".to_string()),
    }
}

/// Reads a share's file and fills the hash cache. Run through the share's `HashSlot`, so it happens once
/// per share however many downloads arrive, and never on the command loop.
async fn init_hash(state: &State, path: &std::path::Path) -> Result<String, String> {
    let before = file_stamp(path);
    let hash = match file_sha256(path, |_| {}).await {
        Ok(hash) => hash,
        Err(e) => {
            log(format!("a clip could not be read to hash it: {e}"));
            return Err("gone".to_string());
        }
    };
    // Only the bytes that were hashed may be cached: a file rewritten while it was read keeps its old
    // stamp and is hashed again next time.
    if let Some((size, modified)) = before {
        if file_stamp(path) == Some((size, modified)) {
            let mut cache = state.hashes.lock().unwrap();
            if cache.len() > 4096 {
                cache.clear();
            }
            cache.insert(path.to_path_buf(), (size, modified, hash.clone()));
        }
    }
    Ok(hash)
}

/// A file's size and modification time: what a cached hash is valid for.
fn file_stamp(path: &std::path::Path) -> Option<(u64, u128)> {
    let meta = std::fs::metadata(path).ok()?;
    let modified = meta.modified().ok()?.duration_since(UNIX_EPOCH).ok()?.as_nanos();
    Some((meta.len(), modified))
}

/// A cached hash is the file's hash only while the file is the same one: same size, same modification
/// time, exactly the test `file_tag` makes for a resume.
fn hash_entry_current(entry: Option<&(u64, u128, String)>, size: u64, modified: u128) -> bool {
    entry.is_some_and(|(cached_size, cached_modified, _)| *cached_size == size && *cached_modified == modified)
}

/// A share's hash, computed once: whichever of the background warm-up or the first download gets there
/// first computes it and every later download reuses it. `Err` is why the file cannot be hashed; no
/// header is ever sent without a hash.
type HashSlot = Arc<OnceCell<Result<String, String>>>;

struct Share {
    path: PathBuf,
    name: String,
    size: u64,
    /// The served file's SHA-256, as 64 hex characters, once it has been read.
    hash: HashSlot,
    /// The size and modification time the link promised. A rewrite, even one that keeps the size, ends
    /// the link as GONE instead of serving bytes the cached hash no longer describes.
    stamp: Option<(u64, u128)>,
    token: [u8; 16],
    /// A small JPEG of the clip's first frame, base64, sent in the header so the receiver sees what is
    /// coming before the first video byte. Empty when the engine could not make one.
    poster: String,
    expires: u64,
    max_active: usize,
    max_total: usize,
    active: usize,
    total: usize,
    /// Notified when the link stops serving (unserve, expiry, quit): a transfer already running ends
    /// with it instead of carrying the rest of the file.
    cancel: Arc<Notify>,
    /// Set with the notify: a transfer that reaches its next wait after the wake-up still stops.
    stopped: Arc<AtomicBool>,
}

/// Ends every transfer this link is running now, and leaves a permit for one that has not reached its
/// first wait yet. A link has at most `max_active` (four by default) of them at once.
fn stop_transfers(share: &Share) {
    share.stopped.store(true, Ordering::SeqCst);
    share.cancel.notify_waiters();
    share.cancel.notify_one();
}

/// What a share answers when a download asks to start streaming.
#[derive(Debug, PartialEq, Eq)]
enum Grant {
    /// The connection may stream, and holds a slot from here on.
    Start,
    /// Every slot is busy; nothing was taken.
    Full,
    /// The link's downloads have all been given out; nothing was taken.
    Spent,
}

/// Reserves the right to stream, for a connection that still holds the slot it was offered (`held`) or
/// for one that gave it back while its user was deciding. The cap and the budget are read and taken
/// under the share's own lock, so a waiter cannot take a slot another download has taken meanwhile:
/// that is what keeps `max_active` a promise and not a hint.
fn grant_download(share: &mut Share, held: bool) -> Grant {
    // A resume spends the budget too: counting only fresh starts let a peer resume from byte 1 for ever.
    // Conservative, and the cap is enforced whatever the offset.
    if share.total >= share.max_total {
        return Grant::Spent;
    }
    if !held && share.active >= share.max_active {
        return Grant::Full;
    }
    share.total += 1;
    if !held {
        share.active += 1;
    }
    Grant::Start
}

struct Guard {
    /// Where the peer's current request window is counted from.
    window: Instant,
    /// The peer's last request. A row is aged by this, never by its window: a peer that is still asking
    /// keeps the strikes it earned, even across the moment its window rolls over.
    last: Instant,
    attempts: u32,
    strikes: u32,
    banned_until: Option<Instant>,
}

/// Whether a peer's row is still worth keeping: while its ban runs, or while it is still asking. An
/// expired ban is not a reason to remember the peer for ever, and a row that has gone quiet says
/// nothing a fresh one would not.
fn guard_kept(guard: &Guard, now: Instant) -> bool {
    guard.banned_until.is_some_and(|until| until > now) || now.duration_since(guard.last) < GUARD_QUIET
}

/// Every peer that has asked recently, with when the table is next swept. The sweep runs on its own
/// clock, not on every request: the table used to be walked while the lock was held on every handshake,
/// which cost a receiver's first bytes time that grew with the number of peers the sharer had met.
struct Guards {
    rows: HashMap<EndpointId, Guard>,
    next_sweep: Instant,
}

impl Guards {
    fn new(now: Instant) -> Self {
        Guards { rows: HashMap::new(), next_sweep: now + GUARD_SWEEP }
    }

    /// Drops the rows nothing is worth remembering, at most once per `GUARD_SWEEP`.
    fn sweep(&mut self, now: Instant) {
        if now < self.next_sweep && self.rows.len() <= GUARD_CEILING {
            return;
        }
        self.rows.retain(|_, guard| guard_kept(guard, now));
        self.next_sweep = now + GUARD_SWEEP;
    }

    /// Whether a peer may make a request now. Banned peers and peers over the per-minute budget may not.
    fn allow(&mut self, peer: EndpointId, now: Instant) -> bool {
        self.sweep(now);
        let guard = self.rows.entry(peer).or_insert(Guard {
            window: now,
            last: now,
            attempts: 0,
            strikes: 0,
            banned_until: None,
        });
        if let Some(until) = guard.banned_until {
            if now < until {
                return false;
            }
            guard.banned_until = None;
            guard.strikes = 0;
        }
        if now.duration_since(guard.window) > RATE_WINDOW {
            guard.window = now;
            guard.attempts = 0;
        }
        guard.last = now;
        guard.attempts += 1;
        guard.attempts <= ATTEMPTS_PER_MINUTE
    }

    /// A wrong guess. Enough of them and the peer is banned.
    fn strike(&mut self, peer: EndpointId, now: Instant) {
        if let Some(guard) = self.rows.get_mut(&peer) {
            guard.last = now;
            guard.strikes += 1;
            if guard.strikes >= STRIKES_BEFORE_BAN {
                guard.banned_until = Some(now + BAN);
                log(format!("peer {} banned for 15 min after repeated wrong links", peer.fmt_short()));
            }
        }
    }
}

struct Bucket {
    per_sec: u64,
    tokens: f64,
    last: Instant,
}

struct Fetch {
    answer: Option<oneshot::Sender<bool>>,
    cancel: Arc<Notify>,
}

struct State {
    shares: Mutex<HashMap<[u8; 16], Share>>,
    guards: Mutex<Guards>,
    global: Mutex<(Instant, u32)>,
    connections: AtomicUsize,
    bucket: Mutex<Bucket>,
    fetches: Mutex<HashMap<String, Fetch>>,
    /// Hashes already taken, by path: (size, modification time, hash). The engine re-offers every live
    /// link on each helper start, so without this a restart would read every clip again.
    hashes: Mutex<HashMap<PathBuf, (u64, u128, String)>>,
    /// The serve side's waiting times: the product's, or a test's shorter ones.
    waits: Waits,
}

impl State {
    /// Whether a peer may make a request now. Banned peers and peers over the per-minute budget may not.
    fn allow(&self, peer: EndpointId) -> bool {
        self.guards.lock().unwrap().allow(peer, Instant::now())
    }

    /// Whether wrong guesses from everyone together are under the global budget. Peer identities are
    /// free to make, so a guesser can dodge the per-peer ban; this bounds the work they cause. The
    /// token (128 random bits) is what actually keeps them out.
    fn guessing_allowed(&self) -> bool {
        let mut global = self.global.lock().unwrap();
        if global.0.elapsed() > Duration::from_secs(60) {
            *global = (Instant::now(), 0);
        }
        global.1 < GLOBAL_STRIKES_PER_MINUTE
    }

    /// A wrong guess. Enough of them and the peer is banned.
    fn strike(&self, peer: EndpointId) {
        self.global.lock().unwrap().1 += 1;
        self.guards.lock().unwrap().strike(peer, Instant::now());
    }

    /// Waits until `bytes` may go out under the upload cap.
    async fn pace(&self, bytes: usize) {
        loop {
            let wait = {
                let mut bucket = self.bucket.lock().unwrap();
                if bucket.per_sec == 0 {
                    return;
                }
                let now = Instant::now();
                let elapsed = now.duration_since(bucket.last).as_secs_f64();
                bucket.last = now;
                // A quarter of a second of burst at most, so a pause never turns into a flood.
                let ceiling = bucket.per_sec as f64 / 4.0 + bytes as f64;
                bucket.tokens = (bucket.tokens + elapsed * bucket.per_sec as f64).min(ceiling);
                if bucket.tokens >= bytes as f64 {
                    bucket.tokens -= bytes as f64;
                    return;
                }
                Duration::from_secs_f64((bytes as f64 - bucket.tokens) / bucket.per_sec as f64)
            };
            tokio::time::sleep(wait).await;
        }
    }
}

/// A reserved download slot on a share, given back however the transfer ends.
struct Slot {
    state: Arc<State>,
    share: [u8; 16],
}

impl Drop for Slot {
    fn drop(&mut self) {
        if let Some(share) = self.state.shares.lock().unwrap().get_mut(&self.share) {
            share.active = share.active.saturating_sub(1);
        }
    }
}

// ---------------------------------------------------------------- serving

async fn serve(state: Arc<State>, incoming: iroh::endpoint::Incoming) -> Result<(), String> {
    let connection = timeout(Duration::from_secs(15), incoming.into_future())
        .await
        .map_err(|_| "handshake timed out".to_string())?
        .map_err(|e| e.to_string())?;
    let peer = connection.remote_id();
    if !state.allow(peer) {
        connection.close(SLOW_DOWN.into(), b"slow down");
        return Ok(());
    }
    // The global budget slows every new request instead of turning it away: a leaked link's holder
    // cannot silence the sharer's other links, and a legitimate receiver waits at most two seconds.
    if !state.guessing_allowed() {
        tokio::time::sleep(Duration::from_secs(2)).await;
    }
    let (mut send, mut recv) = timeout(Duration::from_secs(10), connection.accept_bi())
        .await
        .map_err(|_| "no request".to_string())?
        .map_err(|e| e.to_string())?;
    let mut request = [0u8; 36];
    timeout(Duration::from_secs(10), recv.read_exact(&mut request))
        .await
        .map_err(|_| "request timed out".to_string())?
        .map_err(|e| e.to_string())?;
    if &request[..4] != MAGIC {
        state.strike(peer);
        connection.close(REFUSED.into(), b"refused");
        return Ok(());
    }
    let share_id: [u8; 16] = request[4..20].try_into().unwrap();
    let token: [u8; 16] = request[20..36].try_into().unwrap();

    let (status, granted) = {
        let mut shares = state.shares.lock().unwrap();
        match shares.get_mut(&share_id) {
            Some(share) if bool::from(share.token.ct_eq(&token)) => {
                if now_unix() >= share.expires {
                    (EXPIRED, None)
                } else if share.active >= share.max_active || share.total >= share.max_total {
                    (FULL, None)
                } else if file_stamp(&share.path) != share.stamp {
                    // Deleted or rewritten since the link was made: what was promised is not there.
                    (GONE, None)
                } else {
                    // A slot is held while the file is offered to the receiver; `total` is counted only
                    // once it actually starts downloading (below), so a peer that never answers cannot
                    // spend the link's budget.
                    share.active += 1;
                    (OK, Some((share.path.clone(), share.name.clone(), share.size, share.hash.clone(), share.expires, share.poster.clone(), share.cancel.clone(), share.stopped.clone())))
                }
            }
            // Present the token check as the same work for a link that does not exist.
            _ => {
                let _ = [0u8; 16].ct_eq(&token);
                (REFUSED, None)
            }
        }
    };
    if status == REFUSED {
        state.strike(peer);
    }
    let Some((path, name, size, hash, expires, poster, cancel, stopped)) = granted else {
        send.write_all(&[status]).await.map_err(|e| e.to_string())?;
        let _ = send.finish();
        let _ = timeout(Duration::from_secs(5), connection.closed()).await;
        return Ok(());
    };
    // Held while the hash is awaited: a file that cannot be hashed gives the slot back with this return.
    let mut slot = Some(Slot { state: state.clone(), share: share_id });
    // No header leaves here without a hash. An old helper would have no `sha256` for us to send, but this
    // one always does; a file that cannot be read is not offered at all. GONE is a reason the engine
    // already knows: what the link promised is not there.
    let hash = match hash.get_or_init(|| init_hash(&state, &path)).await {
        Ok(hash) => hash.clone(),
        Err(e) => {
            log(format!("link {}: {e}", hex::encode(share_id)));
            let _ = send.write_all(&[GONE]).await;
            let _ = send.finish();
            // Until the receiver has read it: dropping the connection here closes it under the answer,
            // and the receiver then reports "refused" for a file that is simply gone.
            let _ = timeout(Duration::from_secs(5), connection.closed()).await;
            return Ok(());
        }
    };
    if stopped.load(Ordering::SeqCst) {
        let _ = send.finish();
        return Ok(());
    }
    send.write_all(&[OK]).await.map_err(|e| e.to_string())?;
    let mut header = serde_json::to_vec(&json!({"name": name, "size": size, "expires": expires, "tag": file_tag(&path, size), "sha256": hash, "poster": poster})).unwrap();
    // The length goes on the wire as a u16. A poster that would push the header past it is dropped
    // rather than wrapped: a truncated length would desynchronise the stream and kill the transfer.
    if header.len() >= MAX_HEADER {
        header = serde_json::to_vec(&json!({"name": name, "size": size, "expires": expires, "tag": file_tag(&path, size), "sha256": hash})).unwrap();
    }
    send.write_all(&(header.len() as u16).to_be_bytes()).await.map_err(|e| e.to_string())?;
    send.write_all(&header).await.map_err(|e| e.to_string())?;

    // The receiver says whether it wants the file (it may ask its user first), and from which byte: a
    // download that broke part-way asks only for the rest. A request that says nothing for 10 s gives its
    // slot back (`active`), so idle peers cannot hold every download the link has, but it keeps its place
    // in the queue: someone still choosing whether to take a large clip is not cut off.
    let deadline = Instant::now() + Duration::from_secs(150);
    let mut go = [0u8; 9];
    let mut filled = 0;
    while filled < go.len() {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            break;
        }
        tokio::select! {
            // `read` is cancellation safe: what arrived stays in `go` when the idle wait wins.
            result = timeout(remaining, recv.read(&mut go[filled..])) => match result {
                Ok(Ok(Some(read))) if read > 0 => filled += read,
                _ => break,
            },
            _ = tokio::time::sleep(state.waits.idle_hold) => slot = None,
            _ = cancel.notified() => {
                // The link was stopped or expired: nobody is served from here any more.
                let _ = send.finish();
                return Ok(());
            }
        }
    }
    if filled < go.len() || go[0] != 1 {
        let _ = send.finish();
        return Ok(());
    }
    let offset = u64::from_be_bytes(go[1..9].try_into().unwrap());
    if offset > size {
        let _ = send.finish();
        return Err("asked to resume past the end".into());
    }
    // The download starts here, so this is what spends the link's budget. A slot given back while the
    // receiver was choosing is taken again, and the cap is checked again on the same lock: a link that
    // filled up meanwhile is waited out for a while, then answered FULL, which the receiver reads as
    // "try again", rather than streaming past the number of viewers the link was made with.
    let wait_until = Instant::now() + state.waits.slot_wait;
    loop {
        let (grant, gone) = {
            let mut shares = state.shares.lock().unwrap();
            match shares.get_mut(&share_id) {
                Some(share) => (grant_download(share, slot.is_some()), false),
                // The link is gone: it was notified when it stopped, and the copy below ends on that.
                None => (Grant::Start, true),
            }
        };
        match grant {
            Grant::Start => {
                if !gone && slot.is_none() {
                    slot = Some(Slot { state: state.clone(), share: share_id });
                }
                break;
            }
            Grant::Full if !stopped.load(Ordering::SeqCst) && Instant::now() < wait_until => {
                tokio::select! {
                    _ = tokio::time::sleep(state.waits.slot_poll) => {}
                    // Stopped or expired while waiting for a slot: nothing to serve any more.
                    _ = cancel.notified() => {}
                }
            }
            Grant::Full => {
                // Every slot is busy. The answer rides on the close code: finishing the stream first
                // would let the receiver read end-of-stream and call the clip incomplete instead of
                // "try again". An older receiver reads either as a failed download, which is what it
                // always did with a link it could not be served from.
                if !stopped.load(Ordering::SeqCst) {
                    connection.close(FULL.into(), b"full");
                }
                return Ok(());
            }
            Grant::Spent => {
                // Every download this link allows has been given out.
                let _ = send.finish();
                return Ok(());
            }
        }
    }
    if stopped.load(Ordering::SeqCst) {
        // Stopped or expired while waiting for a slot: the link serves nobody from here.
        let _ = send.finish();
        return Ok(());
    }
    let _slot = slot;
    let share_hex = hex::encode(share_id);
    let mut file = tokio::fs::File::open(&path).await.map_err(|e| e.to_string())?;
    file.seek(std::io::SeekFrom::Start(offset)).await.map_err(|e| e.to_string())?;
    // The route, so the sharer's own sheet can say a relay is why it is slow.
    let via = if connection.paths().iter().any(|p| p.is_relay() && p.is_selected()) { "relay" } else { "direct" };
    // Say a viewer started only once the file is really open: an open error must not leave a count.
    emit(json!({"ev": "viewer", "share": share_hex, "state": "start", "bytes": offset, "via": via}));
    let mut buffer = vec![0u8; CHUNK];
    let mut sent: u64 = offset;
    let outcome: Result<(), String> = async {
        while sent < size {
            if stopped.load(Ordering::SeqCst) {
                return Err("cancelled".to_string());
            }
            let step = async {
                let want = CHUNK.min((size - sent) as usize);
                let read = file.read(&mut buffer[..want]).await.map_err(|e| e.to_string())?;
                if read == 0 {
                    return Err("the file got shorter".to_string());
                }
                state.pace(read).await;
                timeout(Duration::from_secs(60), send.write_all(&buffer[..read]))
                    .await
                    .map_err(|_| "the receiver stopped reading".to_string())?
                    .map_err(|e| e.to_string())?;
                Ok(read as u64)
            };
            let written: u64 = tokio::select! {
                result = step => result?,
                // The link was stopped or expired: the rest of the file is not sent.
                _ = cancel.notified() => return Err("cancelled".to_string()),
            };
            sent += written;
        }
        send.finish().map_err(|e| e.to_string())?;
        // Wait for the receiver to close, so every byte is delivered before this side goes.
        let _ = timeout(Duration::from_secs(60), connection.closed()).await;
        Ok(())
    }
    .await;
    let state_name = if outcome.is_ok() { "done" } else { "failed" };
    emit(json!({"ev": "viewer", "share": share_hex, "state": state_name, "bytes": sent, "via": via}));
    outcome
}

// ---------------------------------------------------------------- fetching

struct FetchRequest {
    id: String,
    endpoint: EndpointId,
    relay: Option<RelayUrl>,
    share: [u8; 16],
    token: [u8; 16],
    dest: PathBuf,
    max_bytes: u64,
    auto_bytes: u64,
    resume_tag: String,
}

fn reason_for(status: u8) -> &'static str {
    match status {
        EXPIRED => "expired",
        FULL => "full",
        SLOW_DOWN => "busy",
        GONE => "gone",
        _ => "refused",
    }
}

/// Whether the sharer closed the connection because every download slot it has is busy. That answer
/// can only come after the header (the slots are re-checked when the receiver says yes), so it rides on
/// the close code rather than on a status byte: "full" tells the engine to say "try again in a minute".
fn closed_full(error: &ReadError) -> bool {
    matches!(
        error,
        ReadError::ConnectionLost(ConnectionError::ApplicationClosed(close)) if close.error_code == VarInt::from(FULL)
    )
}

async fn fetch(state: Arc<State>, endpoint: Endpoint, request: FetchRequest, answer: oneshot::Receiver<bool>, cancel: Arc<Notify>) {
    let id = request.id.clone();
    let result = tokio::select! {
        result = fetch_inner(&endpoint, &request, answer) => result,
        _ = cancel.notified() => Err("cancelled".to_string()),
    };
    match result {
        Ok((bytes, path)) => emit(json!({"ev": "done", "id": id, "bytes": bytes, "path": path})),
        // The partial file stays: the engine either resumes it or deletes it.
        Err(reason) => emit(json!({"ev": "failed", "id": id, "reason": reason})),
    }
    state.fetches.lock().unwrap().remove(&id);
}

async fn fetch_inner(endpoint: &Endpoint, request: &FetchRequest, answer: oneshot::Receiver<bool>) -> Result<(u64, &'static str), String> {
    let mut addr = EndpointAddr::new(request.endpoint);
    if let Some(relay) = &request.relay {
        addr = addr.with_relay_url(relay.clone());
    }
    let connection = timeout(Duration::from_secs(30), endpoint.connect(addr, ALPN))
        .await
        .map_err(|_| "unreachable".to_string())?
        .map_err(|_| "unreachable".to_string())?;
    let (mut send, mut recv) = connection.open_bi().await.map_err(|_| "unreachable".to_string())?;
    let mut message = Vec::with_capacity(36);
    message.extend_from_slice(MAGIC);
    message.extend_from_slice(&request.share);
    message.extend_from_slice(&request.token);
    send.write_all(&message).await.map_err(|_| "unreachable".to_string())?;

    let mut status = [0u8; 1];
    timeout(Duration::from_secs(180), recv.read_exact(&mut status))
        .await
        .map_err(|_| "unreachable".to_string())?
        .map_err(|_| "refused".to_string())?;
    if status[0] != OK {
        return Err(reason_for(status[0]).to_string());
    }
    let mut length = [0u8; 2];
    timeout(Duration::from_secs(20), recv.read_exact(&mut length))
        .await
        .map_err(|_| "stalled".to_string())?
        .map_err(|_| "protocol".to_string())?;
    let length = u16::from_be_bytes(length) as usize;
    if length == 0 || length > MAX_HEADER {
        return Err("protocol".into());
    }
    let mut header = vec![0u8; length];
    timeout(Duration::from_secs(20), recv.read_exact(&mut header))
        .await
        .map_err(|_| "stalled".to_string())?
        .map_err(|_| "protocol".to_string())?;
    let header: Value = serde_json::from_slice(&header).map_err(|_| "protocol".to_string())?;
    let name = header["name"].as_str().unwrap_or("").chars().take(200).collect::<String>();
    let size = header["size"].as_u64().ok_or("protocol")?;
    let expires = header["expires"].as_u64().unwrap_or(0);
    let tag = header["tag"].as_str().unwrap_or("").chars().take(64).collect::<String>();
    // The sender's poster rides in the header, so the receiver has it before the first video byte.
    // Optional by design: an old sender has none, and one that is there but unusable is dropped below.
    let poster = header["poster"].as_str().unwrap_or("").chars().take(40000).collect::<String>();
    // An old sender's header has no hash, and is accepted as legacy. One that is there but is not a SHA-256
    // is not something we can check the file against, so the file is not taken.
    let expected = expected_sha256(&header)?;
    if size == 0 || size > request.max_bytes {
        let _ = send.write_all(&[0; 9]).await;
        return Err("too_large".into());
    }
    // What a broken attempt left is kept only if it is the same file, and no longer than it.
    let mut offset = 0u64;
    if !tag.is_empty() && tag == request.resume_tag {
        if let Ok(meta) = tokio::fs::metadata(&request.dest).await {
            if meta.len() <= size {
                offset = meta.len();
            }
        }
    }
    // The route, said while the bytes are still on their way: a relay is a fraction of a direct path's
    // speed, so a slow transfer is explained by this line rather than guessed at.
    let via = if connection.paths().iter().any(|p| p.is_relay() && p.is_selected()) { "relay" } else { "direct" };
    emit(json!({"ev": "meta", "id": request.id, "name": name, "size": size, "expires": expires, "tag": tag, "offset": offset, "poster": poster, "via": via}));
    // Small clips start at once; a large one waits for the user's yes.
    let accept = if size <= request.auto_bytes {
        true
    } else {
        timeout(Duration::from_secs(140), answer).await.ok().and_then(|r| r.ok()).unwrap_or(false)
    };
    if !accept {
        let _ = send.write_all(&[0; 9]).await;
        let _ = send.finish();
        return Err("declined".into());
    }
    let mut go = [1u8; 9];
    go[1..9].copy_from_slice(&offset.to_be_bytes());
    send.write_all(&go).await.map_err(|_| "unreachable".to_string())?;
    let _ = send.finish();

    let mut file = if offset > 0 {
        let mut file = tokio::fs::OpenOptions::new().write(true).open(&request.dest).await.map_err(|_| "io".to_string())?;
        file.set_len(offset).await.map_err(|_| "io".to_string())?;
        file.seek(std::io::SeekFrom::Start(offset)).await.map_err(|_| "io".to_string())?;
        file
    } else {
        tokio::fs::File::create(&request.dest).await.map_err(|_| "io".to_string())?
    };
    let mut buffer = vec![0u8; CHUNK];
    let mut received: u64 = offset;
    let mut last_report = Instant::now();
    while received < size {
        let want = CHUNK.min((size - received) as usize);
        let read = timeout(Duration::from_secs(30), recv.read(&mut buffer[..want]))
            .await
            .map_err(|_| "stalled".to_string())?
            .map_err(|e| if closed_full(&e) { "full".to_string() } else { "stalled".to_string() })?;
        // None is the peer closing, Some(0) an empty read: either way the rest of the file is gone.
        match read {
            None | Some(0) => {
                drop(file);
                return Err("incomplete".into());
            }
            Some(read) => {
                file.write_all(&buffer[..read]).await.map_err(|_| "io".to_string())?;
                received += read as u64;
            }
        }
        if last_report.elapsed() > Duration::from_millis(200) {
            last_report = Instant::now();
            emit(json!({"ev": "progress", "id": request.id, "bytes": received, "size": size}));
        }
    }
    // Anything past the promised size is not read: the stream is simply closed.
    file.flush().await.map_err(|_| "io".to_string())?;
    drop(file);
    if received != size {
        return Err("incomplete".into());
    }
    // What arrived must be the file the sender promised, or it is deleted and never filed. Progress keeps
    // the engine's stall watchdog from mistaking the hash for a dead transfer.
    check_received(&request.dest, expected, &request.id, size).await?;
    let path = if connection.paths().iter().any(|p| p.is_relay() && p.is_selected()) { "relay" } else { "direct" };
    connection.close(0u32.into(), b"done");
    Ok((received, path))
}

// ---------------------------------------------------------------- main

#[tokio::main]
async fn main() {
    let mut lines = BufReader::new(tokio::io::stdin()).lines();
    let Ok(Some(first)) = lines.next_line().await else { return };
    let init: Value = serde_json::from_str(&first).unwrap_or(Value::Null);
    let Some(secret) = init["secret"].as_str().and_then(hex_array::<32>) else {
        emit(json!({"ev": "error", "msg": "the helper was started without its key"}));
        std::process::exit(2);
    };
    let state = Arc::new(State {
        shares: Mutex::new(HashMap::new()),
        guards: Mutex::new(Guards::new(Instant::now())),
        global: Mutex::new((Instant::now(), 0)),
        connections: AtomicUsize::new(0),
        bucket: Mutex::new(Bucket {
            // 0 is Unlimited: nothing is paced at all.
            per_sec: init["rate_kbps"].as_u64().unwrap_or(0) * 1000 / 8,
            tokens: 0.0,
            last: Instant::now(),
        }),
        fetches: Mutex::new(HashMap::new()),
        hashes: Mutex::new(HashMap::new()),
        waits: WAITS,
    });

    // Probe only (BUFFRX_SHARE_RELAY_ONLY=1): every byte goes through the relay, as on a network that
    // blocks UDP. The product always prefers a direct connection, so a share is never slowed by a relay
    // it does not need.
    let relay_only = std::env::var("BUFFRX_SHARE_RELAY_ONLY").is_ok_and(|v| v == "1");
    // The transport's own default is sized for a 100 Mbit line at 100 ms: its 1.25 MB stream window
    // becomes a hard 40 Mbit ceiling over an intercontinental 250 ms, whatever the wire underneath is.
    // 8 MB covers 250 ms at 250 Mbit, and the send buffer twice that; a faster or closer link buffers
    // no more than what actually arrives, so the room costs nothing until it is used.
    let transport = QuicTransportConfig::builder()
        .stream_receive_window(VarInt::from_u32(8 * 1024 * 1024))
        .send_window(2 * 8 * 1024 * 1024)
        .build();
    let make = |proxy: Option<url::Url>| {
        let mut builder = Endpoint::builder(presets::N0)
            .secret_key(SecretKey::from_bytes(&secret))
            .alpns(vec![ALPN.to_vec()])
            .transport_config(transport.clone());
        if relay_only {
            builder = builder.clear_ip_transports();
        }
        if let Some(url) = proxy {
            builder = builder.proxy_url(url);
        }
        // Tests only: no DNS lookup of peers, which proves a link's relay address is enough on its own.
        if std::env::var("BUFFRX_SHARE_NO_LOOKUP").is_ok_and(|v| v == "1") {
            builder = builder.clear_address_lookup();
        }
        builder
    };
    let bind = |builder: iroh::endpoint::Builder| async move {
        match builder.bind().await {
            Ok(endpoint) => endpoint,
            Err(e) => {
                emit(json!({"ev": "error", "msg": format!("the network could not start: {e}")}));
                std::process::exit(3);
            }
        }
    };
    // Behind a proxy (a school's or an office's) the relay is reached through it: Windows' own proxy
    // setting as the engine read it, else the usual environment variables. A proxy that is set but does
    // not work (a leftover debugging proxy, a VPN's local one that is off) must not cost the relay that
    // works without it: after 10 s through the proxy the helper tries again without.
    let proxy = init["proxy"]
        .as_str()
        .filter(|p| !p.is_empty())
        .and_then(|p| p.parse().ok())
        .or_else(|| ["HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy"].iter().find_map(|k| {
            std::env::var(k).ok().filter(|v| !v.is_empty()).and_then(|v| v.parse().ok())
        }));
    let mut endpoint = bind(make(proxy.clone())).await;
    let mut online = timeout(Duration::from_secs(if proxy.is_some() { 10 } else { 20 }), endpoint.online()).await.is_ok();
    if !online && proxy.is_some() {
        log("no relay through the proxy; trying without it");
        endpoint.close().await;
        endpoint = bind(make(None)).await;
        online = timeout(Duration::from_secs(20), endpoint.online()).await.is_ok();
    }
    let relay = endpoint.addr().relay_urls().next().map(|u| u.to_string()).unwrap_or_default();
    emit(json!({"ev": "ready", "endpoint": hex::encode(endpoint.id().as_bytes()), "relay": relay, "online": online}));
    if !online {
        // Fetching still works (it dials the sharer's relay from the link), but a link of this PC's
        // cannot be found until a relay is: the engine is told the moment one is.
        log("no relay reachable yet");
        let endpoint = endpoint.clone();
        tokio::spawn(async move {
            endpoint.online().await;
            let relay = endpoint.addr().relay_urls().next().map(|u| u.to_string()).unwrap_or_default();
            emit(json!({"ev": "online", "relay": relay}));
        });
    }

    // Serving.
    {
        let state = state.clone();
        let endpoint = endpoint.clone();
        tokio::spawn(async move {
            while let Some(incoming) = endpoint.accept().await {
                if state.connections.load(Ordering::SeqCst) >= MAX_CONNECTIONS {
                    incoming.refuse();
                    continue;
                }
                state.connections.fetch_add(1, Ordering::SeqCst);
                let state = state.clone();
                tokio::spawn(async move {
                    if let Err(e) = serve(state.clone(), incoming).await {
                        log(format!("a transfer ended early: {e}"));
                    }
                    state.connections.fetch_sub(1, Ordering::SeqCst);
                });
            }
        });
    }
    // Expired links are dropped every 15 s even when nobody asks for them.
    {
        let state = state.clone();
        tokio::spawn(async move {
            loop {
                tokio::time::sleep(Duration::from_secs(15)).await;
                let now = now_unix();
                let mut gone = Vec::new();
                state.shares.lock().unwrap().retain(|id, share| {
                    let keep = share.expires > now;
                    if !keep {
                        // An expired link stops serving at once, whatever is still downloading.
                        stop_transfers(share);
                        gone.push(hex::encode(id));
                    }
                    keep
                });
                for share in gone {
                    emit(json!({"ev": "expired", "share": share}));
                }
            }
        });
    }

    // Commands from the engine, until it closes stdin.
    while let Ok(Some(line)) = lines.next_line().await {
        let command: Value = match serde_json::from_str(&line) {
            Ok(value) => value,
            Err(_) => continue,
        };
        match command["op"].as_str().unwrap_or("") {
            "serve" => {
                let (Some(id), Some(token)) = (
                    command["share"].as_str().and_then(hex_array::<16>),
                    command["token"].as_str().and_then(hex_array::<16>),
                ) else {
                    continue;
                };
                let path = PathBuf::from(command["path"].as_str().unwrap_or(""));
                // Taken once and kept: the engine re-sends every live link on each helper start, so a
                // restart reuses the hash while the file has the same size and modification time. Nothing
                // is awaited here: the command loop keeps reading commands, and the first download (or the
                // warm-up task below) waits for the one read.
                let hash: HashSlot = Arc::new(OnceCell::new());
                let stamp = file_stamp(&path);
                let cached = stamp.and_then(|(size, modified)| {
                    let cache = state.hashes.lock().unwrap();
                    match cache.get(&path) {
                        Some(entry) if hash_entry_current(Some(entry), size, modified) => Some(entry.2.clone()),
                        _ => None,
                    }
                });
                if let Some(hash_hex) = cached {
                    let _ = hash.set(Ok(hash_hex));
                } else {
                    let hashing = state.clone();
                    let hashing_path = path.clone();
                    let hashing_slot = hash.clone();
                    // Through the slot, so a download that arrives mid-read waits for this one read
                    // instead of starting its own.
                    tokio::spawn(async move {
                        let _ = hashing_slot.get_or_init(|| init_hash(&hashing, &hashing_path)).await;
                    });
                }
                state.shares.lock().unwrap().insert(
                    id,
                    Share {
                        path,
                        name: command["name"].as_str().unwrap_or("clip").chars().take(200).collect(),
                        size: command["size"].as_u64().unwrap_or(0),
                        hash,
                        stamp,
                        token,
                        poster: command["poster"].as_str().unwrap_or("").chars().take(40000).collect(),
                        expires: command["expires"].as_u64().unwrap_or(0),
                        max_active: command["max_active"].as_u64().unwrap_or(4) as usize,
                        max_total: command["max_total"].as_u64().unwrap_or(25) as usize,
                        active: 0,
                        total: 0,
                        cancel: Arc::new(Notify::new()),
                        stopped: Arc::new(AtomicBool::new(false)),
                    },
                );
            }
            "unserve" => {
                if let Some(id) = command["share"].as_str().and_then(hex_array::<16>) {
                    // A download already running stops now, not when it happens to end on its own.
                    if let Some(share) = state.shares.lock().unwrap().remove(&id) {
                        stop_transfers(&share);
                    }
                }
            }
            "rate" => {
                let mut bucket = state.bucket.lock().unwrap();
                bucket.per_sec = command["kbps"].as_u64().unwrap_or(0) * 1000 / 8;
            }
            "fetch" => {
                let id = command["id"].as_str().unwrap_or("").to_string();
                let endpoint_id = command["endpoint"]
                    .as_str()
                    .and_then(hex_array::<32>)
                    .and_then(|bytes| EndpointId::from_bytes(&bytes).ok());
                let share = command["share"].as_str().and_then(hex_array::<16>);
                let token = command["token"].as_str().and_then(hex_array::<16>);
                let (Some(endpoint_id), Some(share), Some(token), false) = (endpoint_id, share, token, id.is_empty()) else {
                    emit(json!({"ev": "failed", "id": id, "reason": "bad_link"}));
                    continue;
                };
                let relay = command["relay"].as_str().filter(|r| !r.is_empty()).and_then(|r| r.parse::<RelayUrl>().ok());
                let (answer_tx, answer_rx) = oneshot::channel();
                let cancel = Arc::new(Notify::new());
                state.fetches.lock().unwrap().insert(id.clone(), Fetch { answer: Some(answer_tx), cancel: cancel.clone() });
                let request = FetchRequest {
                    id,
                    endpoint: endpoint_id,
                    relay,
                    share,
                    token,
                    dest: PathBuf::from(command["dest"].as_str().unwrap_or("")),
                    max_bytes: command["max_bytes"].as_u64().unwrap_or(0),
                    auto_bytes: command["auto_bytes"].as_u64().unwrap_or(0),
                    resume_tag: command["resume_tag"].as_str().unwrap_or("").to_string(),
                };
                tokio::spawn(fetch(state.clone(), endpoint.clone(), request, answer_rx, cancel));
            }
            "answer" => {
                let id = command["id"].as_str().unwrap_or("");
                let accept = command["accept"].as_bool().unwrap_or(false);
                if let Some(fetch) = state.fetches.lock().unwrap().get_mut(id) {
                    if let Some(answer) = fetch.answer.take() {
                        let _ = answer.send(accept);
                    }
                }
            }
            "cancel" => {
                let id = command["id"].as_str().unwrap_or("");
                if let Some(fetch) = state.fetches.lock().unwrap().get(id) {
                    fetch.cancel.notify_one();
                }
            }
            // The PC woke from sleep or changed networks: look for paths again now, not on the next timer.
            "network" => endpoint.network_change().await,
            // The engine is closing the helper: every transfer still running ends with it.
            "quit" => {
                for share in state.shares.lock().unwrap().values() {
                    stop_transfers(share);
                }
                break;
            }
            _ => {}
        }
    }
    endpoint.close().await;
}

#[cfg(test)]
mod tests {
    use super::*;
    use sha2::{Digest, Sha256};
    use tokio::task::JoinHandle;

    fn scratch(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("buffrx-share-test-{}-{name}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir.join("clip.mp4")
    }

    fn hash_of(bytes: &[u8]) -> String {
        let mut hasher = Sha256::new();
        hasher.update(bytes);
        hex::encode(hasher.finalize())
    }

    #[tokio::test]
    async fn a_file_that_matches_its_hash_is_accepted() {
        let path = scratch("clean");
        tokio::fs::write(&path, b"buffrx clip bytes").await.unwrap();
        let expected = hash_of(b"buffrx clip bytes");
        assert_eq!(check_received(&path, Some(&expected), "t", 17).await, Ok(()));
        assert!(path.exists(), "the file that matched its hash stays for filing");
    }

    #[tokio::test]
    async fn a_same_size_rewrite_is_rejected_and_deleted() {
        let path = scratch("rewrite");
        let expected = hash_of(b"AAAAAAAA");
        tokio::fs::write(&path, b"BBBBBBBB").await.unwrap();
        assert_eq!(check_received(&path, Some(&expected), "t", 8).await, Err("corrupt".to_string()));
        assert!(!path.exists(), "a file that does not match its hash is never filed");
    }

    #[tokio::test]
    async fn a_truncated_file_is_rejected_and_deleted() {
        let path = scratch("truncated");
        let expected = hash_of(b"0123456789");
        tokio::fs::write(&path, b"01234").await.unwrap();
        assert_eq!(check_received(&path, Some(&expected), "t", 10).await, Err("corrupt".to_string()));
        assert!(!path.exists(), "a resumed file that cannot meet the hash is not kept");
    }

    #[tokio::test]
    async fn a_resumed_prefix_that_does_not_match_is_rejected_and_deleted() {
        let path = scratch("resumed");
        let expected = hash_of(b"0123456789");
        // The size the sender promised, with the prefix a resume kept from a file that changed: only a
        // hash over the whole file can see this, and it is why the hash covers the part never streamed.
        tokio::fs::write(&path, b"XXXXX56789").await.unwrap();
        assert_eq!(check_received(&path, Some(&expected), "t", 10).await, Err("corrupt".to_string()));
        assert!(!path.exists(), "a resumed file whose prefix is wrong is deleted");
    }

    #[tokio::test]
    async fn an_old_sender_without_a_hash_is_accepted() {
        let path = scratch("legacy");
        tokio::fs::write(&path, b"from a BuffrX before the hash").await.unwrap();
        assert_eq!(check_received(&path, None, "t", 29).await, Ok(()));
        assert!(path.exists(), "a link made before the hash still delivers its clip");
    }

    #[test]
    fn a_malformed_hash_header_is_refused() {
        let mut header = json!({});
        assert_eq!(expected_sha256(&header), Ok(None));
        header["sha256"] = json!(hash_of(b"x"));
        assert!(expected_sha256(&header).unwrap().is_some());
        header["sha256"] = json!("abc");
        assert_eq!(expected_sha256(&header), Err("protocol".to_string()));
        header["sha256"] = json!(format!("g{}", "0".repeat(63)));
        assert_eq!(expected_sha256(&header), Err("protocol".to_string()), "not a hex digit is not a SHA-256");
        header["sha256"] = json!(7);
        assert_eq!(expected_sha256(&header), Err("protocol".to_string()));
    }

    #[test]
    fn a_cached_hash_is_reused_only_for_the_same_file() {
        let entry = (10u64, 1234u128, "abc".to_string());
        assert!(hash_entry_current(Some(&entry), 10, 1234), "same size and time reuses the hash");
        assert!(!hash_entry_current(Some(&entry), 11, 1234), "another size hashes again");
        assert!(!hash_entry_current(Some(&entry), 10, 1235), "another time hashes again");
        assert!(!hash_entry_current(None, 10, 1234), "nothing cached hashes");
    }

    // ------------------------------------------------------------ the download slot

    fn share_for(max_active: usize, max_total: usize, active: usize, total: usize) -> Share {
        Share {
            path: PathBuf::from("clip.mp4"),
            name: "clip".to_string(),
            size: 10,
            hash: Arc::new(OnceCell::new()),
            stamp: None,
            token: [0u8; 16],
            poster: String::new(),
            expires: u64::MAX,
            max_active,
            max_total,
            active,
            total,
            cancel: Arc::new(Notify::new()),
            stopped: Arc::new(AtomicBool::new(false)),
        }
    }

    #[test]
    fn a_slot_given_back_is_not_taken_again_past_the_cap() {
        // The receiver gave its slot back while its user chose, and another download took the last one.
        let mut share = share_for(1, 10, 1, 1);
        assert_eq!(grant_download(&mut share, false), Grant::Full);
        assert_eq!(share.active, 1, "a full link still streams for one viewer at a time");
        assert_eq!(share.total, 1, "and a download that cannot start spends nothing");
    }

    #[test]
    fn a_held_slot_starts_without_taking_a_second_one() {
        let mut share = share_for(2, 10, 2, 1);
        assert_eq!(grant_download(&mut share, true), Grant::Start);
        assert_eq!(share.active, 2, "the slot it holds is the one it streams under");
        assert_eq!(share.total, 2, "the download still spends the link's budget");
    }

    #[test]
    fn waiters_cannot_fill_a_link_past_its_cap() {
        let mut share = share_for(2, 10, 0, 0);
        assert_eq!(grant_download(&mut share, false), Grant::Start);
        assert_eq!(grant_download(&mut share, false), Grant::Start);
        assert_eq!(grant_download(&mut share, false), Grant::Full, "the third waiter is turned away");
        assert_eq!(share.active, 2, "never more than max_active streams at once");
        assert_eq!(share.total, 2);
    }

    #[test]
    fn a_spent_link_takes_no_slot() {
        let mut share = share_for(4, 2, 0, 2);
        assert_eq!(grant_download(&mut share, true), Grant::Spent, "the budget is checked before the cap");
        assert_eq!(share.active, 0);
        assert_eq!(share.total, 2);
    }

    // ------------------------------------------------------------ the peer guard table

    fn secs(n: u64) -> Duration {
        Duration::from_secs(n)
    }

    fn test_state(waits: Waits) -> State {
        State {
            shares: Mutex::new(HashMap::new()),
            guards: Mutex::new(Guards::new(Instant::now())),
            global: Mutex::new((Instant::now(), 0)),
            connections: AtomicUsize::new(0),
            bucket: Mutex::new(Bucket { per_sec: 0, tokens: 0.0, last: Instant::now() }),
            fetches: Mutex::new(HashMap::new()),
            hashes: Mutex::new(HashMap::new()),
            waits,
        }
    }

    fn peer(n: u8) -> EndpointId {
        SecretKey::from_bytes(&[n; 32]).public()
    }

    fn row(last: Instant, strikes: u32, ban: Option<Instant>) -> Guard {
        Guard { window: last, last, attempts: 0, strikes, banned_until: ban }
    }

    #[test]
    fn an_expired_ban_is_forgotten_and_a_running_one_is_not() {
        let t0 = Instant::now();
        let mut guards = Guards::new(t0);
        guards.rows.insert(peer(1), row(t0 - secs(180), 5, Some(t0 - secs(1))));
        guards.rows.insert(peer(2), row(t0 - secs(180), 5, Some(t0 + secs(600))));
        guards.rows.insert(peer(3), row(t0 - secs(180), 0, None));
        guards.rows.insert(peer(4), row(t0 - secs(10), 5, Some(t0 - secs(1))));
        // A request once the sweep is due sweeps the table.
        assert!(guards.allow(peer(9), t0 + GUARD_SWEEP + secs(1)));
        assert!(!guards.rows.contains_key(&peer(1)), "a ban that is over and a peer that has gone quiet is forgotten");
        assert!(guards.rows.contains_key(&peer(2)), "a ban still running is kept however quiet the peer is");
        assert!(!guards.rows.contains_key(&peer(3)), "a quiet peer with no ban is forgotten, as before");
        assert!(guards.rows.contains_key(&peer(4)), "a peer seen in the last minute keeps its row");
        assert!(guards.rows.contains_key(&peer(9)));
    }

    #[test]
    fn a_ban_still_running_turns_the_peer_away() {
        let t0 = Instant::now();
        let mut guards = Guards::new(t0);
        guards.rows.insert(peer(1), row(t0, 5, Some(t0 + BAN)));
        assert!(!guards.allow(peer(1), t0 + secs(1)), "a banned peer may not ask again");
        assert!(!guards.allow(peer(1), t0 + GUARD_SWEEP + secs(1)), "and a sweep does not forget it while the ban runs");
    }

    #[test]
    fn an_expired_ban_is_cleared_when_its_peer_comes_back() {
        let t0 = Instant::now();
        let mut guards = Guards::new(t0);
        guards.rows.insert(peer(1), row(t0 - secs(1), 5, Some(t0 - secs(1))));
        assert!(guards.allow(peer(1), t0));
        let guard = guards.rows.get(&peer(1)).unwrap();
        assert_eq!(guard.banned_until, None);
        assert_eq!(guard.strikes, 0, "a ban that is over does not come back with the peer");
        assert_eq!(guard.attempts, 1);
    }

    #[test]
    fn an_active_peer_keeps_its_strikes_across_rate_windows() {
        let t0 = Instant::now();
        let mut guards = Guards::new(t0);
        let p = peer(1);
        for _ in 0..3 {
            assert!(guards.allow(p, t0));
            guards.strike(p, t0);
        }
        // The peer keeps asking, so its row is never quiet while its request window rolls over.
        assert!(guards.allow(p, t0 + secs(30)));
        assert!(guards.allow(p, t0 + secs(59)));
        // Another peer's request sweeps the table a minute after the first: the active peer keeps its row,
        // because its row is aged by its last request and not by the start of its window.
        assert!(guards.allow(peer(2), t0 + secs(61)));
        let guard = guards.rows.get(&p).expect("a peer that is still asking is never forgotten");
        assert_eq!(guard.strikes, 3, "the strikes it earned are still there");
        // Its next request is the first of a new minute, so its budget starts again, and its strikes do not.
        assert!(guards.allow(p, t0 + secs(62)));
        assert_eq!(guards.rows[&p].attempts, 1, "the new minute's budget starts again");
        // So the two wrong guesses it needs for a ban are still two, not five again.
        assert!(guards.allow(p, t0 + secs(62)));
        guards.strike(p, t0 + secs(62));
        assert_eq!(guards.rows[&p].strikes, 4);
        assert!(guards.rows[&p].banned_until.is_none());
        assert!(guards.allow(p, t0 + secs(62)));
        guards.strike(p, t0 + secs(62));
        assert!(guards.rows[&p].banned_until.is_some(), "an active peer still earns its ban");
    }

    #[test]
    fn the_table_is_swept_on_a_schedule_not_on_every_request() {
        let t0 = Instant::now();
        let mut guards = Guards::new(t0);
        let old = peer(7);
        guards.rows.insert(old, row(t0 - secs(3600), 0, None));
        assert!(guards.allow(peer(1), t0));
        assert!(guards.rows.contains_key(&old), "a request is not charged for the whole table");
        assert!(guards.allow(peer(2), t0 + GUARD_SWEEP));
        assert!(!guards.rows.contains_key(&old), "the sweep drops what is not worth keeping");
        guards.rows.insert(old, row(t0 - secs(3600), 0, None));
        assert!(guards.allow(peer(3), t0 + GUARD_SWEEP + secs(1)));
        assert!(guards.rows.contains_key(&old), "and the next one is a period later, not a request later");
        assert!(guards.allow(peer(4), t0 + 2 * GUARD_SWEEP));
        assert!(!guards.rows.contains_key(&old));
    }

    #[test]
    fn a_flood_of_identities_cannot_grow_the_table_between_sweeps() {
        let t0 = Instant::now();
        let mut guards = Guards::new(t0);
        for n in 0..=GUARD_CEILING {
            let mut key = [0u8; 32];
            key[..8].copy_from_slice(&(n as u64).to_le_bytes());
            key[31] = 1;
            guards.rows.insert(SecretKey::from_bytes(&key).public(), row(t0 - secs(3600), 0, None));
        }
        assert!(guards.allow(peer(1), t0 + secs(1)), "a flood above the ceiling sends the sweep early");
        assert!(guards.rows.len() <= 2, "and the table goes back to the peers that are still asking");
    }

    // ------------------------------------------------------------ a link that fills up

    const SHARE: [u8; 16] = [7; 16];
    const TOKEN: [u8; 16] = [9; 16];
    const CLIP: &[u8] = b"a shared clip";
    /// A receiver's yes: this is the answer to the header, and byte 0 of it is the only non-zero one.
    const GO: [u8; 9] = [1, 0, 0, 0, 0, 0, 0, 0, 0];

    /// The product's waits, a second long instead of half a minute: the same states, reached faster.
    fn fast_waits() -> Waits {
        Waits { idle_hold: Duration::from_millis(300), slot_wait: Duration::from_millis(600), slot_poll: Duration::from_millis(10) }
    }

    fn scratch_part(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("buffrx-share-test-{}-{name}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir.join("clip.part")
    }

    /// Offers one clip on the state's only link, with the slots the test asks for.
    async fn offered(state: &State, path: PathBuf, max_active: usize) {
        tokio::fs::write(&path, CLIP).await.unwrap();
        state.shares.lock().unwrap().insert(
            SHARE,
            Share {
                path: path.clone(),
                name: "clip.mp4".to_string(),
                size: CLIP.len() as u64,
                hash: Arc::new(OnceCell::new()),
                stamp: file_stamp(&path),
                token: TOKEN,
                poster: String::new(),
                expires: u64::MAX,
                max_active,
                max_total: 10,
                active: 0,
                total: 0,
                cancel: Arc::new(Notify::new()),
                stopped: Arc::new(AtomicBool::new(false)),
            },
        );
    }

    /// A helper on a loopback endpoint, serving every connection through `serve`, and a client that
    /// finds it by its id exactly as a receiver finds a sharer. Nothing listens off this machine.
    async fn link(state: Arc<State>) -> (Endpoint, EndpointAddr) {
        let server = Endpoint::builder(presets::Minimal).alpns(vec![ALPN.to_vec()]).bind().await.unwrap();
        let addr = server.addr();
        let lookup = iroh::address_lookup::MemoryLookup::new();
        lookup.add_endpoint_info(addr.clone());
        let client = Endpoint::builder(presets::Minimal).address_lookup(lookup).bind().await.unwrap();
        tokio::spawn(async move {
            while let Some(incoming) = server.accept().await {
                let state = state.clone();
                tokio::spawn(async move {
                    let _ = serve(state, incoming).await;
                });
            }
        });
        (client, addr)
    }

    /// Waits for the served link to reach a state, instead of sleeping for a guessed time.
    async fn until(state: &State, held: impl Fn(&Share) -> bool) {
        let deadline = Instant::now() + Duration::from_secs(10);
        loop {
            if state.shares.lock().unwrap().values().any(|share| held(share)) {
                return;
            }
            assert!(Instant::now() < deadline, "the link never reached the state the test waited for");
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
    }

    /// A receiver's own half of the wire, for the checks that read the answer by hand.
    struct Asking {
        send: iroh::endpoint::SendStream,
        recv: iroh::endpoint::RecvStream,
        _connection: iroh::endpoint::Connection,
    }

    async fn ask(client: &Endpoint, addr: &EndpointAddr) -> Asking {
        let connection = client.connect(addr.clone(), ALPN).await.expect("the link answers");
        let (mut send, recv) = connection.open_bi().await.unwrap();
        let mut request = Vec::new();
        request.extend_from_slice(MAGIC);
        request.extend_from_slice(&SHARE);
        request.extend_from_slice(&TOKEN);
        send.write_all(&request).await.unwrap();
        Asking { send, recv, _connection: connection }
    }

    /// The file a link offers, read as the receiver reads it: status, then the header.
    async fn header(recv: &mut iroh::endpoint::RecvStream) -> Value {
        let mut status = [0u8; 1];
        timeout(Duration::from_secs(10), recv.read_exact(&mut status)).await.expect("a status").unwrap();
        assert_eq!(status[0], OK, "the link offers its file");
        let mut length = [0u8; 2];
        timeout(Duration::from_secs(10), recv.read_exact(&mut length)).await.expect("a length").unwrap();
        let mut body = vec![0u8; u16::from_be_bytes(length) as usize];
        timeout(Duration::from_secs(10), recv.read_exact(&mut body)).await.expect("a header").unwrap();
        serde_json::from_slice(&body).unwrap()
    }

    /// The receiver the product runs, held at the moment its user is deciding whether to take the clip,
    /// and then asking to start after another download has taken the link's only slot. The receiver is
    /// returned with its request on the wire and its answer still to be read.
    async fn link_that_fills_up(name: &str) -> (Endpoint, Asking, JoinHandle<()>) {
        let state = Arc::new(test_state(fast_waits()));
        offered(&state, scratch(&format!("{name}-src")), 1).await;
        let (client, addr) = link(state.clone()).await;
        // A download whose slot is held while its user decides, and given back, as after ten seconds.
        let mut waiter = ask(&client, &addr).await;
        header(&mut waiter.recv).await;
        until(&state, |share| share.active == 0).await;
        // Another download takes the link's only slot meanwhile.
        let holder = tokio::spawn({
            let client = client.clone();
            let addr = addr.clone();
            async move {
                let mut holder = ask(&client, &addr).await;
                header(&mut holder.recv).await;
                holder.send.write_all(&GO).await.unwrap();
                // Held until the task is dropped with the test: this is the download the link streams.
                holder._connection.closed().await;
            }
        });
        until(&state, |share| share.active == 1).await;
        // The first receiver's user says yes: it asks for the file it can no longer be given.
        waiter.send.write_all(&GO).await.unwrap();
        (client, waiter, holder)
    }

    #[tokio::test]
    async fn a_receiver_that_waited_for_its_slot_is_told_the_link_is_full() {
        let state = Arc::new(test_state(fast_waits()));
        offered(&state, scratch("full-src"), 1).await;
        let dest = scratch_part("full-dest");
        let (client, addr) = link(state.clone()).await;
        let request = FetchRequest {
            id: "full".to_string(),
            endpoint: addr.id,
            relay: None,
            share: SHARE,
            token: TOKEN,
            dest: dest.clone(),
            max_bytes: 1 << 20,
            auto_bytes: 0,
            resume_tag: String::new(),
        };
        let (answer, answer_rx) = oneshot::channel();
        let receiving = tokio::spawn({
            let client = client.clone();
            async move { fetch_inner(&client, &request, answer_rx).await }
        });
        // Its slot is held while its user decides, and given back before the answer comes.
        until(&state, |share| share.active == 1).await;
        until(&state, |share| share.active == 0).await;
        // The link fills up with another download before that user says yes.
        let mut holder = ask(&client, &addr).await;
        header(&mut holder.recv).await;
        holder.send.write_all(&GO).await.unwrap();
        until(&state, |share| share.active == 1).await;
        answer.send(true).unwrap();
        let result = timeout(Duration::from_secs(10), receiving).await.expect("the receiver answers").unwrap();
        assert_eq!(result, Err("full".to_string()), "the link filled up, so the engine says to try again in a minute");
    }

    #[tokio::test]
    async fn the_full_answer_rides_on_the_close_and_not_on_the_end_of_the_stream() {
        let (_client, mut waiter, _holder) = link_that_fills_up("close").await;
        let mut buffer = [0u8; 8];
        let read = timeout(Duration::from_secs(10), waiter.recv.read(&mut buffer)).await.expect("the link answers");
        match read {
            Err(error) => assert!(closed_full(&error), "a full link says so on the close: {error:?}"),
            Ok(read) => panic!("the receiver read {read:?} instead of the close, and would call the clip incomplete"),
        }
    }

    #[tokio::test]
    async fn an_older_receiver_fails_cleanly_on_a_link_that_filled_up() {
        let (_client, mut waiter, _holder) = link_that_fills_up("legacy").await;
        let mut buffer = [0u8; 8];
        // What an older receiver did with this read: a read error was a stalled wire, an empty read an
        // incomplete file. Either way it fails with a reason the engine knows, and keeps nothing.
        let reason = match timeout(Duration::from_secs(10), waiter.recv.read(&mut buffer)).await.expect("the link answers") {
            Ok(None) | Ok(Some(0)) => "incomplete",
            Ok(Some(_)) => panic!("a full link sent bytes it has no slot for"),
            Err(_) => "stalled",
        };
        assert!(matches!(reason, "incomplete" | "stalled"), "an older receiver fails cleanly: {reason}");
    }

    #[tokio::test]
    async fn a_request_on_a_full_link_still_gets_the_status_an_older_receiver_reads() {
        let state = Arc::new(test_state(fast_waits()));
        offered(&state, scratch("status-src"), 1).await;
        let (client, addr) = link(state.clone()).await;
        let mut holder = ask(&client, &addr).await;
        header(&mut holder.recv).await;
        holder.send.write_all(&GO).await.unwrap();
        until(&state, |share| share.active == 1).await;
        let mut newcomer = ask(&client, &addr).await;
        let mut status = [0u8; 1];
        timeout(Duration::from_secs(10), newcomer.recv.read_exact(&mut status)).await.expect("a status").unwrap();
        assert_eq!(status[0], FULL);
        assert_eq!(reason_for(status[0]), "full", "the answer an older receiver already understood");
    }

    #[tokio::test]
    async fn a_stopped_link_ends_a_download_that_is_waiting_for_a_slot() {
        let state = Arc::new(test_state(fast_waits()));
        offered(&state, scratch("stop-src"), 1).await;
        let dest = scratch_part("stop-dest");
        let (client, addr) = link(state.clone()).await;
        let request = FetchRequest {
            id: "stop".to_string(),
            endpoint: addr.id,
            relay: None,
            share: SHARE,
            token: TOKEN,
            dest: dest.clone(),
            max_bytes: 1 << 20,
            auto_bytes: 0,
            resume_tag: String::new(),
        };
        let (answer, answer_rx) = oneshot::channel();
        let receiving = tokio::spawn({
            let client = client.clone();
            async move { fetch_inner(&client, &request, answer_rx).await }
        });
        until(&state, |share| share.active == 1).await;
        until(&state, |share| share.active == 0).await;
        let mut holder = ask(&client, &addr).await;
        header(&mut holder.recv).await;
        holder.send.write_all(&GO).await.unwrap();
        until(&state, |share| share.active == 1).await;
        // Its user says yes just as the link is stopped: nothing is streamed, and the wait ends.
        answer.send(true).unwrap();
        let stopped = state.shares.lock().unwrap().remove(&SHARE).expect("the link is still offered");
        stop_transfers(&stopped);
        let result = timeout(Duration::from_secs(10), receiving).await.expect("the wait ends with the link").unwrap();
        let reason = result.expect_err("a download that waited on a stopped link does not complete");
        // The link's end is a wire that stops answering, which is what the engine already handles: the
        // partial file is kept and the next attempt resumes it.
        assert_eq!(reason, "stalled", "a stopped link ends the wait, without a word about slots");
    }

    // ------------------------------------------------------------ a full link's answer

    #[test]
    fn a_full_sender_is_told_apart_from_a_dead_wire() {
        use iroh::endpoint::ApplicationClose;
        let closed = |code: u8| {
            ReadError::ConnectionLost(ConnectionError::ApplicationClosed(ApplicationClose {
                error_code: VarInt::from(code),
                reason: Vec::<u8>::new().into(),
            }))
        };
        assert!(closed_full(&closed(FULL)), "a full link is answered as full, not as a stall");
        assert!(!closed_full(&closed(SLOW_DOWN)), "another close code says nothing about the slots");
        assert!(!closed_full(&ReadError::ConnectionLost(ConnectionError::TimedOut)));
        assert!(!closed_full(&ReadError::ClosedStream));
        assert_eq!(reason_for(FULL), "full", "the engine says to try again in a minute");
    }
}
