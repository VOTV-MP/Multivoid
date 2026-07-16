//! Production async HTTP master / lobby server for VOTV coop (zero-open-ports MP).
//!
//! Rust port of `tools/coop_master_server.py`, WIRE-COMPATIBLE with it (identical
//! JSON endpoints, field names, and the byte-exact coturn TURN credential) so the
//! Python and Rust masters can run in parallel on different ports during cutover.
//! RULE 3: VPS infra, never ships in the mod. See the Python file's module docstring
//! + research/findings/network/votv-master-server-RE-and-rust-port-scope-2026-07-16.md
//! for the endpoint list, the security design, and the careful spots.
//!
//! Concurrency model: a single `Mutex<MasterState>` guards the lobby maps + rate
//! buckets + the /v1/lobbies cache. Handlers are SYNCHRONOUS (pure CPU) and run
//! entirely under the lock; every socket await (read body, write response) happens
//! OUTSIDE the lock. This mirrors the Python asyncio single-loop model — no lock is
//! ever held across an await — while still using the multi-thread tokio runtime.

use coop_server::common::{
    clamp_str, ct_eq, env_int, env_str, log, token_hex, token_urlsafe, turn_creds,
};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::net::Ipv6Addr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{LazyLock, Mutex, MutexGuard};
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::time::timeout;

// ---- limits / tunables (mirror the Python constants) ------------------------

const HTTP_TIMEOUT: Duration = Duration::from_secs(15);
const MAX_HEADER: usize = 16 * 1024;
const MAX_BODY: usize = 64 * 1024;
const MAX_CONNS: usize = 256;

const TURN_TTL: u64 = 120;
const LOBBY_TTL: Duration = Duration::from_secs(300);
const LOBBIES_CACHE_TTL: Duration = Duration::from_secs(5);

const MAX_LOBBIES_PER_IP: usize = 8;
const MAX_LOBBIES_GLOBAL: usize = 1000;
// Hard cap on distinct rate-limit buckets, so a source-diverse flood can't grow the
// `rate` map toward MemoryMax between the 30s sweeps (security audit 2026-07-16, M3).
// With /64 IPv6 coarsening a flood needs distinct routable prefixes to add keys, so
// this is a belt-and-suspenders ceiling; when hit, new buckets are refused (the
// flooding prefixes are simply treated as rate-limited).
const MAX_RATE_KEYS: usize = 50_000;

// per-(IP, class) sliding-window rate limits: (window, max events).
const RL_CREATE: (Duration, usize) = (Duration::from_secs(60), 10);
const RL_JOIN: (Duration, usize) = (Duration::from_secs(60), 20);
const RL_MUTATE: (Duration, usize) = (Duration::from_secs(60), 240);

const MAX_NAME: usize = 63;
const MAX_WORLD: usize = 39;
const MAX_VERSION: usize = 23;

// latest released mod (operator-maintained; bump on each mod release).
const LATEST_PROTO: i64 = 66;
const LATEST_MOD: &str = "0.9.0-n";
const LATEST_URL: &str = "https://github.com/pelmentor/VOTV_MP/releases";

const TRUSTED_PROXY_PEERS: [&str; 2] = ["127.0.0.1", "::1"];

// ---- config -----------------------------------------------------------------

struct Config {
    port: u16,
    turn_secret: String,
    signaling_token: String,
    signaling_url: String,
    stun_uri: String,
    turn_uri: String,
}

impl Config {
    fn from_env() -> Config {
        Config {
            port: env_int("COOP_MASTER_PORT", 10001) as u16,
            turn_secret: env_str("COOP_TURN_SECRET", ""),
            signaling_token: env_str("COOP_SIGNALING_TOKEN", ""),
            signaling_url: env_str("COOP_SIGNALING_URL", ""),
            stun_uri: env_str("COOP_STUN_URI", ""),
            turn_uri: env_str("COOP_TURN_URI", ""),
        }
    }
}

static CFG: LazyLock<Config> = LazyLock::new(Config::from_env);
static CONNS: AtomicUsize = AtomicUsize::new(0);
static STATE: LazyLock<Mutex<MasterState>> = LazyLock::new(|| Mutex::new(MasterState::new()));

/// Lock STATE, tolerating poisoning (audit M4): under panic=unwind a handler that
/// panics while holding the lock would poison it; recovering the inner guard keeps
/// one bad request from bricking every subsequent handler. The state is structurally
/// usable post-panic (handlers only mutate maps, no half-torn invariants).
fn lock_state() -> MutexGuard<'static, MasterState> {
    STATE.lock().unwrap_or_else(|e| e.into_inner())
}

// ---- state ------------------------------------------------------------------

struct Lobby {
    session_id: String,
    lobby_id: String,
    token: String,
    host_identity: String,
    name: String,
    version: String,
    proto: i64,
    world: String,
    locked: bool,
    players_cur: i64,
    players_max: i64,
    listed: bool,
    last_seen: Instant,
    ip: String,
    conn: String, // "p2p" | "direct"
    direct_port: i64,
}

impl Lobby {
    fn new(ip: &str) -> Lobby {
        Lobby {
            session_id: token_hex(16),
            lobby_id: token_hex(8),
            token: token_urlsafe(24),
            host_identity: format!("h{}", token_hex(8)),
            name: String::new(),
            version: String::new(),
            proto: 0,
            world: String::new(),
            locked: false,
            players_cur: 0,
            players_max: 4,
            listed: true,
            last_seen: Instant::now(),
            ip: ip.to_string(),
            conn: "p2p".to_string(),
            direct_port: 0,
        }
    }
}

struct MasterState {
    lobbies: HashMap<String, Lobby>,      // sessionId -> Lobby
    lobby_by_public: HashMap<String, String>, // lobbyId -> sessionId
    rate: HashMap<String, Vec<Instant>>,  // "ipbucket|class" -> recent timestamps
    cache_t: Option<Instant>,
    // Arc so /v1/lobbies can clone the cached body/rows cheaply under the lock and
    // serialize the response OUTSIDE it (audit L5: serializing a ~200 KB body under
    // the global lock serialized it against all host/join/heartbeat processing).
    cache_rows: Arc<Vec<Value>>,
    cache_all_body: Arc<Vec<u8>>,
}

impl MasterState {
    fn new() -> MasterState {
        MasterState {
            lobbies: HashMap::new(),
            lobby_by_public: HashMap::new(),
            rate: HashMap::new(),
            cache_t: None,
            cache_rows: Arc::new(Vec::new()),
            cache_all_body: Arc::new(Vec::new()),
        }
    }
}

// ---- JSON value coercion (match Python's tolerant int()/bool()/str()) --------

fn coerce_str(v: Option<&Value>) -> String {
    match v {
        Some(Value::String(s)) => s.clone(),
        None | Some(Value::Null) => String::new(),
        Some(Value::Bool(b)) => if *b { "True".into() } else { "False".into() },
        Some(Value::Number(n)) => n.to_string(),
        Some(other) => other.to_string(),
    }
}

/// clamp a body string field: coerce -> strip control chars -> take `maxlen` chars.
fn clamp_field(body: &Value, key: &str, maxlen: usize) -> String {
    clamp_str(&coerce_str(body.get(key)), maxlen)
}

/// Python `int(body.get(key, default))` with its try/except -> default fallback.
fn as_int(body: &Value, key: &str, default: i64) -> i64 {
    match body.get(key) {
        Some(Value::Number(n)) => n
            .as_i64()
            .or_else(|| n.as_f64().map(|f| f as i64))
            .unwrap_or(default),
        Some(Value::String(s)) => s.trim().parse::<i64>().unwrap_or(default),
        _ => default,
    }
}

/// Python truthiness of `body.get(key, default)`.
fn as_bool(body: &Value, key: &str, default: bool) -> bool {
    match body.get(key) {
        None | Some(Value::Null) => default,
        Some(Value::Bool(b)) => *b,
        Some(Value::Number(n)) => n.as_f64().map(|f| f != 0.0).unwrap_or(default),
        Some(Value::String(s)) => !s.is_empty(),
        Some(Value::Array(a)) => !a.is_empty(),
        Some(Value::Object(o)) => !o.is_empty(),
    }
}

fn body_has(body: &Value, key: &str) -> bool {
    body.get(key).is_some()
}

fn as_str<'a>(body: &'a Value, key: &str) -> Option<&'a str> {
    body.get(key).and_then(|v| v.as_str())
}

// ---- helpers ----------------------------------------------------------------

fn resolve_client_ip(peer_ip: &str, headers: &HashMap<String, String>) -> String {
    if TRUSTED_PROXY_PEERS.contains(&peer_ip) {
        if let Some(xr) = headers.get("x-real-ip") {
            let xr = xr.trim();
            if !xr.is_empty() {
                return xr.to_string();
            }
        }
        if let Some(xff) = headers.get("x-forwarded-for") {
            let parts: Vec<&str> = xff.split(',').map(|p| p.trim()).filter(|p| !p.is_empty()).collect();
            if let Some(last) = parts.last() {
                return last.to_string(); // rightmost = what the trusted proxy observed
            }
        }
    }
    peer_ip.to_string()
}

/// Coarsen a client address to the allocation an attacker realistically controls, so
/// abuse controls (rate limits, lobby caps, TURN-cred accounting) can't be bypassed by
/// address rotation (audit M2/M3): a full IPv4 address, or an IPv6 /64 (the smallest
/// prefix a host is routably assigned — a single tenant typically owns a whole /64).
fn ip_bucket(ip: &str) -> String {
    if ip.contains(':') {
        if let Ok(v6) = ip.parse::<Ipv6Addr>() {
            let s = v6.segments();
            return format!("{:x}:{:x}:{:x}:{:x}::/64", s[0], s[1], s[2], s[3]);
        }
    }
    ip.to_string()
}

fn rate_ok(state: &mut MasterState, ip: &str, cls: &str, window: Duration, limit: usize) -> bool {
    let key = format!("{}|{}", ip_bucket(ip), cls);
    let now = Instant::now();
    // Bound the map: refuse a brand-new bucket once at the ceiling (audit M3) so a
    // source-diverse flood can't outgrow the 30s sweeper. Existing buckets still work.
    if !state.rate.contains_key(&key) && state.rate.len() >= MAX_RATE_KEYS {
        return false;
    }
    let bucket = state.rate.entry(key).or_default();
    bucket.retain(|ts| now.duration_since(*ts) < window);
    if bucket.len() >= limit {
        return false;
    }
    bucket.push(now);
    true
}

/// The connectivity block every host/join response carries. `turn_label` is the
/// coarse per-client identity the TURN username is bound to (audit M2). NOTE: coturn's
/// REST username is "<exp>:<label>" with a per-mint expiry, so coturn cannot aggregate
/// `user-quota` on the label — the EFFECTIVE per-source bound on cred minting is the
/// master's per-/64 rate limit on /v1/join (RL_JOIN) plus coturn's global total-quota
/// + per-session max-bps + aggregate bps-capacity. Binding the label to the IP bucket
/// (vs a fresh-random per-mint identity) removes the "unique identity per mint" faucet
/// framing and gives coherent per-source attribution; it is not the quota enforcer.
fn ice_block(turn_label: &str) -> serde_json::Map<String, Value> {
    let mut m = serde_json::Map::new();
    m.insert("signalingUrl".into(), json!(CFG.signaling_url));
    m.insert("signalingToken".into(), json!(CFG.signaling_token));
    m.insert("stun".into(), json!(CFG.stun_uri));
    let turn = turn_creds(&CFG.turn_uri, &CFG.turn_secret, turn_label, TURN_TTL).unwrap_or_else(|| json!({}));
    m.insert("turn".into(), turn);
    m
}

fn drop_lobby(state: &mut MasterState, session_id: &str, lobby_id: &str) {
    state.lobbies.remove(session_id);
    state.lobby_by_public.remove(lobby_id);
}

fn evict_if_full(state: &mut MasterState) {
    while state.lobbies.len() >= MAX_LOBBIES_GLOBAL {
        // drop the stalest last_seen
        let stalest = state
            .lobbies
            .values()
            .min_by_key(|lo| lo.last_seen)
            .map(|lo| (lo.session_id.clone(), lo.lobby_id.clone()));
        match stalest {
            Some((sid, pub_id)) => {
                drop_lobby(state, &sid, &pub_id);
                log(&format!("evicted stalest lobby {pub_id} (global cap)"));
            }
            None => break,
        }
    }
}

fn lobbies_per_ip(state: &MasterState, ip: &str) -> usize {
    // Count by the coarse bucket (audit M3), not the exact address, so an IPv6 /64
    // can't mint MAX_LOBBIES_PER_IP lobbies per /128.
    let bucket = ip_bucket(ip);
    state.lobbies.values().filter(|lo| ip_bucket(&lo.ip) == bucket).count()
}

fn auth_by_session<'a>(state: &'a MasterState, body: &Value) -> Option<&'a Lobby> {
    let sid = as_str(body, "sessionId")?;
    let tok = as_str(body, "token")?;
    let lo = state.lobbies.get(sid)?;
    if ct_eq(tok.as_bytes(), lo.token.as_bytes()) {
        Some(lo)
    } else {
        None
    }
}

// ---- endpoint handlers : return (status, body) ------------------------------

fn h_host(state: &mut MasterState, ip: &str, body: &Value) -> (u16, Value) {
    if !rate_ok(state, ip, "create", RL_CREATE.0, RL_CREATE.1) {
        return (429, json!({"error": "rate"}));
    }
    if lobbies_per_ip(state, ip) >= MAX_LOBBIES_PER_IP {
        return (429, json!({"error": "too many lobbies for this address"}));
    }
    evict_if_full(state);

    let mut lo = Lobby::new(ip);
    let name = clamp_field(body, "name", MAX_NAME);
    lo.name = if name.is_empty() { "VOTV Coop".into() } else { name };
    let version = clamp_field(body, "version", MAX_VERSION);
    lo.version = if version.is_empty() { "0.0.0".into() } else { version };
    lo.proto = as_int(body, "proto", 0).clamp(0, 65535);
    lo.world = clamp_field(body, "world", MAX_WORLD);
    lo.locked = as_bool(body, "locked", false);
    lo.players_max = as_int(body, "players_max", 4).clamp(1, 4);

    if as_str(body, "conn") == Some("direct") {
        let dp = as_int(body, "direct_port", -1);
        if !(1024..=65535).contains(&dp) {
            // REJECT loudly BEFORE registering (audit R1): a silent p2p downgrade
            // would hand joiners ICE creds a LanDirect host can't speak.
            return (400, json!({"error": "direct_port out of range (1024-65535)"}));
        }
        lo.conn = "direct".into();
        lo.direct_port = dp;
    }
    lo.players_cur = 1;
    lo.last_seen = Instant::now();

    let session_id = lo.session_id.clone();
    let lobby_id = lo.lobby_id.clone();
    let host_identity = lo.host_identity.clone();
    let token = lo.token.clone();
    let conn = lo.conn.clone();
    let is_direct = conn == "direct";

    state.lobby_by_public.insert(lobby_id.clone(), session_id.clone());
    state.lobbies.insert(session_id.clone(), lo);
    log(&format!(
        "host {} '{}' v{} from {} ({} live)",
        lobby_id,
        state.lobbies[&session_id].name,
        state.lobbies[&session_id].version,
        ip,
        state.lobbies.len()
    ));

    let mut resp = serde_json::Map::new();
    resp.insert("sessionId".into(), json!(session_id));
    resp.insert("lobbyId".into(), json!(lobby_id));
    resp.insert("hostIdentity".into(), json!(host_identity));
    resp.insert("token".into(), json!(token));
    resp.insert("conn".into(), json!(conn));
    // A DIRECT host never touches signaling/ICE -> no creds. P2P hosts get the block.
    // TURN cred is bound to the client's IP bucket (audit M2), not the host identity.
    if !is_direct {
        resp.extend(ice_block(&ip_bucket(ip)));
    }
    (200, Value::Object(resp))
}

fn h_heartbeat(state: &mut MasterState, ip: &str, body: &Value) -> (u16, Value) {
    if !rate_ok(state, ip, "mutate", RL_MUTATE.0, RL_MUTATE.1) {
        return (429, json!({"error": "rate"}));
    }
    // resolve identity first (immutable borrow), then mutate.
    let sid = match auth_by_session(state, body) {
        Some(lo) => lo.session_id.clone(),
        None => return (403, json!({"error": "unknown session or bad token"})),
    };
    {
        let lo = state.lobbies.get_mut(&sid).expect("just authed");
        let pc = as_int(body, "players_cur", lo.players_cur);
        lo.players_cur = pc.clamp(0, lo.players_max);
        if body_has(body, "listed") {
            lo.listed = as_bool(body, "listed", lo.listed);
        }
        lo.last_seen = Instant::now();
    }
    // TURN cred bound to the client IP bucket (audit M2), refreshed each heartbeat.
    let turn = turn_creds(&CFG.turn_uri, &CFG.turn_secret, &ip_bucket(ip), TURN_TTL)
        .unwrap_or_else(|| json!({}));
    (200, json!({"ok": true, "turn": turn}))
}

fn h_leave(state: &mut MasterState, ip: &str, body: &Value) -> (u16, Value) {
    if !rate_ok(state, ip, "mutate", RL_MUTATE.0, RL_MUTATE.1) {
        return (429, json!({"error": "rate"}));
    }
    let (sid, pub_id) = match auth_by_session(state, body) {
        Some(lo) => (lo.session_id.clone(), lo.lobby_id.clone()),
        None => return (403, json!({"error": "unknown session or bad token"})),
    };
    drop_lobby(state, &sid, &pub_id);
    log(&format!("leave {} ({} live)", pub_id, state.lobbies.len()));
    (200, json!({"ok": true}))
}

fn h_visibility(state: &mut MasterState, ip: &str, body: &Value) -> (u16, Value) {
    if !rate_ok(state, ip, "mutate", RL_MUTATE.0, RL_MUTATE.1) {
        return (429, json!({"error": "rate"}));
    }
    let pub_id = match as_str(body, "lobbyId") {
        Some(s) => s.to_string(),
        None => return (400, json!({"error": "lobbyId + token required"})),
    };
    let tok = match as_str(body, "token") {
        Some(s) => s.to_string(),
        None => return (400, json!({"error": "lobbyId + token required"})),
    };
    let sid = match state.lobby_by_public.get(&pub_id) {
        Some(s) => s.clone(),
        None => return (403, json!({"error": "unknown lobby or bad token"})),
    };
    let listed = as_bool(body, "listed", true);
    let (ok, lobby_id) = match state.lobbies.get_mut(&sid) {
        Some(lo) if ct_eq(tok.as_bytes(), lo.token.as_bytes()) => {
            lo.listed = listed;
            (true, lo.lobby_id.clone())
        }
        _ => (false, String::new()),
    };
    if !ok {
        return (403, json!({"error": "unknown lobby or bad token"}));
    }
    log(&format!("visibility {lobby_id} listed={listed}"));
    (200, json!({"ok": true}))
}

fn h_join(state: &mut MasterState, ip: &str, body: &Value) -> (u16, Value) {
    if !rate_ok(state, ip, "join", RL_JOIN.0, RL_JOIN.1) {
        return (429, json!({"error": "rate"}));
    }
    let pub_id = match as_str(body, "lobbyId") {
        Some(s) => s.to_string(),
        None => return (400, json!({"error": "lobbyId required"})),
    };
    let sid = match state.lobby_by_public.get(&pub_id) {
        Some(s) => s.clone(),
        None => return (404, json!({"error": "lobby not found"})),
    };
    let lo = match state.lobbies.get(&sid) {
        Some(lo) => lo,
        None => return (404, json!({"error": "lobby not found"})),
    };
    // lo.locked is a browser UI hint only; the real admission gate is the game-layer
    // post-Connected join-secret challenge (a secret the master never sees).
    if lo.conn == "direct" && lo.direct_port != 0 {
        log(&format!(
            "join {} DIRECT -> {}:{} from {}",
            lo.lobby_id, lo.ip, lo.direct_port, ip
        ));
        return (200, json!({"conn": "direct", "addr": format!("{}:{}", lo.ip, lo.direct_port)}));
    }
    let session_id = lo.session_id.clone();
    let host_identity = lo.host_identity.clone();
    let lobby_id = lo.lobby_id.clone();
    let peer_identity = format!("c{}", token_hex(8));
    log(&format!("join {lobby_id} as {peer_identity} from {ip}"));
    let mut resp = serde_json::Map::new();
    resp.insert("sessionId".into(), json!(session_id));
    resp.insert("peerIdentity".into(), json!(peer_identity));
    resp.insert("hostIdentity".into(), json!(host_identity));
    resp.insert("conn".into(), json!("p2p"));
    // TURN cred bound to the joiner's IP bucket (audit M2), not the fresh peer id.
    resp.extend(ice_block(&ip_bucket(ip)));
    (200, Value::Object(resp))
}

fn build_rows(state: &MasterState) -> Vec<Value> {
    let now = Instant::now();
    let mut rows = Vec::new();
    for lo in state.lobbies.values() {
        if !lo.listed {
            continue;
        }
        rows.push(json!({
            "lobbyId": lo.lobby_id,
            "name": lo.name,
            "version": lo.version,
            "proto": lo.proto,
            "world": lo.world,
            "locked": lo.locked,
            "players_cur": lo.players_cur,
            "players_max": lo.players_max,
            "age": now.duration_since(lo.last_seen).as_secs() as i64,
            "conn": lo.conn,
        }));
    }
    rows
}

/// Refresh the /v1/lobbies cache if stale and return cheap Arc handles to the cached
/// unfiltered body + the row set. Called UNDER the lock; the caller serializes any
/// version-filtered response from the returned rows AFTER dropping the lock (audit
/// L5 — the ~200 KB clone/serialize no longer runs under the global mutex).
fn lobbies_snapshot(state: &mut MasterState) -> (Arc<Vec<u8>>, Arc<Vec<Value>>) {
    let now = Instant::now();
    let stale = match state.cache_t {
        Some(t) => now.duration_since(t) >= LOBBIES_CACHE_TTL,
        None => true,
    };
    if stale {
        let rows = build_rows(state);
        let body = serde_json::to_vec(&json!({"lobbies": rows})).unwrap_or_default();
        state.cache_all_body = Arc::new(body);
        state.cache_rows = Arc::new(rows);
        state.cache_t = Some(now);
    }
    (Arc::clone(&state.cache_all_body), Arc::clone(&state.cache_rows))
}

/// Serialize a version-filtered lobby list from a cached row snapshot (runs OFF the
/// lock). Bounded by MAX_LOBBIES rows.
fn filter_lobbies(rows: &[Value], version_filter: &str) -> Vec<u8> {
    let filtered: Vec<&Value> = rows
        .iter()
        .filter(|r| r.get("version").and_then(|v| v.as_str()) == Some(version_filter))
        .collect();
    serde_json::to_vec(&json!({"lobbies": filtered})).unwrap_or_default()
}

// ---- HTTP plumbing ----------------------------------------------------------

fn dispatch_post(path: &str, ip: &str, body: &Value) -> Option<(u16, Value)> {
    let mut state = lock_state();
    let r = match path {
        "/v1/host" => h_host(&mut state, ip, body),
        "/v1/heartbeat" => h_heartbeat(&mut state, ip, body),
        "/v1/leave" => h_leave(&mut state, ip, body),
        "/v1/visibility" => h_visibility(&mut state, ip, body),
        "/v1/join" => h_join(&mut state, ip, body),
        _ => return None,
    };
    Some(r)
}

fn reason_phrase(status: u16) -> &'static str {
    match status {
        200 => "OK",
        400 => "Bad Request",
        403 => "Forbidden",
        404 => "Not Found",
        405 => "Method Not Allowed",
        413 => "Payload Too Large",
        429 => "Too Many Requests",
        500 => "Internal Server Error",
        _ => "OK",
    }
}

async fn write_response(stream: &mut TcpStream, status: u16, body: &[u8]) {
    let head = format!(
        "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
        status,
        reason_phrase(status),
        body.len()
    );
    // Bound the write (audit L6): a client that stops reading a large /v1/lobbies body
    // must not pin its CONNS admission slot until the OS TCP timeout. Drop on expiry.
    let _ = timeout(HTTP_TIMEOUT, async {
        stream.write_all(head.as_bytes()).await?;
        stream.write_all(body).await?;
        stream.flush().await
    })
    .await;
}

fn json_bytes(v: &Value) -> Vec<u8> {
    serde_json::to_vec(v).unwrap_or_else(|_| b"{}".to_vec())
}

fn find_subsequence(hay: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || hay.len() < needle.len() {
        return None;
    }
    hay.windows(needle.len()).position(|w| w == needle)
}

/// Read the header block up to (and consuming) the terminating CRLFCRLF, bounded by
/// `max`. Returns (head_bytes, leftover_body_bytes_already_read). Mirrors the Python
/// `readuntil(b"\r\n\r\n")` with the start_server `limit`.
async fn read_head(stream: &mut TcpStream, max: usize) -> Result<(Vec<u8>, Vec<u8>), HeadErr> {
    let mut buf: Vec<u8> = Vec::with_capacity(2048);
    let mut tmp = [0u8; 4096];
    loop {
        if let Some(i) = find_subsequence(&buf, b"\r\n\r\n") {
            let leftover = buf[i + 4..].to_vec();
            buf.truncate(i);
            return Ok((buf, leftover));
        }
        if buf.len() > max {
            return Err(HeadErr::TooLarge);
        }
        let n = stream.read(&mut tmp).await.map_err(|_| HeadErr::Closed)?;
        if n == 0 {
            return Err(HeadErr::Closed);
        }
        buf.extend_from_slice(&tmp[..n]);
    }
}

enum HeadErr {
    TooLarge,
    Closed,
}

struct ConnGuard;
impl Drop for ConnGuard {
    fn drop(&mut self) {
        CONNS.fetch_sub(1, Ordering::Relaxed);
    }
}

async fn handle(mut stream: TcpStream, peer_ip: String) {
    // Concurrent-connection admission cap: shed before any read.
    if CONNS.fetch_add(1, Ordering::Relaxed) >= MAX_CONNS {
        CONNS.fetch_sub(1, Ordering::Relaxed);
        log(&format!("[{peer_ip}] refused: connection cap ({MAX_CONNS})"));
        return;
    }
    let _guard = ConnGuard;

    // ---- read + parse header block (bounded + timed) ----
    let (head, leftover) = match timeout(HTTP_TIMEOUT, read_head(&mut stream, MAX_HEADER)).await {
        Ok(Ok(v)) => v,
        Ok(Err(HeadErr::TooLarge)) => {
            write_response(&mut stream, 413, &json_bytes(&json!({"error": "headers too large"}))).await;
            return;
        }
        Ok(Err(HeadErr::Closed)) | Err(_) => return, // eof / timeout
    };

    let head_str = String::from_utf8_lossy(&head);
    let mut lines = head_str.split("\r\n");
    let request_line = lines.next().unwrap_or("");
    let mut rl = request_line.splitn(3, ' ');
    let (method, raw_path) = match (rl.next(), rl.next(), rl.next()) {
        (Some(m), Some(p), Some(_)) => (m, p),
        _ => {
            write_response(&mut stream, 400, &json_bytes(&json!({"error": "bad request line"}))).await;
            return;
        }
    };

    let mut headers: HashMap<String, String> = HashMap::new();
    for ln in lines {
        if ln.is_empty() {
            continue;
        }
        if let Some((k, v)) = ln.split_once(':') {
            headers.insert(k.trim().to_lowercase(), v.trim().to_string());
        }
    }

    let (path, query) = match raw_path.split_once('?') {
        Some((p, q)) => (p, q),
        None => (raw_path, ""),
    };
    let client_ip = resolve_client_ip(&peer_ip, &headers);

    // ---- body (POST only, bounded) ----
    let mut body_obj = Value::Object(serde_json::Map::new());
    if method == "POST" {
        let clen: i64 = headers
            .get("content-length")
            .and_then(|s| s.trim().parse::<i64>().ok())
            .unwrap_or(-1);
        if clen < 0 || clen as usize > MAX_BODY {
            write_response(&mut stream, 413, &json_bytes(&json!({"error": "body too large"}))).await;
            return;
        }
        let clen = clen as usize;
        let mut raw = leftover;
        if raw.len() < clen {
            let need = clen - raw.len();
            let mut rest = vec![0u8; need];
            match timeout(HTTP_TIMEOUT, stream.read_exact(&mut rest)).await {
                Ok(Ok(_)) => raw.extend_from_slice(&rest),
                _ => return, // eof / timeout
            }
        }
        raw.truncate(clen);
        if !raw.is_empty() {
            match serde_json::from_slice::<Value>(&raw) {
                Ok(v) if v.is_object() => body_obj = v,
                _ => {
                    write_response(&mut stream, 400, &json_bytes(&json!({"error": "bad json"}))).await;
                    return;
                }
            }
        }
    }

    // ---- route ----
    if method == "GET" && path == "/v1/lobbies" {
        let mut vf = String::new();
        for kv in query.split('&') {
            if let Some(rest) = kv.strip_prefix("version=") {
                vf = clamp_str(rest, MAX_VERSION);
            }
        }
        // Refresh + snapshot under the lock; serialize the response OFF the lock (L5).
        let (all_body, rows) = {
            let mut state = lock_state();
            lobbies_snapshot(&mut state)
        };
        if vf.is_empty() {
            write_response(&mut stream, 200, &all_body).await;
        } else {
            let body = filter_lobbies(&rows, &vf);
            write_response(&mut stream, 200, &body).await;
        }
    } else if method == "GET" && path == "/v1/latest" {
        write_response(
            &mut stream,
            200,
            &json_bytes(&json!({"proto": LATEST_PROTO, "mod": LATEST_MOD, "url": LATEST_URL})),
        )
        .await;
    } else if method == "GET" && path == "/healthz" {
        let n = lock_state().lobbies.len();
        write_response(&mut stream, 200, &json_bytes(&json!({"ok": true, "lobbies": n}))).await;
    } else if method == "POST" {
        match dispatch_post(path, &client_ip, &body_obj) {
            Some((status, resp)) => write_response(&mut stream, status, &json_bytes(&resp)).await,
            None => write_response(&mut stream, 404, &json_bytes(&json!({"error": "not found"}))).await,
        }
    } else {
        write_response(&mut stream, 404, &json_bytes(&json!({"error": "not found"}))).await;
    }
}

async fn sweeper() {
    let max_window = [RL_CREATE.0, RL_JOIN.0, RL_MUTATE.0].into_iter().max().unwrap();
    loop {
        tokio::time::sleep(Duration::from_secs(30)).await;
        let now = Instant::now();
        let mut state = lock_state();
        // expire stale lobbies
        let dead: Vec<(String, String, u64)> = state
            .lobbies
            .values()
            .filter(|lo| now.duration_since(lo.last_seen) > LOBBY_TTL)
            .map(|lo| (lo.session_id.clone(), lo.lobby_id.clone(), now.duration_since(lo.last_seen).as_secs()))
            .collect();
        for (sid, pub_id, stale) in dead {
            drop_lobby(&mut state, &sid, &pub_id);
            log(&format!("expired {pub_id} (stale {stale}s)"));
        }
        // prune fully-stale rate buckets (else `rate` leaks one entry per distinct IP)
        state
            .rate
            .retain(|_, v| v.last().map(|last| now.duration_since(*last) < max_window).unwrap_or(false));
    }
}

#[tokio::main]
async fn main() {
    // fail fast on missing secrets (same as the Python FATAL exits)
    if CFG.turn_secret.is_empty() {
        log("FATAL: COOP_TURN_SECRET not set -- refusing to mint TURN creds");
        std::process::exit(1);
    }
    if CFG.signaling_token.is_empty() {
        log("FATAL: COOP_SIGNALING_TOKEN not set -- clients could not reach signaling");
        std::process::exit(1);
    }

    let addr = format!("0.0.0.0:{}", CFG.port);
    let listener = match TcpListener::bind(&addr).await {
        Ok(l) => l,
        Err(e) => {
            log(&format!("FATAL: bind {addr} failed: {e}"));
            std::process::exit(1);
        }
    };
    log(&format!(
        "master listening on {addr} (signaling={} stun={} turn={})",
        if CFG.signaling_url.is_empty() { "?" } else { &CFG.signaling_url },
        if CFG.stun_uri.is_empty() { "?" } else { &CFG.stun_uri },
        if CFG.turn_uri.is_empty() { "off" } else { "on" }
    ));

    tokio::spawn(sweeper());

    loop {
        match listener.accept().await {
            Ok((stream, addr)) => {
                let peer_ip = addr.ip().to_string();
                tokio::spawn(handle(stream, peer_ip));
            }
            Err(e) => {
                log(&format!("accept error: {e}"));
            }
        }
    }
}
