//! The create domain: `/v1/host/challenge` and `/v1/host`, the handlers that list a lobby, and the
//! row-table helpers they alone use (replace by key, the per-address count, the eviction).

use super::{
    as_bool, as_int, as_str, build_claim_from_body, clamp_field, drop_lobby, host_listed, rate_ok, Lobby,
    MasterState, MAX_GAME, MAX_GAME_BUILD, MAX_LOBBIES_GLOBAL, MAX_LOBBIES_PER_IP, MAX_NAME, MAX_VERSION,
    MAX_WORLD, RL_CHALLENGE, RL_CREATE,
};
use crate::common::{identity_shape_ok, ip_bucket, log};
use crate::ice::{ice_block, HostTurn, TURN_TTL_HOST};
use crate::lobby_proof::judge;
use crate::master_config::CFG;
use crate::version_gate::{below_floor, version_gate_env};
use serde_json::{json, Value};
use std::time::Instant;

/// Make room for one more row: while the table is full, drop the stalest UNPROVED row, and only
/// when every row is proved the stalest proved one. A proved create is never refused at the cap;
/// an unproved one is, before this runs, when every row is proved. MTA caps nothing: each client
/// queries the port of every row it lists itself (Client/core/ServerBrowser/CServerList.cpp:490-595),
/// so a bogus row costs a viewer a probe and the master nothing (its own side is not in the
/// reference tree); a master that takes pushes needs a cap, and the proof ranks who leaves first.
pub fn evict_if_full(state: &mut MasterState) {
    while state.lobbies.len() >= MAX_LOBBIES_GLOBAL {
        let victim = state
            .lobbies
            .values()
            .min_by_key(|lo| (lo.proved, lo.last_seen))
            .map(|lo| (lo.session_id.clone(), lo.lobby_id.clone(), lo.proved));
        match victim {
            Some((sid, pub_id, proved)) => {
                drop_lobby(state, &sid, &pub_id);
                let tier = if proved { "proved" } else { "unproved" };
                log(&format!("evicted stalest {tier} lobby {pub_id} (global cap)"));
            }
            None => break,
        }
    }
}

/// The (session, lobby) ids of every row that lists `identity`.
fn rows_of_key(state: &MasterState, identity: &str) -> Vec<(String, String)> {
    let rows = state.lobbies.values().filter(|lo| lo.host_identity == identity);
    rows.map(|lo| (lo.session_id.clone(), lo.lobby_id.clone())).collect()
}

pub fn lobbies_per_ip(state: &MasterState, ip: &str) -> usize {
    // Count by the coarse bucket (audit M3), not the exact address, so an IPv6 /64
    // can't mint MAX_LOBBIES_PER_IP lobbies per /128.
    let bucket = ip_bucket(ip);
    state.lobbies.values().filter(|lo| ip_bucket(&lo.ip) == bucket).count()
}

/// `POST /v1/host/challenge`: the nonce a create signs. Counted in its own rate class, bound to
/// the caller's address bucket, single use, alive 30 s. Issuing stores nothing (see
/// `lobby_proof::Nonces`), so no flood of challenges can fill a table; the rate class cannot
/// refuse an honest host either (`rate_table`). MTA's announce has no challenge
/// (CMasterServerAnnouncer.h:235-267) and its server-side one is answered, not issued
/// (CHTTPD.cpp:131-170): here the master issues it because it is the party that must be convinced.
pub fn h_challenge(state: &mut MasterState, now: Instant, ip: &str) -> (u16, Value) {
    if !rate_ok(state, ip, "challenge", RL_CHALLENGE.0, RL_CHALLENGE.1) {
        return (429, json!({"error": "rate"}));
    }
    (200, json!({"nonce": state.nonces.issue(&ip_bucket(ip), now)}))
}

/// What `h_host` reads from outside the request: the clock, this master's names, the floor, the block list.
pub struct HostCtx<'a> {
    pub now: Instant,
    pub names: &'a [String],
    pub min_listed_proto: i64,
    /// The operator's block list (`COOP_BLOCKED_IDENTITIES`): identities no create may list.
    pub blocked: &'a [String],
}

impl HostCtx<'static> {
    pub fn live() -> HostCtx<'static> {
        HostCtx {
            now: Instant::now(),
            names: &CFG.public_names,
            min_listed_proto: CFG.min_listed_proto,
            blocked: &CFG.blocked_identities,
        }
    }
}

/// `POST /v1/host`. Refusals run in this order: shape, block list, nonce, proof, floor, caps, then the eviction.
pub fn h_host(state: &mut MasterState, ctx: &HostCtx, ip: &str, body: &Value) -> (u16, Value) {
    if !rate_ok(state, ip, "create", RL_CREATE.0, RL_CREATE.1) {
        return (429, json!({"error": "rate"}));
    }

    let mut lo = Lobby::draft(ip);
    let name = clamp_field(body, "name", MAX_NAME);
    lo.name = if name.is_empty() { "VOTV Coop".into() } else { name };
    // v122 Paper-pair identity: game + proto. `version` is the LEGACY pre-v122
    // display tag (old hosts sent their game version here) -- pass-through, no
    // fabricated default.
    lo.version = clamp_field(body, "version", MAX_VERSION);
    lo.game = clamp_field(body, "game", MAX_GAME);
    lo.game_build = clamp_field(body, "game_build", MAX_GAME_BUILD);
    lo.proto = as_int(body, "proto", 0).clamp(0, 65535);
    // Version-pair gate (2026-08-29): shape-invalid game / impossible build
    // numbers are refused BEFORE the lobby exists -- see version_gate.rs.
    if let Err(reason) = version_gate_env(&lo.game, lo.proto, &lo.game_build) {
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
    (lo.build_sha, lo.build_key, lo.build_sig) = build_claim_from_body(body);
    lo.listed = host_listed(body);

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
    lo.last_seen = ctx.now;

    // The proof. A nonce is spent here, so every shape refusal above leaves it alive.
    match judge(&mut state.nonces, ctx.names, &ip_bucket(ip), ctx.now, body, &lo.host_identity, lo.proto) {
        Ok(proved) => lo.proved = proved,
        Err(why) => {
            let (status, error) = why.answer();
            log(&format!("host REFUSED from {ip}: {error} (b{})", lo.proto));
            return (status, json!({"error": error}));
        }
    }
    if below_floor(lo.proto, ctx.min_listed_proto) {
        log(&format!("host REFUSED from {ip}: b{} is below the listed floor {}", lo.proto, ctx.min_listed_proto));
        return (403, json!({"error": "build-too-old"}));
    }
    // The operator's block (MTA's master flags a row RESTRICTION_BLOCK_SERVER and every client hides
    // it, Client/core/ServerBrowser/CServerList.h:75 and CServerBrowser.cpp:1358). Asked after the
    // proof and the floor: a build that must prove learns its key is blocked only by proving it, so
    // an unauthenticated create cannot probe the list. A create of an older build, which need not
    // prove, proves nothing and so may learn it; it is refused here all the same. A proved blocked
    // create has spent its nonce, as any proved create does.
    if ctx.blocked.contains(&lo.host_identity) {
        log(&format!("host REFUSED from {ip}: blocked identity (b{})", lo.proto));
        return (403, json!({"error": "blocked"}));
    }

    // One row per proved key: the caps count the rows listing it as gone; nothing drops until they pass.
    // MTA's identity of a row is address and port (CServerList.cpp:700-706); a P2P host has no stable
    // address a joiner could dial, so its key is the identity and a restarted host keeps one row.
    let replaced = if lo.proved { rows_of_key(state, &lo.host_identity) } else { Vec::new() };
    let mine = |sid: &String| state.lobbies.get(sid).is_some_and(|row| ip_bucket(&row.ip) == ip_bucket(ip));
    if lobbies_per_ip(state, ip) - replaced.iter().filter(|(sid, _)| mine(sid)).count() >= MAX_LOBBIES_PER_IP {
        return (429, json!({"error": "too many lobbies for this address"}));
    }
    // A table of nothing but proved rows is not made room in by an unproved create: that would
    // let unproved junk churn real hosts out. A proved create always gets a place.
    if !lo.proved && state.lobbies.len() >= MAX_LOBBIES_GLOBAL && state.lobbies.values().all(|row| row.proved) {
        log(&format!("host REFUSED from {ip}: b{} unproved, every row is proved and the table is full", lo.proto));
        return (429, json!({"error": "the master is full of proved lobbies"}));
    }
    lo.draw_ids();
    if let Some((_, public_id)) = replaced.first() {
        lo.lobby_id = public_id.clone(); // the row keeps its public id; session and token are re-issued
    }
    for (sid, public_id) in &replaced {
        drop_lobby(state, sid, public_id);
    }

    let session_id = lo.session_id.clone();
    let lobby_id = lo.lobby_id.clone();
    let host_identity = lo.host_identity.clone();
    let token = lo.token.clone();
    let conn = lo.conn.clone();
    let is_direct = conn == "direct";

    // Room is made only for a create that passed every check above: a refused one must cost
    // nobody else their lobby.
    evict_if_full(state);
    let said = format!("host {lobby_id} '{}' game={} ({}) b{}", lo.name, lo.game, lo.game_build, lo.proto);
    let said = format!("{said} {} from {ip}", if lo.proved { "proved" } else { "unproved" });
    state.lobby_by_public.insert(lobby_id.clone(), session_id.clone());
    state.lobbies.insert(session_id.clone(), lo);
    log(&format!("{said} ({} live)", state.lobbies.len()));

    let mut resp = serde_json::Map::new();
    resp.insert("sessionId".into(), json!(session_id));
    resp.insert("lobbyId".into(), json!(lobby_id));
    resp.insert("hostIdentity".into(), json!(host_identity));
    resp.insert("token".into(), json!(token));
    resp.insert("conn".into(), json!(conn));
    // A DIRECT host never touches signaling/ICE -> no creds. P2P hosts get the block.
    // TURN cred is bound to the client's IP bucket (audit M2), not the host identity.
    if !is_direct {
        let block = ice_block(&ip_bucket(ip), TURN_TTL_HOST);
        if let (Some(turn), Some(lo)) = (block.get("turn"), state.lobbies.get_mut(&session_id)) {
            lo.turn = HostTurn::minted_now(turn, Instant::now());
        }
        resp.extend(block);
    }
    (200, Value::Object(resp))
}
