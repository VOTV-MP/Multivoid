//! The lobby domain: what a lobby is, the state every request mutates, and the handlers behind
//! `/v1/heartbeat`, `/v1/leave`, `/v1/visibility` and `/v1/join`, plus the browse snapshot and the
//! sweeper that reaps what stopped heartbeating. The create (`/v1/host/challenge`, `/v1/host`) is in `host`.

use crate::common::{build_claim_shape_ok, clamp_str, ct_eq, ip_bucket, log, token_hex, token_urlsafe};
use crate::ice::{heartbeat_turn, ice_block, turn_creds, HostTurn, TURN_TTL_HOST, TURN_TTL_JOIN};
use crate::lobby_proof::Nonces;
use crate::master_config::CFG;
use crate::rate_table::RateTable;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::sync::Arc;
use std::sync::{LazyLock, Mutex, MutexGuard};
use std::time::{Duration, Instant};

pub mod host;
use host::{h_challenge, h_host, HostCtx};

// 90s = 3 missed 30s heartbeats before a lobby is reaped. Was 300s (ghost-lobby bug:
// a TASK-KILLED host sends no /v1/leave, so its dead entry lingered up to the TTL; a
// dead host was seen at 237s/297s age). Lowered 2026-07-16 per the user's go.
pub const LOBBY_TTL: Duration = Duration::from_secs(90);
pub const LOBBIES_CACHE_TTL: Duration = Duration::from_secs(5);

pub const MAX_LOBBIES_PER_IP: usize = 8;
pub const MAX_LOBBIES_GLOBAL: usize = 1000;
// Hard cap on distinct rate-limit keys, so a source-diverse flood can't grow the `rate`
// table toward MemoryMax between the 30s sweeps (security audit 2026-07-16, M3). At the cap
// a NEW key is admitted and the least recently seen one forgotten (`rate_table`): the table
// degrades to forgetting old flooders and never refuses an address it does not hold. MTA bounds
// its mitigation otherwise: once its own table holds 100 flooding entries it stops counting
// queries per address and answers the rest (reference/mtasa-blue/Server/mods/deathmatch/logic/
// ASE.cpp:183); this table keeps counting every key it holds.
pub const MAX_RATE_KEYS: usize = 50_000;

// per-(IP, class) sliding-window rate limits: (window, max events).
pub const RL_CHALLENGE: (Duration, usize) = (Duration::from_secs(60), 10);
pub const RL_CREATE: (Duration, usize) = (Duration::from_secs(60), 10);
pub const RL_JOIN: (Duration, usize) = (Duration::from_secs(60), 20);
pub const RL_MUTATE: (Duration, usize) = (Duration::from_secs(60), 240);
// GET /v1/lobbies: the 5 s snapshot cache bounds the work, this bounds the bytes an address can pull.
// An open browser fetches every 5 s (12 a minute), so 120 leaves room for ten players behind one address.
pub const RL_BROWSE: (Duration, usize) = (Duration::from_secs(60), 120);

pub const MAX_NAME: usize = 63;
pub const MAX_WORLD: usize = 39;
pub const MAX_VERSION: usize = 23;
pub const MAX_GAME: usize = 23; // v122: the host's VOTV game target ("0.9.0n")
pub const MAX_GAME_BUILD: usize = 25; // b222: the build the host runs, "<stamp 8 hex>-<pak hash 16 hex>"

// The latest released mod, served by /v1/latest. DEPLOY CONFIG, not code: COOP_LATEST_GAMES and
// COOP_LATEST_URL in /etc/coop-master.env, so a release is an env edit + `systemctl restart coop-master`
// -- no rebuild. The release ritual writes the whole list from the release ledger (publish.ps1 prints it,
// release_master_latest.sh writes it on every master, verify_latest.ps1 checks it). No list = no record:
// the client treats proto<=0 as NO VERDICT, so it never fabricates a "latest" or an update nag.
pub const LATEST_URL: &str = "https://github.com/VOTV-MP/Multivoid/releases";

/// The game every client older than `games` targets: all releases before b222 are for 0.9.0n. Their
/// legacy `proto`/`mod` are this game's entry, so that cohort is never told to update to a Multivoid made
/// for a game it does not have. A historical fact, frozen.
pub const LEGACY_GAME: &str = "0.9.0n";

/// The latest release per game version, served by /v1/latest as `games` (b222):
/// `COOP_LATEST_GAMES=<game>:<build>:<tag>,...` -- e.g. `0.9.0n:221:0.9.0n b221,0.10.0:230:0.10.0 b230`; the
/// tag keeps the "<game> b<N>" shape and may hold spaces, never `,` or `:`. The mod picks the entry for the
/// game it runs.
#[derive(Debug, Clone, PartialEq)]
pub struct LatestGame {
    pub game: String,
    pub proto: i64,
    pub tag: String,
}

/// Parse `COOP_LATEST_GAMES`. An entry that does not parse, or names a game an earlier entry named, is dropped
/// and named in the second list (the caller logs it once): a typo in the deploy config costs that entry, never the
/// others. Empty -> no entries.
pub fn parse_latest_games(spec: &str) -> (Vec<LatestGame>, Vec<String>) {
    let mut games = Vec::new();
    let mut refused = Vec::new();
    for entry in spec.split(',').map(str::trim).filter(|e| !e.is_empty()) {
        let parts: Vec<&str> = entry.splitn(3, ':').collect();
        let ok = parts.len() == 3
            && crate::version_gate::game_shape_ok(parts[0])
            && parts[1].parse::<i64>().map(|p| p > 0 && p <= 65535).unwrap_or(false)
            && !parts[2].is_empty()
            && parts[2].len() <= MAX_VERSION
            && parts[2].bytes().all(|b| b != b':' && (b == b' ' || b.is_ascii_graphic()));
        if ok && !games.iter().any(|g: &LatestGame| g.game == parts[0]) {
            games.push(LatestGame {
                game: parts[0].to_string(),
                proto: parts[1].parse().unwrap_or(0),
                tag: parts[2].to_string(),
            });
        } else {
            refused.push(entry.to_string());
        }
    }
    (games, refused)
}

/// /v1/latest's body: every game's latest under `games` (absent when none), and the legacy `proto`/`mod`
/// from the LEGACY_GAME entry (0 and "" without one -- no verdict for the old cohort). `url` as given.
pub fn latest_answer(games: &[LatestGame], url: &str) -> Value {
    let legacy = games.iter().find(|g| g.game == LEGACY_GAME);
    let mut body = json!({
        "proto": legacy.map_or(0, |g| g.proto),
        "mod": legacy.map_or("", |g| g.tag.as_str()),
        "url": url,
    });
    if !games.is_empty() {
        body["games"] = Value::Array(
            games.iter().map(|g| json!({"game": g.game, "proto": g.proto, "mod": g.tag})).collect(),
        );
    }
    body
}

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
    pub proved: bool, // the create proved its host holds the key `host_identity` names
    pub name: String,
    pub version: String,
    pub game: String, // v122: host's VOTV game target ("" = pre-field host)
    pub game_build: String, // b222: the game build the host runs ("" = pre-field host)
    pub proto: i64,
    pub world: String,
    pub locked: bool,
    pub players_cur: i64,
    pub players_max: i64,
    pub listed: bool,
    // The host's own claim of its build, recorded when well formed and verified by nobody here:
    // the binary's SHA-256 (64 hex), the signing key's id (0 = unsigned) and the signature (128 hex).
    // Empty / 0 for a host that sent none.
    pub build_sha: String,
    pub build_key: u8,
    pub build_sig: String,
    pub last_seen: Instant,
    pub ip: String,
    pub conn: String, // "p2p" | "direct"
    pub direct_port: i64,
    // The host's TURN credential by this record; None for a lobby that holds none (a direct one, or a
    // master with no TURN configured).
    pub turn: Option<HostTurn>,
    // How the host's players reach it, as the host measures each connection and sends with its beats.
    pub links: LobbyLinks,
}

/// A lobby's players counted by the link the host measures on each (relayed, direct, on the LAN), the host
/// itself not counted: what other players' links to this host are, not the path a new joiner will get.
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct LobbyLinks {
    pub relayed: i64,
    pub direct: i64,
    pub lan: i64,
}

impl LobbyLinks {
    /// A beat's counts, each clamped to the lobby's size; None when the beat carries none.
    pub fn from_beat(body: &Value, cap: i64) -> Option<LobbyLinks> {
        let l = body.get("links")?;
        Some(LobbyLinks {
            relayed: as_int(l, "relayed", 0).clamp(0, cap),
            direct: as_int(l, "direct", 0).clamp(0, cap),
            lan: as_int(l, "lan", 0).clamp(0, cap),
        })
    }

    /// The row's one word: "relay" when any player comes through the relay, else "direct", else "lan",
    /// else "" before the host has measured anyone. A DIRECT lobby says "direct" from its mode: its
    /// joiners dial the host's own address.
    pub fn word(&self, conn: &str) -> &'static str {
        if conn == "direct" {
            "direct"
        } else if self.relayed > 0 {
            "relay"
        } else if self.direct > 0 {
            "direct"
        } else if self.lan > 0 {
            "lan"
        } else {
            ""
        }
    }
}

impl Lobby {
    /// A row with its ids drawn (the tests' way to put a row in a table).
    #[cfg(test)]
    fn new(ip: &str) -> Lobby {
        let mut lo = Lobby::draft(ip);
        lo.draw_ids();
        lo
    }

    /// The three random ids: session, public lobby id, host token.
    fn draw_ids(&mut self) {
        self.session_id = token_hex(16);
        self.lobby_id = token_hex(8);
        self.token = token_urlsafe(24);
    }

    /// A row with no ids yet. `h_host` fills it from the body and draws the ids only after
    /// every refusal has passed, so a refused create pays for no randomness.
    fn draft(ip: &str) -> Lobby {
        Lobby {
            session_id: String::new(),
            lobby_id: String::new(),
            token: String::new(),
            // Empty until `/v1/host` supplies the host's own public key, which it
            // must (see h_host). The master no longer mints a routing name: a name
            // it asserts on a host's behalf is one nobody can prove, and since
            // 2026-08-29 the relay registers only proved keys.
            host_identity: String::new(),
            proved: false,
            name: String::new(),
            version: String::new(),
            game: String::new(),
            game_build: String::new(),
            proto: 0,
            world: String::new(),
            locked: false,
            players_cur: 0,
            players_max: 4,
            listed: true,
            build_sha: String::new(),
            build_key: 0,
            build_sig: String::new(),
            last_seen: Instant::now(),
            ip: ip.to_string(),
            conn: "p2p".to_string(),
            direct_port: 0,
            turn: None,
            links: LobbyLinks::default(),
        }
    }
}

pub struct MasterState {
    pub lobbies: HashMap<String, Lobby>,      // sessionId -> Lobby
    pub lobby_by_public: HashMap<String, String>, // lobbyId -> sessionId
    pub rate: RateTable, // "ipbucket|class" -> recent timestamps, capped at MAX_RATE_KEYS
    pub cache_t: Option<Instant>,
    // Arc so /v1/lobbies can clone the cached body cheaply under the lock and write the
    // response OUTSIDE it (audit L5: serializing a ~200 KB body under the global lock
    // serialized it against all host/join/heartbeat processing).
    pub cache_all_body: Arc<Vec<u8>>,
    pub nonces: Nonces, // minted by /v1/host/challenge, spent by /v1/host
}

impl MasterState {
    fn new() -> MasterState {
        MasterState {
            lobbies: HashMap::new(),
            lobby_by_public: HashMap::new(),
            rate: RateTable::new(MAX_RATE_KEYS),
            nonces: Nonces::new(),
            cache_t: None,
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

/// The build claim of a `/v1/host` body: (sha, key, sig). A body without `build_sha` carries none (the
/// defaults, silently); a claim that is not well formed is dropped to the defaults with one log line
/// and the host is still listed. The claim is optional data and the master verifies nothing, so a bad
/// one costs the host nothing but its claim.
pub fn build_claim_from_body(body: &Value) -> (String, u8, String) {
    let Some(sha) = body.get("build_sha").and_then(Value::as_str) else {
        return (String::new(), 0, String::new());
    };
    let key = body.get("build_key").and_then(Value::as_i64).unwrap_or(0);
    let sig = body.get("build_sig").and_then(Value::as_str).unwrap_or("");
    if build_claim_shape_ok(sha, key, sig) {
        (sha.to_string(), key as u8, sig.to_string())
    } else {
        log("host announce: a malformed build claim was dropped");
        (String::new(), 0, String::new())
    }
}

/// Whether a `/v1/host` body lists the lobby: its `listed`, true when it sends none.
pub fn host_listed(body: &Value) -> bool {
    as_bool(body, "listed", true)
}

// ---- helpers ----------------------------------------------------------------

pub fn resolve_client_ip(peer_ip: &str, x_real_ip: Option<&str>, x_forwarded_for: Option<&str>) -> String {
    if TRUSTED_PROXY_PEERS.contains(&peer_ip) {
        if let Some(xr) = x_real_ip {
            let xr = xr.trim();
            if !xr.is_empty() {
                return xr.to_string();
            }
        }
        if let Some(xff) = x_forwarded_for {
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
    state.rate.hit(&key, Instant::now(), window, limit)
}

/// `GET /v1/lobbies`: the cached list body for an address within its read rate, else None (the
/// caller answers `429`).
pub fn h_browse(state: &mut MasterState, ip: &str) -> Option<Arc<Vec<u8>>> {
    if !rate_ok(state, ip, "browse", RL_BROWSE.0, RL_BROWSE.1) {
        return None;
    }
    Some(lobbies_snapshot(state))
}


pub fn drop_lobby(state: &mut MasterState, session_id: &str, lobby_id: &str) {
    state.lobbies.remove(session_id);
    state.lobby_by_public.remove(lobby_id);
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

pub fn h_heartbeat(state: &mut MasterState, ip: &str, body: &Value) -> (u16, Value) {
    if !rate_ok(state, ip, "mutate", RL_MUTATE.0, RL_MUTATE.1) {
        return (429, json!({"error": "rate"}));
    }
    // resolve identity first (immutable borrow), then mutate.
    let sid = match auth_by_session(state, body) {
        Some(lo) => lo.session_id.clone(),
        None => return (403, json!({"error": "unknown session or bad token"})),
    };
    let now = Instant::now();
    let lo = state.lobbies.get_mut(&sid).expect("just authed");
    let pc = as_int(body, "players_cur", lo.players_cur);
    lo.players_cur = pc.clamp(0, lo.players_max);
    if body_has(body, "listed") {
        lo.listed = as_bool(body, "listed", lo.listed);
    }
    if let Some(links) = LobbyLinks::from_beat(body, lo.players_max) {
        lo.links = links;
    }
    lo.last_seen = now;
    // The host's TURN credential: it allocates with it for every joiner its lobby takes, so the one from
    // /v1/host alone left a relay-only host unjoinable after its lifetime. A beat reports the username of the
    // credential its session holds; one that reports none is from a mod that takes no renewal and is
    // answered without one. SECURITY (A6): this endpoint once minted on EVERY beat, at RL_MUTATE rate, with
    // nothing reading the answer, and that minting was retired. The one mint here is bounded by the record's
    // own half-life, one per lobby per TURN_TTL_HOST / 2, so an address at MAX_LOBBIES_PER_IP lobbies mints
    // fewer a minute than /v1/join's own RL_JOIN lets it; it goes only to a host that reads it, and a re-send
    // of the record mints nothing.
    let before = lo.turn.as_ref().map(|t| t.username.clone());
    let minted = || turn_creds(&CFG.turn_uri, &CFG.turn_secret, &ip_bucket(ip), TURN_TTL_HOST);
    if let Some(block) = heartbeat_turn(&mut lo.turn, as_str(body, "turn_user"), now, minted) {
        let renewed = lo.turn.as_ref().map(|t| &t.username) != before.as_ref();
        log(&format!("heartbeat {} turn {}", lo.lobby_id, if renewed { "renewed" } else { "re-sent" }));
        return (200, json!({"ok": true, "turn": block}));
    }
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
    resp.extend(ice_block(&ip_bucket(ip), TURN_TTL_JOIN));
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
            "identity": lo.host_identity, // the host's key; `proved` says it signed for it
            "proved": lo.proved,
            "name": lo.name,
            "version": lo.version,
            "game": lo.game,    // v122: the host's VOTV game target (gate tier 1)
            "game_build": lo.game_build,  // b222: the game build the host runs, compared byte for byte
            "proto": lo.proto,
            "world": lo.world,
            "locked": lo.locked,
            "players_cur": lo.players_cur,
            "players_max": lo.players_max,
            "age": now.duration_since(lo.last_seen).as_secs() as i64,
            "conn": lo.conn,
            "link": lo.links.word(&lo.conn),
            "links": {"relayed": lo.links.relayed, "direct": lo.links.direct, "lan": lo.links.lan},
            "build_sha": lo.build_sha,
            "build_key": lo.build_key,
            "build_sig": lo.build_sig,
        }));
    }
    rows
}

/// Refresh the /v1/lobbies cache if stale and return a cheap Arc handle to the cached
/// body. Called UNDER the lock; the caller writes it AFTER dropping the lock (audit
/// L5 — the ~200 KB body is no longer cloned or serialized under the global mutex).
pub fn lobbies_snapshot(state: &mut MasterState) -> Arc<Vec<u8>> {
    let now = Instant::now();
    let stale = match state.cache_t {
        Some(t) => now.duration_since(t) >= LOBBIES_CACHE_TTL,
        None => true,
    };
    if stale {
        let rows = build_rows(state);
        let body = serde_json::to_vec(&json!({"lobbies": rows})).unwrap_or_default();
        state.cache_all_body = Arc::new(body);
        state.cache_t = Some(now);
    }
    Arc::clone(&state.cache_all_body)
}

pub fn dispatch_post(path: &str, ip: &str, body: &Value) -> Option<(u16, Value)> {
    let mut state = lock_state();
    let r = match path {
        "/v1/host" => h_host(&mut state, &HostCtx::live(), ip, body),
        "/v1/host/challenge" => h_challenge(&mut state, Instant::now(), ip),
        "/v1/heartbeat" => h_heartbeat(&mut state, ip, body),
        "/v1/leave" => h_leave(&mut state, ip, body),
        "/v1/visibility" => h_visibility(&mut state, ip, body),
        "/v1/join" => h_join(&mut state, ip, body),
        _ => return None,
    };
    Some(r)
}

pub async fn sweeper() {
    let max_window = [RL_CHALLENGE.0, RL_CREATE.0, RL_JOIN.0, RL_MUTATE.0, RL_BROWSE.0].into_iter().max().unwrap();
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
        // prune fully-stale rate keys (else `rate` holds one entry per distinct IP until the cap)
        state.rate.prune(now, max_window);
        let forgotten = state.rate.take_forgotten();
        if forgotten > 0 {
            log(&format!("rate table at its cap: forgot {forgotten} least-recently-seen keys to admit new ones"));
        }
    }
}

#[cfg(test)]
mod tests;
