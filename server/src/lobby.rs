//! The lobby domain: what a lobby is, the state every request mutates, and the handlers behind
//! `/v1/host`, `/v1/heartbeat`, `/v1/leave`, `/v1/visibility` and `/v1/join`, plus the browse
//! snapshot and the sweeper that reaps what stopped heartbeating.

use crate::common::{
    clamp_str, ct_eq, identity_shape_ok, ip_bucket, log, token_hex, token_urlsafe, turn_creds,
};
use crate::master_config::CFG;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::sync::Arc;
use std::sync::{LazyLock, Mutex, MutexGuard};
use std::time::{Duration, Instant};

pub const TURN_TTL: u64 = 120;
// 90s = 3 missed 30s heartbeats before a lobby is reaped. Was 300s (ghost-lobby bug:
// a TASK-KILLED host sends no /v1/leave, so its dead entry lingered up to the TTL; a
// dead host was seen at 237s/297s age). Lowered 2026-07-16 per the user's go.
pub const LOBBY_TTL: Duration = Duration::from_secs(90);
pub const LOBBIES_CACHE_TTL: Duration = Duration::from_secs(5);

pub const MAX_LOBBIES_PER_IP: usize = 8;
pub const MAX_LOBBIES_GLOBAL: usize = 1000;
// Hard cap on distinct rate-limit buckets, so a source-diverse flood can't grow the
// `rate` map toward MemoryMax between the 30s sweeps (security audit 2026-07-16, M3).
// With /64 IPv6 coarsening a flood needs distinct routable prefixes to add keys, so
// this is a belt-and-suspenders ceiling; when hit, new buckets are refused (the
// flooding prefixes are simply treated as rate-limited).
pub const MAX_RATE_KEYS: usize = 50_000;

// per-(IP, class) sliding-window rate limits: (window, max events).
pub const RL_CREATE: (Duration, usize) = (Duration::from_secs(60), 10);
pub const RL_JOIN: (Duration, usize) = (Duration::from_secs(60), 20);
pub const RL_MUTATE: (Duration, usize) = (Duration::from_secs(60), 240);

pub const MAX_NAME: usize = 63;
pub const MAX_WORLD: usize = 39;
pub const MAX_VERSION: usize = 23;
pub const MAX_GAME: usize = 23; // v122: the host's VOTV game target ("0.9.0n")

// latest released mod, served by /v1/latest. DEPLOY CONFIG, not code: overridable via
// COOP_LATEST_PROTO / COOP_LATEST_MOD / COOP_LATEST_URL in /etc/coop-master.env, so a
// release bump is an env edit + `systemctl restart coop-master` -- no rebuild.
// v122 (2026-07-19): the compiled-in default is proto 0 = "no released record" --
// the client treats proto<=0 as NO VERDICT (silent identity label; it can never
// fabricate a stale "latest" or an update nag). ZERO releases exist yet; the first
// real release sets COOP_LATEST_PROTO (its build number) + COOP_LATEST_MOD (the
// Paper-pair display tag, e.g. "0.9.0n-b130") per the release checklist.
pub const LATEST_PROTO: i64 = 0;
pub const LATEST_MOD: &str = "";
pub const LATEST_URL: &str = "https://github.com/VOTV-MP/Multivoid/releases";

pub const TRUSTED_PROXY_PEERS: [&str; 2] = ["127.0.0.1", "::1"];

pub static STATE: LazyLock<Mutex<MasterState>> = LazyLock::new(|| Mutex::new(MasterState::new()));

/// Lock STATE, tolerating poisoning (audit M4): under panic=unwind a handler that
/// panics while holding the lock would poison it; recovering the inner guard keeps
/// one bad request from bricking every subsequent handler. The state is structurally
/// usable post-panic (handlers only mutate maps, no half-torn invariants).
pub fn lock_state() -> MutexGuard<'static, MasterState> {
    STATE.lock().unwrap_or_else(|e| e.into_inner())
}

// ---- state ------------------------------------------------------------------

pub struct Lobby {
    pub session_id: String,
    pub lobby_id: String,
    pub token: String,
    pub host_identity: String,
    pub name: String,
    pub version: String,
    pub game: String, // v122: host's VOTV game target ("" = pre-field host)
    pub proto: i64,
    pub world: String,
    pub locked: bool,
    pub players_cur: i64,
    pub players_max: i64,
    pub listed: bool,
    pub last_seen: Instant,
    pub ip: String,
    pub conn: String, // "p2p" | "direct"
    pub direct_port: i64,
}

impl Lobby {
    fn new(ip: &str) -> Lobby {
        Lobby {
            session_id: token_hex(16),
            lobby_id: token_hex(8),
            token: token_urlsafe(24),
            // Empty until `/v1/host` supplies the host's own public key, which it
            // must (see h_host). The master no longer mints a routing name: a name
            // it asserts on a host's behalf is one nobody can prove, and since
            // 2026-08-29 the relay registers only proved keys.
            host_identity: String::new(),
            name: String::new(),
            version: String::new(),
            game: String::new(),
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

pub struct MasterState {
    pub lobbies: HashMap<String, Lobby>,      // sessionId -> Lobby
    pub lobby_by_public: HashMap<String, String>, // lobbyId -> sessionId
    pub rate: HashMap<String, Vec<Instant>>,  // "ipbucket|class" -> recent timestamps
    pub cache_t: Option<Instant>,
    // Arc so /v1/lobbies can clone the cached body/rows cheaply under the lock and
    // serialize the response OUTSIDE it (audit L5: serializing a ~200 KB body under
    // the global lock serialized it against all host/join/heartbeat processing).
    pub cache_rows: Arc<Vec<Value>>,
    pub cache_all_body: Arc<Vec<u8>>,
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

pub fn coerce_str(v: Option<&Value>) -> String {
    match v {
        Some(Value::String(s)) => s.clone(),
        None | Some(Value::Null) => String::new(),
        Some(Value::Bool(b)) => if *b { "True".into() } else { "False".into() },
        Some(Value::Number(n)) => n.to_string(),
        Some(other) => other.to_string(),
    }
}

/// clamp a body string field: coerce -> strip control chars -> take `maxlen` chars.
pub fn clamp_field(body: &Value, key: &str, maxlen: usize) -> String {
    clamp_str(&coerce_str(body.get(key)), maxlen)
}

/// Python `int(body.get(key, default))` with its try/except -> default fallback.
pub fn as_int(body: &Value, key: &str, default: i64) -> i64 {
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
pub fn as_bool(body: &Value, key: &str, default: bool) -> bool {
    match body.get(key) {
        None | Some(Value::Null) => default,
        Some(Value::Bool(b)) => *b,
        Some(Value::Number(n)) => n.as_f64().map(|f| f != 0.0).unwrap_or(default),
        Some(Value::String(s)) => !s.is_empty(),
        Some(Value::Array(a)) => !a.is_empty(),
        Some(Value::Object(o)) => !o.is_empty(),
    }
}

pub fn body_has(body: &Value, key: &str) -> bool {
    body.get(key).is_some()
}

pub fn as_str<'a>(body: &'a Value, key: &str) -> Option<&'a str> {
    body.get(key).and_then(|v| v.as_str())
}

// ---- helpers ----------------------------------------------------------------

pub fn resolve_client_ip(peer_ip: &str, headers: &HashMap<String, String>) -> String {
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

pub fn rate_ok(state: &mut MasterState, ip: &str, cls: &str, window: Duration, limit: usize) -> bool {
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
pub fn ice_block(turn_label: &str) -> serde_json::Map<String, Value> {
    let mut m = serde_json::Map::new();
    m.insert("signalingUrl".into(), json!(CFG.signaling_url));
    m.insert("signalingToken".into(), json!(CFG.signaling_token));
    m.insert("stun".into(), json!(CFG.stun_uri));
    let turn = turn_creds(&CFG.turn_uri, &CFG.turn_secret, turn_label, TURN_TTL).unwrap_or_else(|| json!({}));
    m.insert("turn".into(), turn);
    m
}

pub fn drop_lobby(state: &mut MasterState, session_id: &str, lobby_id: &str) {
    state.lobbies.remove(session_id);
    state.lobby_by_public.remove(lobby_id);
}

pub fn evict_if_full(state: &mut MasterState) {
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

pub fn lobbies_per_ip(state: &MasterState, ip: &str) -> usize {
    // Count by the coarse bucket (audit M3), not the exact address, so an IPv6 /64
    // can't mint MAX_LOBBIES_PER_IP lobbies per /128.
    let bucket = ip_bucket(ip);
    state.lobbies.values().filter(|lo| ip_bucket(&lo.ip) == bucket).count()
}

// ---- version-pair gate (2026-08-29) -----------------------------------------
//
// The register endpoint used to swallow ANY self-reported version pair -- a
// field host was seen advertising "b148" when no such build exists, and the
// lobby list dutifully rendered it (every real client sees an unjoinable
// impossible-version row: pure pollution, and it reads as "a newer build
// exists"). The pair is self-reported and host attestation is a separate arc
// (docs/security/README.md -- peer certificates), so this gate is now deliberately
// minimal: it refuses SHAPE-invalid game strings and NOTHING ELSE.
//
// THE BUILD-NUMBER CEILING IS RETIRED (2026-08-31). `COOP_MAX_BUILD` and
// `COOP_ALLOWED_BUILDS` are no longer read; see version_gate for why. Any
// deployment still carrying them in /etc/coop-master.env can leave them there --
// they are simply ignored, so retiring this needs no coordinated redeploy, which
// is itself the point.
// proto==0 (legacy pre-v122 hosts) always passed and still does.

/// "0.9.0n" shape: three '.'-separated numeric parts (1-2 digits), the last
/// with an optional 1-2 letter suffix. Hand-rolled -- no regex dependency.
pub fn game_shape_ok(game: &str) -> bool {
    let parts: Vec<&str> = game.split('.').collect();
    if parts.len() != 3 {
        return false;
    }
    let digits = |s: &str| !s.is_empty() && s.len() <= 2 && s.bytes().all(|b| b.is_ascii_digit());
    if !digits(parts[0]) || !digits(parts[1]) {
        return false;
    }
    let last = parts[2];
    let dig_end = last.bytes().take_while(|b| b.is_ascii_digit()).count();
    let suffix = &last[dig_end..];
    dig_end >= 1 && dig_end <= 2 && suffix.len() <= 2
        && suffix.bytes().all(|b| b.is_ascii_lowercase())
}

/// Pure gate half (env resolved by the caller so this is unit-testable).
///
/// SHAPE ONLY. THE MASTER DOES NOT ADJUDICATE WHICH BUILDS EXIST -- the proto ceiling
/// (`COOP_MAX_BUILD`) and the exact-set allowlist (`COOP_ALLOWED_BUILDS`) are RETIRED. A
/// ceiling that refuses hosting to a build the master has not been told about stops testers
/// on fresh builds from playing at all, which costs more than the listing it was cleaning up.
///
/// They were added for A58 -- a field host advertised a build that did not exist and the
/// browser listed it, which reads as "a newer version is out". The trade turned out bad in
/// both directions:
///
///   * COST: `kProtocolVersion` moves on EVERY wire change by standing rule, so anyone
///     running a build newer than the deployed env value was refused hosting outright
///     ("build bN does not exist"). That is us denying our own testers, and it made every
///     proto bump require a coordinated master redeploy BEFORE anybody could host.
///   * BENEFIT: A58's own recorded residual already says number-based gating "cannot
///     attribute, only bound" -- a spoofer simply claims a REAL number instead.
///
/// And the pollution it defended against is now handled honestly by the client, which it
/// was not when this was written: an unknown build renders with the red mismatch mark, the
/// details panel names which side must update, and `JoinLobby` refuses on byte-equality
/// with a popup before any connection is made. A row that cannot be joined says so.
///
/// What STAYS is `game_shape_ok`, because that is a PARSING concern (keep arbitrary
/// strings out of a field every browser renders), not a version-policy one.
pub fn version_gate(game: &str, proto: i64) -> Result<(), String> {
    if !game.is_empty() && !game_shape_ok(game) {
        return Err(format!("bad game version '{}'", game));
    }
    let _ = proto;
    Ok(())
}

pub fn version_gate_env(game: &str, proto: i64) -> Result<(), String> {
    // No env left to resolve: the ceiling and the allowlist are retired. Kept as a named
    // seam so the call site still reads as a gate and a future policy has an obvious home.
    version_gate(game, proto)
}

pub fn auth_by_session<'a>(state: &'a MasterState, body: &Value) -> Option<&'a Lobby> {
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

pub fn h_host(state: &mut MasterState, ip: &str, body: &Value) -> (u16, Value) {
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
    // v122 Paper-pair identity: game + proto. `version` is the LEGACY pre-v122
    // display tag (old hosts sent their game version here) -- pass-through, no
    // fabricated default.
    lo.version = clamp_field(body, "version", MAX_VERSION);
    lo.game = clamp_field(body, "game", MAX_GAME);
    lo.proto = as_int(body, "proto", 0).clamp(0, 65535);
    // Version-pair gate (2026-08-29): shape-invalid game / impossible build
    // numbers are refused BEFORE the lobby exists -- see the gate block above.
    if let Err(reason) = version_gate_env(&lo.game, lo.proto) {
        log(&format!("host REFUSED from {}: {} (name='{}')", ip, reason, lo.name));
        return (400, json!({"error": reason}));
    }
    // The identity joiners dial: the host's own durable public key, `gen:<64 hex>`.
    // REQUIRED since 2026-08-29. It used to be optional, with a minted `h<16hex>`
    // fallback for an already-released host that sent none -- that fallback is
    // retired with the b<=133 cohort (user decision), and its retirement is not
    // cosmetic: a minted name is one the master ASSERTS on a host's behalf, and
    // the signaling relay now registers nothing that its holder did not sign for.
    // Advertising a name no peer can be registered under would list a lobby that
    // is unjoinable, which is the exact failure this endpoint exists to prevent.
    //
    // A pre-b145 host therefore gets a 400 here and never reaches the relay, so
    // its deprecation is a NAMED refusal at the first call rather than a silent
    // rendezvous failure two hops later.
    match as_str(body, "identity") {
        Some(id) if identity_shape_ok(id) => lo.host_identity = id.to_string(),
        _ => {
            log(&format!("host REFUSED from {}: missing/bad identity", ip));
            return (400, json!({"error": "this build is too old to host -- update Multivoid"}));
        }
    }
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
        "host {} '{}' game={} b{} from {} ({} live)",
        lobby_id,
        state.lobbies[&session_id].name,
        state.lobbies[&session_id].game,
        state.lobbies[&session_id].proto,
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

pub fn h_heartbeat(state: &mut MasterState, ip: &str, body: &Value) -> (u16, Value) {
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
    // SECURITY: this endpoint used to re-mint a TURN
    // credential on EVERY heartbeat -- an HMAC signature handed out at RL_MUTATE rate to
    // anyone holding a token for their own lobby. The minting was retired whole (RULE 2)
    // rather than rate-limited, because it had NO CONSUMER: the only caller is
    // LobbyAnnouncer's heartbeat thread, and it reads ONLY resp.ok and resp.status --
    // it never parses the heartbeat's response body at all, in contrast with the
    // /v1/host announce, which does ParseObject on it. Credentials are issued where
    // they are actually consumed, at /v1/host and /v1/join.
    (200, json!({"ok": true}))
}

pub fn h_leave(state: &mut MasterState, ip: &str, body: &Value) -> (u16, Value) {
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

pub fn h_visibility(state: &mut MasterState, ip: &str, body: &Value) -> (u16, Value) {
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

pub fn h_join(state: &mut MasterState, ip: &str, body: &Value) -> (u16, Value) {
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
    // lo.locked is a browser UI hint, and THE GATE BEHIND IT NOW EXISTS (2026-08-31, game
    // proto 149): a joiner proves knowledge of the lobby password inside the peer-admission
    // exchange and a wrong or missing proof is refused before a seat is spent. The master
    // does not participate -- it echoes a bool a host asserted and verifies nothing, which
    // is correct: the gate belongs at the host's own admission, not at the listing.
    //
    // THIS COMMENT HAS NOW BEEN WRONG IN BOTH DIRECTIONS. It first claimed a "game-layer
    // post-Connected join-secret challenge" that existed nowhere; the correction ("NO
    // admission gate behind it: a locked lobby is enterable by anyone") was true for six
    // weeks and went stale the day the gate shipped, and it was STILL LIVE ON THE DEPLOYED
    // MASTER when an audit read it. A status sentence is a claim about code that moves --
    // in a file that only changes when someone remembers it exists.
    if lo.conn == "direct" && lo.direct_port != 0 {
        log(&format!(
            "join {} DIRECT -> {}:{} from {}",
            lo.lobby_id, lo.ip, lo.direct_port, ip
        ));
        // `hostIdentity` RIDES THE DIRECT RESPONSE TOO (2026-08-31). It used to be P2P-only,
        // because on that lane it is the ROUTING name -- what signaling rendezvouses on --
        // and a direct joiner dials an address instead. Since the peer-identity work it is
        // ALSO the value a joiner binds the answering key to, and a password proof refuses
        // to be emitted without that binding. So a LOCKED + DIRECT lobby was unjoinable
        // from the browser: the client had nothing to bind to and closed itself.
        //
        // The client already caps and parses this field on the P2P branch; adding it here
        // costs one clone of a value this function already holds.
        return (200, json!({
            "conn": "direct",
            "addr": format!("{}:{}", lo.ip, lo.direct_port),
            "hostIdentity": lo.host_identity.clone()
        }));
    }
    let session_id = lo.session_id.clone();
    let host_identity = lo.host_identity.clone();
    let lobby_id = lo.lobby_id.clone();
    // No `peerIdentity`. The master used to mint a `c<16hex>` here for the joiner
    // to install with ResetIdentity; a joiner has registered under its OWN durable
    // key since b144 and stopped reading this field then, and the relay now
    // registers only keys their holder signed for -- so a minted name is one no
    // peer could use even if it wanted to. Retired with the b<=133 cohort.
    log(&format!("join {lobby_id} from {ip}"));
    let mut resp = serde_json::Map::new();
    resp.insert("sessionId".into(), json!(session_id));
    resp.insert("hostIdentity".into(), json!(host_identity));
    resp.insert("conn".into(), json!("p2p"));
    // TURN cred bound to the joiner's IP bucket (audit M2), not the fresh peer id.
    resp.extend(ice_block(&ip_bucket(ip)));
    (200, Value::Object(resp))
}

pub fn build_rows(state: &MasterState) -> Vec<Value> {
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
            "game": lo.game,    // v122: the host's VOTV game target (gate tier 1)
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
pub fn lobbies_snapshot(state: &mut MasterState) -> (Arc<Vec<u8>>, Arc<Vec<Value>>) {
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
pub fn filter_lobbies(rows: &[Value], version_filter: &str) -> Vec<u8> {
    let filtered: Vec<&Value> = rows
        .iter()
        .filter(|r| r.get("version").and_then(|v| v.as_str()) == Some(version_filter))
        .collect();
    serde_json::to_vec(&json!({"lobbies": filtered})).unwrap_or_default()
}

pub fn dispatch_post(path: &str, ip: &str, body: &Value) -> Option<(u16, Value)> {
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

pub async fn sweeper() {
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

#[cfg(test)]
mod tests {
    use super::{game_shape_ok, version_gate};
    use crate::common::identity_shape_ok;

    #[test]
    fn game_shape_accepts_real_votv_versions() {
        for g in ["0.9.0n", "0.8.1c", "0.7.0", "0.9.0", "1.0.0", "0.9.1pt"] {
            assert!(game_shape_ok(g), "{g} should pass");
        }
    }

    #[test]
    fn game_shape_refuses_garbage() {
        for g in ["", "lol", "0.9", "0.9.0.1", "0.9.0N", "0.9.0nnn", "999.9.0n",
                  "0.9.0n b148", "<script>", "0..0", "0.9.вot"] {
            assert!(!game_shape_ok(g), "{g} should fail");
        }
    }

    #[test]
    fn any_build_number_may_host() {
        // THE ASSERTION IS INVERTED ON PURPOSE, and it is the point of the change: a
        // tester running a build the master has never heard of MUST be able to host.
        // b148 against a "newest released b143" was the case the old ceiling refused,
        // and refusing it denied our own testers on every proto bump.
        assert!(version_gate("0.9.0n", 148).is_ok());
        assert!(version_gate("0.9.0n", 9999).is_ok());
        assert!(version_gate("0.9.0n", 143).is_ok());
        assert!(version_gate("0.9.0n", 133).is_ok());
    }

    #[test]
    fn legacy_and_unadvertised_pass() {
        assert!(version_gate("", 0).is_ok());        // legacy pre-v122 host
        assert!(version_gate("0.9.0n", 0).is_ok());  // no build advertised
    }

    #[test]
    fn gate_still_refuses_bad_shape_with_any_proto() {
        // SHAPE survives: this is a parsing concern (keep arbitrary strings out of a
        // field every browser renders), not a version-policy one.
        assert!(version_gate("h4x0r edition", 133).is_err());
        assert!(version_gate("h4x0r edition", 9999).is_err());
    }

    #[test]
    fn identity_shape_accepts_a_real_rendered_key() {
        let k = format!("gen:{}", "ab01cd23".repeat(8)); // 64 hex chars
        assert_eq!(k.len(), 68);
        assert!(identity_shape_ok(&k));
    }

    #[test]
    fn identity_shape_refuses_everything_that_would_dial_nobody() {
        let hex64 = "ab01cd23".repeat(8);
        // The NEGATIVES are the point: each of these would list a lobby that no
        // joiner can reach, which is indistinguishable from "P2P is broken".
        assert!(!identity_shape_ok(""));
        assert!(!identity_shape_ok("h0123456789abcdef"));       // the legacy mint
        assert!(!identity_shape_ok(&hex64));                    // no prefix
        assert!(!identity_shape_ok(&format!("str:{hex64}")));   // wrong type
        assert!(!identity_shape_ok(&format!("gen:{}", &hex64[..62]))); // short
        assert!(!identity_shape_ok(&format!("gen:{hex64}0")));  // odd length
        assert!(!identity_shape_ok(&format!("gen:{}", hex64.to_uppercase()))); // case
        assert!(!identity_shape_ok(&format!("gen:{} x", &hex64[..62])));       // spaced
    }
}
