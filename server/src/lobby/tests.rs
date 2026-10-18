//! The lobby domain's unit tests. The announce proof through `h_host` is in `tests/proof.rs`.

mod proof;

use super::host::{h_challenge, h_host, HostCtx};
use super::{
    build_claim_from_body, h_browse, host_listed, latest_answer, parse_latest_games, rate_ok, LatestGame, Lobby,
    LobbyLinks, MasterState, LATEST_URL, MAX_LOBBIES_GLOBAL, MAX_RATE_KEYS, RL_BROWSE,
};
use crate::common::{build_claim_shape_ok, identity_shape_ok};
use crate::lobby_proof::{MAX_SPENT, SPENT_ENTRY_BYTES};

#[test]
fn latest_games_parse_keeps_good_entries_and_names_bad_ones() {
    let (games, refused) =
        parse_latest_games("0.9.0n:221:0.9.0n b221, 0.10.0:230:0.10.0 b230,garbage,0.9.0n:0:x,0.9.0n:5:a:b");
    assert_eq!(games.len(), 2);
    assert_eq!(games[0], LatestGame { game: "0.9.0n".into(), proto: 221, tag: "0.9.0n b221".into() });
    assert_eq!(games[1].game, "0.10.0");
    assert_eq!(refused, vec!["garbage".to_string(), "0.9.0n:0:x".to_string(), "0.9.0n:5:a:b".to_string()]);
    let (dup, refused_dup) = parse_latest_games("0.9.0n:221:a,0.9.0n:222:b");
    assert_eq!(dup.len(), 1, "a game named twice keeps its first entry");
    assert_eq!(refused_dup, vec!["0.9.0n:222:b".to_string()]);
    let (none, refused_none) = parse_latest_games("");
    assert!(none.is_empty() && refused_none.is_empty());
}

#[test]
fn latest_answer_gives_the_old_cohort_its_own_game() {
    let (games, _) = parse_latest_games("0.9.0n:221:0.9.0n b221,0.10.0:230:0.10.0 b230");
    let body = latest_answer(&games, LATEST_URL);
    assert_eq!(body["proto"], 221, "the legacy fields are 0.9.0n's, not the newest release's");
    assert_eq!(body["mod"], "0.9.0n b221");
    assert_eq!(body["games"].as_array().map(|a| a.len()), Some(2));
    let empty = latest_answer(&[], LATEST_URL);
    assert_eq!(empty["proto"], 0);
    assert!(empty.get("games").is_none(), "no list, no field");
}

#[test]
fn a_lobby_row_names_its_players_links_in_one_word() {
    let l = |relayed, direct, lan| LobbyLinks { relayed, direct, lan };
    assert_eq!(l(0, 0, 0).word("p2p"), "");        // nobody measured yet
    assert_eq!(l(0, 0, 2).word("p2p"), "lan");
    assert_eq!(l(0, 1, 2).word("p2p"), "direct");
    assert_eq!(l(1, 1, 0).word("p2p"), "relay");   // any relayed player names the relay
    assert_eq!(l(0, 0, 0).word("direct"), "direct"); // a DIRECT lobby, before anyone joins
    assert_eq!(l(0, 0, 1).word("direct"), "direct");
}

#[test]
fn a_beats_links_are_clamped_to_the_lobby() {
    let body = serde_json::json!({"links": {"relayed": 9, "direct": -2, "lan": "1"}});
    assert_eq!(LobbyLinks::from_beat(&body, 4), Some(LobbyLinks { relayed: 4, direct: 0, lan: 1 }));
    assert_eq!(LobbyLinks::from_beat(&serde_json::json!({}), 4), None);
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

#[test]
fn build_claim_shape_accepts_the_three_forms_a_host_sends() {
    let sha = "ab01cd23".repeat(8);
    let sig = "ef45ab67".repeat(16);
    assert!(build_claim_shape_ok(&sha, 0, ""));      // unsigned
    assert!(build_claim_shape_ok(&sha, 1, &sig));    // a release key
    assert!(build_claim_shape_ok(&sha, 255, &sig));  // the test key
}

#[test]
fn build_claim_shape_refuses_what_cannot_be_a_claim() {
    let sha = "ab01cd23".repeat(8);
    let sig = "ef45ab67".repeat(16);
    assert!(!build_claim_shape_ok(&sha, 256, &sig));
    assert!(!build_claim_shape_ok(&sha, -1, &sig));
    assert!(!build_claim_shape_ok(&sha, 0, &sig));              // unsigned with a signature
    assert!(!build_claim_shape_ok(&sha, 1, ""));                // signed without one
    assert!(!build_claim_shape_ok(&sha, 1, &sig[..127]));       // short signature
    assert!(!build_claim_shape_ok(&sha.to_uppercase(), 0, "")); // case
    assert!(!build_claim_shape_ok(&sha[..63], 0, ""));          // short hash
}

#[test]
fn a_host_body_keeps_a_well_formed_claim() {
    let sha = "ab01cd23".repeat(8);
    let sig = "ef45ab67".repeat(16);
    let signed = serde_json::json!({"build_sha": sha, "build_key": 7, "build_sig": sig});
    assert_eq!(build_claim_from_body(&signed), (sha.clone(), 7, sig.clone()));
    let unsigned = serde_json::json!({"build_sha": sha, "build_key": 0, "build_sig": ""});
    assert_eq!(build_claim_from_body(&unsigned), (sha.clone(), 0, String::new()));
    let sha_only = serde_json::json!({"build_sha": sha});
    assert_eq!(build_claim_from_body(&sha_only), (sha, 0, String::new()));
}

#[test]
fn a_host_body_drops_a_malformed_claim_and_a_missing_one_is_none() {
    let sha = "ab01cd23".repeat(8);
    let sig = "ef45ab67".repeat(16);
    let none = (String::new(), 0u8, String::new());
    let drops = |b: serde_json::Value| assert_eq!(build_claim_from_body(&b), none);
    drops(serde_json::json!({"build_sha": sha, "build_key": 0, "build_sig": sig}));
    drops(serde_json::json!({"build_sha": &sha[..63], "build_key": 0, "build_sig": ""}));
    drops(serde_json::json!({"build_sha": sha.to_uppercase(), "build_key": 0, "build_sig": ""}));
    drops(serde_json::json!({"name": "x"}));
}

#[test]
fn a_host_body_lists_unless_it_says_not() {
    assert!(!host_listed(&serde_json::json!({"listed": false})));
    assert!(host_listed(&serde_json::json!({"listed": true})));
    assert!(host_listed(&serde_json::json!({})));
}

#[test]
fn a_refused_create_at_the_global_cap_evicts_nothing() {
    // A full map: every lobby from its own address, so no per-address cap is near.
    let mut state = MasterState::new();
    for i in 0..MAX_LOBBIES_GLOBAL {
        let lo = Lobby::new(&format!("10.{}.{}.1", i / 250, i % 250));
        state.lobby_by_public.insert(lo.lobby_id.clone(), lo.session_id.clone());
        state.lobbies.insert(lo.session_id.clone(), lo);
    }
    let before: Vec<String> = state.lobbies.keys().cloned().collect();
    // No identity: refused with a 400 after the version gate passes the empty pair.
    let ctx = HostCtx { now: std::time::Instant::now(), names: &[], min_listed_proto: 0, blocked: &[] };
    let (status, _) = h_host(&mut state, &ctx, "198.51.100.7", &serde_json::json!({}));
    assert_eq!(status, 400);
    assert_eq!(state.lobbies.len(), MAX_LOBBIES_GLOBAL, "a refused create evicted a live lobby");
    assert!(before.iter().all(|sid| state.lobbies.contains_key(sid)));
}

#[test]
fn a_challenge_answers_a_new_nonce_each_time_and_holds_no_table() {
    let (mut state, now) = (MasterState::new(), std::time::Instant::now());
    let mut nonces = std::collections::HashSet::new();
    for n in 0..10 {
        let (status, body) = h_challenge(&mut state, now, &format!("198.51.100.{n}"));
        assert_eq!(status, 200);
        nonces.insert(body["nonce"].as_str().expect("no nonce").to_string());
    }
    assert_eq!(nonces.len(), 10, "two challenges answered one nonce");
    assert_eq!(state.nonces.spent_len(), 0, "a challenge left something in the master");
}

#[test]
fn a_challenge_is_counted_in_its_own_rate_class() {
    let (mut state, now) = (MasterState::new(), std::time::Instant::now());
    for _ in 0..10 {
        assert_eq!(h_challenge(&mut state, now, "198.51.100.9").0, 200);
    }
    let (status, body) = h_challenge(&mut state, now, "198.51.100.9");
    assert_eq!((status, body["error"].as_str()), (429, Some("rate")));
    // The create class is untouched: a create from the same address is judged, not rate-limited.
    let ctx = HostCtx { now, names: &[], min_listed_proto: 0, blocked: &[] };
    assert_eq!(h_host(&mut state, &ctx, "198.51.100.9", &serde_json::json!({})).0, 400);
}

/// Every key of the table taken by a different address, in the challenge class.
fn flood_the_rate_table(state: &mut MasterState) {
    for i in 0..MAX_RATE_KEYS {
        let ip = format!("10.{}.{}.{}", (i >> 16) & 255, (i >> 8) & 255, i & 255);
        assert!(rate_ok(state, &ip, "challenge", super::RL_CHALLENGE.0, super::RL_CHALLENGE.1), "flood key {i}");
    }
    assert_eq!(state.rate.len(), MAX_RATE_KEYS);
}

#[test]
fn a_full_rate_table_refuses_no_address_it_does_not_hold() {
    let (mut state, now) = (MasterState::new(), std::time::Instant::now());
    flood_the_rate_table(&mut state);
    // An honest host from a new address: its challenge, its create and a joiner's join all pass the rate class.
    let (status, body) = h_challenge(&mut state, now, "198.51.100.77");
    assert_eq!((status, body["error"].as_str()), (200, None), "a flood of new addresses refused a challenge");
    let ctx = HostCtx { now, names: &[], min_listed_proto: 0, blocked: &[] };
    assert_eq!(h_host(&mut state, &ctx, "198.51.100.77", &serde_json::json!({})).0, 400, "the create hit the rate class");
    assert_eq!(super::h_join(&mut state, "198.51.100.78", &serde_json::json!({})).0, 400, "the join hit the rate class");
    assert_eq!(state.rate.len(), MAX_RATE_KEYS, "the table grew past its cap");
}

#[test]
fn a_full_rate_table_forgets_the_oldest_flooder_and_keeps_the_newest() {
    let (mut state, now) = (MasterState::new(), std::time::Instant::now());
    flood_the_rate_table(&mut state);
    assert!(h_challenge(&mut state, now, "198.51.100.77").0 == 200);
    assert!(!state.rate.contains("10.0.0.0|challenge"), "the least recently seen key was kept");
    assert!(state.rate.contains("198.51.100.77|challenge"));
    assert_eq!(state.rate.take_forgotten(), 1);
}

#[test]
fn the_list_has_its_own_rate_class() {
    let mut state = MasterState::new();
    for n in 0..RL_BROWSE.1 {
        assert!(h_browse(&mut state, "198.51.100.5").is_some(), "read {n} was refused");
    }
    assert!(h_browse(&mut state, "198.51.100.5").is_none(), "the list was not rate limited");
    assert!(h_browse(&mut state, "198.51.100.6").is_some(), "another address shared the budget");
    // Reading the list spends no other class's budget.
    assert_eq!(super::h_heartbeat(&mut state, "198.51.100.5", &serde_json::json!({})).0, 403);
}

#[test]
fn the_spent_set_fits_the_masters_memory_budget() {
    // 16 MiB of the unit's 128M: the set must never be the reason the master is killed.
    assert!(MAX_SPENT * SPENT_ENTRY_BYTES <= 16 << 20, "MAX_SPENT outgrew its memory budget");
}
