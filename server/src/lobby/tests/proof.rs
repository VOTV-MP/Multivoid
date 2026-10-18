//! `h_host` and the announce proof: a handler-level test over a constructed `MasterState`, with the
//! master's clock, names, floor and block list injected through `HostCtx`.

use crate::lobby::host::{h_challenge, h_host, HostCtx};
use crate::lobby::{build_rows, h_heartbeat, Lobby, MasterState, MAX_LOBBIES_GLOBAL};
use crate::lobby_proof::{blob, NONCE_LIFE};
use crate::version_gate::MIN_PROVED_PROTO;
use ring::rand::SystemRandom;
use ring::signature::{Ed25519KeyPair, KeyPair};
use serde_json::{json, Value};
use std::time::{Duration, Instant};

const NAME: &str = "master.multivoid.dev";
const IP: &str = "198.51.100.9";
const NEW: i64 = MIN_PROVED_PROTO;
const OLD: i64 = MIN_PROVED_PROTO - 1;

struct Host {
    ident: String,
    kp: Ed25519KeyPair,
}

fn host() -> Host {
    let pkcs8 = Ed25519KeyPair::generate_pkcs8(&SystemRandom::new()).unwrap();
    let kp = Ed25519KeyPair::from_pkcs8(pkcs8.as_ref()).unwrap();
    let ident = format!("gen:{}", hex(kp.public_key().as_ref()));
    Host { ident, kp }
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn sign(h: &Host, nonce: &str, name: &str) -> String {
    hex(h.kp.sign(&blob(&h.ident, nonce, name)).as_ref())
}

/// The master's side of the world: its clock, the names it answers to, its floor, its block list.
struct Env {
    now: Instant,
    names: Vec<String>,
    floor: i64,
    blocked: Vec<String>,
}

impl Env {
    fn new() -> Env {
        Env { now: Instant::now(), names: vec![NAME.to_string()], floor: 0, blocked: Vec::new() }
    }

    fn create(&self, state: &mut MasterState, ip: &str, body: &Value) -> (u16, Value) {
        let ctx = HostCtx { now: self.now, names: &self.names, min_listed_proto: self.floor, blocked: &self.blocked };
        h_host(state, &ctx, ip, body)
    }

    fn nonce(&self, state: &mut MasterState, ip: &str) -> String {
        let (status, answer) = h_challenge(state, self.now, ip);
        assert_eq!(status, 200, "the challenge was refused: {answer}");
        answer["nonce"].as_str().unwrap().to_string()
    }

    /// A create body for `h` carrying a fresh nonce for `ip`, signed for `name`.
    fn signed(&self, state: &mut MasterState, ip: &str, h: &Host, proto: i64, name: &str) -> Value {
        let nonce = self.nonce(state, ip);
        let mut body = plain(h, proto);
        body["proof"] = json!(sign(h, &nonce, name));
        body["nonce"] = json!(nonce);
        body
    }
}

/// A create body with no nonce and no proof: what a build that cannot sign sends.
fn plain(h: &Host, proto: i64) -> Value {
    json!({"name": "t", "game": "0.9.0n", "proto": proto, "identity": h.ident, "conn": "direct", "direct_port": 7777})
}

fn error_of(r: &(u16, Value)) -> (u16, &str) {
    (r.0, r.1["error"].as_str().unwrap_or(""))
}

fn rows_of<'a>(rows: &'a [Value], h: &Host) -> Vec<&'a Value> {
    rows.iter().filter(|r| r["identity"] == h.ident).collect()
}

/// A table filled to the global cap, each row from its own address; row `i` is `i` milliseconds
/// newer than `base`, and `proved(i)` says whether it is proved.
fn full_table(proved: impl Fn(usize) -> bool, base: Instant) -> (MasterState, Vec<String>) {
    let mut state = MasterState::new();
    let mut sessions = Vec::new();
    for i in 0..MAX_LOBBIES_GLOBAL {
        let mut lo = Lobby::new(&format!("10.{}.{}.1", i / 250, i % 250));
        lo.host_identity = format!("gen:{i:064x}");
        lo.proved = proved(i);
        lo.last_seen = base + Duration::from_millis(i as u64);
        sessions.push(lo.session_id.clone());
        state.lobby_by_public.insert(lo.lobby_id.clone(), lo.session_id.clone());
        state.lobbies.insert(lo.session_id.clone(), lo);
    }
    (state, sessions)
}

#[test]
fn a_valid_proof_lists_a_proved_row_that_names_its_key() {
    let (env, mut state, h) = (Env::new(), MasterState::new(), host());
    let body = env.signed(&mut state, IP, &h, NEW, NAME);
    let (status, answer) = env.create(&mut state, IP, &body);
    assert_eq!(status, 200, "{answer}");
    let rows = build_rows(&state);
    let mine = rows_of(&rows, &h);
    assert_eq!(mine.len(), 1);
    assert_eq!(mine[0]["proved"], true);
    assert_eq!(mine[0]["lobbyId"], answer["lobbyId"]);
}

#[test]
fn a_forged_proof_is_refused_and_lists_nothing() {
    let (env, mut state, victim, attacker) = (Env::new(), MasterState::new(), host(), host());
    // The attacker signs the blob that names the VICTIM's key, with its own.
    let nonce = env.nonce(&mut state, IP);
    let mut body = plain(&victim, NEW);
    body["nonce"] = json!(nonce);
    body["proof"] = json!(hex(attacker.kp.sign(&blob(&victim.ident, &nonce, NAME)).as_ref()));
    assert_eq!(error_of(&env.create(&mut state, IP, &body)), (400, "bad-proof"));
    assert!(state.lobbies.is_empty(), "a forged proof listed a row");
}

#[test]
fn a_proof_that_is_present_must_verify_whatever_the_build() {
    let (env, mut state, h, other) = (Env::new(), MasterState::new(), host(), host());
    let nonce = env.nonce(&mut state, IP);
    let mut body = plain(&h, OLD);
    body["nonce"] = json!(nonce);
    body["proof"] = json!(sign(&other, &nonce, NAME));
    assert_eq!(error_of(&env.create(&mut state, IP, &body)), (400, "bad-proof"));
    assert!(state.lobbies.is_empty());
}

#[test]
fn a_malformed_proof_or_nonce_is_bad_proof_before_any_nonce_is_spent() {
    let (env, mut state, h) = (Env::new(), MasterState::new(), host());
    let nonce = env.nonce(&mut state, IP);
    let good = sign(&h, &nonce, NAME);
    for (n, p) in [(nonce.as_str(), &good[..126]), (nonce.as_str(), ""), (&nonce[..60], good.as_str()), ("", good.as_str())] {
        let mut body = plain(&h, NEW);
        body["nonce"] = json!(n);
        body["proof"] = json!(p);
        assert_eq!(error_of(&env.create(&mut state, IP, &body)), (400, "bad-proof"), "nonce {n:?}");
    }
    let mut body = plain(&h, NEW);
    body["nonce"] = json!(nonce);
    body["proof"] = json!(7);
    assert_eq!(error_of(&env.create(&mut state, IP, &body)), (400, "bad-proof"));
    // None of those spent the nonce: the honest create with it still goes through.
    body["proof"] = json!(good);
    assert_eq!(env.create(&mut state, IP, &body).0, 200);
}

#[test]
fn a_replayed_nonce_is_expired() {
    let (env, mut state, h) = (Env::new(), MasterState::new(), host());
    let body = env.signed(&mut state, IP, &h, NEW, NAME);
    assert_eq!(env.create(&mut state, IP, &body).0, 200);
    assert_eq!(error_of(&env.create(&mut state, IP, &body)), (410, "nonce-expired"));
}

#[test]
fn another_buckets_nonce_is_expired_and_its_owner_keeps_it() {
    let (env, mut state, h) = (Env::new(), MasterState::new(), host());
    let body = env.signed(&mut state, IP, &h, NEW, NAME);
    assert_eq!(error_of(&env.create(&mut state, "198.51.100.77", &body)), (410, "nonce-expired"));
    assert!(state.lobbies.is_empty());
    assert_eq!(env.create(&mut state, IP, &body).0, 200, "the owner lost its nonce to a stranger");
}

#[test]
fn a_nonce_older_than_thirty_seconds_is_expired() {
    let (mut env, mut state, h) = (Env::new(), MasterState::new(), host());
    let body = env.signed(&mut state, IP, &h, NEW, NAME);
    // The expiry is whole seconds, so a nonce lives a little over thirty: two more clears it.
    env.now += NONCE_LIFE + Duration::from_secs(2);
    assert_eq!(error_of(&env.create(&mut state, IP, &body)), (410, "nonce-expired"));
}

#[test]
fn a_nonce_still_answers_at_thirty_seconds() {
    let (mut env, mut state, h) = (Env::new(), MasterState::new(), host());
    let body = env.signed(&mut state, IP, &h, NEW, NAME);
    env.now += NONCE_LIFE;
    assert_eq!(env.create(&mut state, IP, &body).0, 200, "a nonce died before its thirty seconds");
}

#[test]
fn a_nonce_with_a_flipped_digit_is_expired_even_with_a_valid_proof_over_it() {
    let (env, mut state, h) = (Env::new(), MasterState::new(), host());
    let nonce = env.nonce(&mut state, IP);
    for at in [0, 20, 63] {
        let mut digits = nonce.clone().into_bytes();
        digits[at] = if digits[at] == b'0' { b'1' } else { b'0' };
        let forged = String::from_utf8(digits).unwrap();
        let mut body = plain(&h, NEW);
        body["proof"] = json!(sign(&h, &forged, NAME)); // the holder signs the forged nonce itself
        body["nonce"] = json!(forged);
        assert_eq!(error_of(&env.create(&mut state, IP, &body)), (410, "nonce-expired"), "digit {at}");
    }
    assert!(state.lobbies.is_empty());
}

#[test]
fn a_nonce_from_another_master_is_expired() {
    let (env, mut one, mut two, h) = (Env::new(), MasterState::new(), MasterState::new(), host());
    let body = env.signed(&mut one, IP, &h, NEW, NAME);
    assert_eq!(error_of(&env.create(&mut two, IP, &body)), (410, "nonce-expired"), "two secrets, one nonce");
    assert_eq!(env.create(&mut one, IP, &body).0, 200, "the issuing master lost its own nonce");
}

#[test]
fn a_flood_of_challenges_refuses_no_honest_create() {
    let (env, mut state, h) = (Env::new(), MasterState::new(), host());
    for n in 0..2000 {
        let ip = format!("203.0.{}.{}", n / 250, n % 250);
        assert_eq!(h_challenge(&mut state, env.now, &ip).0, 200);
    }
    let body = env.signed(&mut state, IP, &h, NEW, NAME);
    assert_eq!(env.create(&mut state, IP, &body).0, 200);
}

#[test]
fn an_unproved_create_at_a_full_table_of_proved_rows_is_refused_and_evicts_nobody() {
    let (env, h) = (Env::new(), host());
    let (mut state, sessions) = full_table(|_| true, env.now);
    let refused = env.create(&mut state, IP, &plain(&h, OLD));
    assert_eq!(refused.0, 429, "{}", refused.1);
    assert_eq!(state.lobbies.len(), MAX_LOBBIES_GLOBAL);
    assert!(sessions.iter().all(|s| state.lobbies.contains_key(s)), "an unproved create evicted a proved row");
    // The refusal costs a proved host nothing: its create still takes the stalest proved row's place.
    let body = env.signed(&mut state, IP, &h, NEW, NAME);
    assert_eq!(env.create(&mut state, IP, &body).0, 200);
    assert!(!state.lobbies.contains_key(&sessions[0]));
}

#[test]
fn a_proof_signed_for_another_master_is_refused_and_a_configured_alias_is_accepted() {
    let (mut env, mut state, h) = (Env::new(), MasterState::new(), host());
    let rogue = env.signed(&mut state, IP, &h, NEW, "rogue.example");
    assert_eq!(error_of(&env.create(&mut state, IP, &rogue)), (400, "bad-proof"));
    let alias = env.signed(&mut state, IP, &h, NEW, "203.0.113.7");
    assert_eq!(error_of(&env.create(&mut state, IP, &alias)), (400, "bad-proof"), "an unlisted name was accepted");
    env.names.push("203.0.113.7".to_string());
    let alias = env.signed(&mut state, IP, &h, NEW, "203.0.113.7");
    assert_eq!(env.create(&mut state, IP, &alias).0, 200, "a configured alias was refused");
}

#[test]
fn a_build_that_signs_is_refused_without_a_proof_and_an_older_one_is_listed_unproved() {
    let (env, mut state, new_host, old_host) = (Env::new(), MasterState::new(), host(), host());
    assert_eq!(error_of(&env.create(&mut state, IP, &plain(&new_host, NEW))), (409, "proof-required"));
    assert_eq!(error_of(&env.create(&mut state, IP, &plain(&new_host, 65535))), (409, "proof-required"));
    assert!(state.lobbies.is_empty());
    assert_eq!(env.create(&mut state, IP, &plain(&old_host, OLD)).0, 200);
    let rows = build_rows(&state);
    let mine = rows_of(&rows, &old_host);
    assert_eq!((mine.len(), &mine[0]["proved"]), (1, &json!(false)));
}

#[test]
fn a_build_below_the_floor_is_refused_after_its_proof_is_judged() {
    let (mut env, mut state, h) = (Env::new(), MasterState::new(), host());
    env.floor = NEW;
    assert_eq!(error_of(&env.create(&mut state, IP, &plain(&h, OLD))), (403, "build-too-old"));
    // A bad proof is judged first, so it is a 400 even below the floor.
    let nonce = env.nonce(&mut state, IP);
    let mut body = plain(&h, OLD);
    body["nonce"] = json!(nonce);
    body["proof"] = json!("00".repeat(64));
    assert_eq!(error_of(&env.create(&mut state, IP, &body)), (400, "bad-proof"));
    let at_floor = env.signed(&mut state, IP, &h, NEW, NAME);
    assert_eq!(env.create(&mut state, IP, &at_floor).0, 200);
    assert_eq!(state.lobbies.len(), 1);
}

#[test]
fn a_full_table_evicts_the_stalest_unproved_row_before_any_proved_one() {
    let (env, h) = (Env::new(), host());
    // Rows 0..500 are proved and the OLDEST; rows 500..1000 are unproved and newer.
    let (mut state, sessions) = full_table(|i| i < 500, env.now);
    let body = env.signed(&mut state, IP, &h, NEW, NAME);
    assert_eq!(env.create(&mut state, IP, &body).0, 200, "a proved create was refused at the cap");
    assert_eq!(state.lobbies.len(), MAX_LOBBIES_GLOBAL);
    assert!(!state.lobbies.contains_key(&sessions[500]), "the stalest unproved row survived");
    assert!(sessions[..500].iter().all(|s| state.lobbies.contains_key(s)), "a proved row was evicted");
    assert!(sessions[501..].iter().all(|s| state.lobbies.contains_key(s)), "a newer unproved row was evicted");
}

#[test]
fn a_full_table_of_proved_rows_evicts_the_stalest_proved_and_still_lists_the_create() {
    let (env, h) = (Env::new(), host());
    let (mut state, sessions) = full_table(|_| true, env.now);
    let body = env.signed(&mut state, IP, &h, NEW, NAME);
    assert_eq!(env.create(&mut state, IP, &body).0, 200);
    assert_eq!(state.lobbies.len(), MAX_LOBBIES_GLOBAL);
    assert!(!state.lobbies.contains_key(&sessions[0]), "the stalest proved row survived");
    assert!(sessions[1..].iter().all(|s| state.lobbies.contains_key(s)));
}

#[test]
fn a_flood_of_unproved_creates_never_displaces_a_proved_row() {
    let mut env = Env::new();
    let (mut state, sessions) = full_table(|i| i == 0, env.now); // one proved row, the stalest
    env.now += Duration::from_secs(2);
    for n in 0..40 {
        let ip = format!("203.0.113.{n}");
        assert_eq!(env.create(&mut state, &ip, &plain(&host(), OLD)).0, 200);
    }
    assert!(state.lobbies.contains_key(&sessions[0]), "40 unproved creates evicted the proved row");
    assert_eq!(state.lobbies.len(), MAX_LOBBIES_GLOBAL);
}

#[test]
fn a_proved_create_replaces_the_row_of_its_key_in_place() {
    let (env, mut state, h) = (Env::new(), MasterState::new(), host());
    let first = env.signed(&mut state, IP, &h, NEW, NAME);
    let (_, a) = env.create(&mut state, IP, &first);
    let second = env.signed(&mut state, IP, &h, NEW, NAME);
    let (status, b) = env.create(&mut state, IP, &second);
    assert_eq!(status, 200);
    assert_eq!(state.lobbies.len(), 1, "a restart added a row");
    assert_eq!(a["lobbyId"], b["lobbyId"], "the row lost its public id");
    assert_ne!(a["sessionId"], b["sessionId"]);
    assert_ne!(a["token"], b["token"]);
    let old_beat = json!({"sessionId": a["sessionId"], "token": a["token"]});
    assert_eq!(h_heartbeat(&mut state, IP, &old_beat).0, 403, "the old token still beats");
    let new_beat = json!({"sessionId": b["sessionId"], "token": b["token"]});
    assert_eq!(h_heartbeat(&mut state, IP, &new_beat).0, 200);
}

#[test]
fn a_proved_create_takes_over_an_unproved_row_listing_its_key_and_a_refused_one_does_not() {
    let (env, mut state, h) = (Env::new(), MasterState::new(), host());
    let (_, squat) = env.create(&mut state, "203.0.113.5", &plain(&h, OLD));
    // A refused proved create (forged) leaves the squatter's row alone.
    let nonce = env.nonce(&mut state, IP);
    let mut forged = plain(&h, NEW);
    forged["nonce"] = json!(nonce);
    forged["proof"] = json!(sign(&host(), &nonce, NAME));
    assert_eq!(env.create(&mut state, IP, &forged).0, 400);
    assert_eq!(state.lobbies.len(), 1);
    // The real holder's create replaces it, from another address.
    let real = env.signed(&mut state, IP, &h, NEW, NAME);
    let (status, answer) = env.create(&mut state, IP, &real);
    assert_eq!(status, 200);
    assert_eq!(state.lobbies.len(), 1);
    assert_eq!(answer["lobbyId"], squat["lobbyId"]);
    let rows = build_rows(&state);
    assert_eq!(rows_of(&rows, &h)[0]["proved"], true);
}

#[test]
fn a_replace_passes_the_per_address_cap_that_a_new_row_would_not() {
    let (env, h) = (Env::new(), host());
    let (mut state, ip) = (MasterState::new(), "203.0.113.200");
    let first = env.signed(&mut state, ip, &h, NEW, NAME);
    assert_eq!(env.create(&mut state, ip, &first).0, 200);
    for _ in 0..7 {
        // The rest of the address's 8 rows, put in directly so the create rate limit is not spent.
        let lo = Lobby::new(ip);
        state.lobby_by_public.insert(lo.lobby_id.clone(), lo.session_id.clone());
        state.lobbies.insert(lo.session_id.clone(), lo);
    }
    assert_eq!(state.lobbies.len(), 8);
    assert_eq!(env.create(&mut state, ip, &plain(&host(), OLD)).0, 429, "a 9th new row was listed");
    let restart = env.signed(&mut state, ip, &h, NEW, NAME);
    assert_eq!(env.create(&mut state, ip, &restart).0, 200, "a restart was refused at the address cap");
    assert_eq!(state.lobbies.len(), 8);
}

#[test]
fn a_replace_at_the_global_cap_evicts_nobody() {
    let (env, h) = (Env::new(), host());
    let (mut state, sessions) = full_table(|_| true, env.now);
    state.lobbies.get_mut(&sessions[500]).unwrap().host_identity = h.ident.clone(); // not the stalest
    let body = env.signed(&mut state, IP, &h, NEW, NAME);
    assert_eq!(env.create(&mut state, IP, &body).0, 200);
    assert_eq!(state.lobbies.len(), MAX_LOBBIES_GLOBAL);
    assert!(!state.lobbies.contains_key(&sessions[500]), "the old session of the key lived on");
    let bystanders = sessions.iter().enumerate().filter(|(i, _)| *i != 500).map(|(_, s)| s);
    assert!(bystanders.into_iter().all(|s| state.lobbies.contains_key(s)), "a replace evicted a bystander");
}

#[test]
fn the_eight_row_cap_per_address_still_holds_for_unproved_rows() {
    let (env, mut state) = (Env::new(), MasterState::new());
    for n in 0..8 {
        assert_eq!(env.create(&mut state, "203.0.113.50", &plain(&host(), OLD)).0, 200, "row {n}");
    }
    let (status, answer) = env.create(&mut state, "203.0.113.50", &plain(&host(), OLD));
    assert_eq!((status, answer["error"].as_str()), (429, Some("too many lobbies for this address")));
}

#[test]
fn a_blocked_identity_is_refused_403_blocked_proved_or_not() {
    let (mut env, mut state, h) = (Env::new(), MasterState::new(), host());
    env.blocked = vec![h.ident.clone()];
    let signed = env.signed(&mut state, IP, &h, NEW, NAME);
    assert_eq!(error_of(&env.create(&mut state, IP, &signed)), (403, "blocked"));
    assert_eq!(state.nonces.spent_len(), 1, "the proved create spent its nonce");
    assert_eq!(error_of(&env.create(&mut state, IP, &plain(&h, OLD))), (403, "blocked"), "an unproved create got through");
    assert!(state.lobbies.is_empty(), "a blocked create was listed");
}

#[test]
fn a_create_that_cannot_prove_the_key_does_not_learn_it_is_blocked() {
    let (mut env, mut state, h) = (Env::new(), MasterState::new(), host());
    env.blocked = vec![h.ident.clone()];
    assert_eq!(error_of(&env.create(&mut state, IP, &plain(&h, NEW))), (409, "proof-required"), "an unsigned create learned the list");
    let nonce = env.nonce(&mut state, IP);
    let mut forged = plain(&h, NEW);
    forged["nonce"] = json!(nonce);
    forged["proof"] = json!(sign(&host(), &nonce, NAME));
    assert_eq!(error_of(&env.create(&mut state, IP, &forged)), (400, "bad-proof"), "a forged proof learned the list");
}

#[test]
fn a_block_list_refuses_only_the_identities_it_names() {
    let (mut env, mut state, victim, other) = (Env::new(), MasterState::new(), host(), host());
    env.blocked = vec![victim.ident.clone()];
    let body = env.signed(&mut state, IP, &other, NEW, NAME);
    assert_eq!(env.create(&mut state, IP, &body).0, 200, "an unlisted identity was refused");
}
