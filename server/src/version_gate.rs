//! Which game versions may host: the shape check that remains of the version-pair gate (2026-08-29).
//!
//! The register endpoint used to swallow ANY self-reported version pair -- a
//! field host was seen advertising "b148" when no such build exists, and the
//! lobby list dutifully rendered it (every real client sees an unjoinable
//! impossible-version row: pure pollution, and it reads as "a newer build
//! exists"). The pair is self-reported and host attestation is a separate arc
//! (docs/security/README.md -- peer certificates), so this gate is now deliberately
//! minimal: it refuses SHAPE-invalid game strings and NOTHING ELSE.
//!
//! THE BUILD-NUMBER CEILING IS RETIRED (2026-08-31). `COOP_MAX_BUILD` and
//! `COOP_ALLOWED_BUILDS` are no longer read; see version_gate for why. Any
//! deployment still carrying them in /etc/coop-master.env can leave them there --
//! they are simply ignored, so retiring this needs no coordinated redeploy, which
//! is itself the point.
//! proto==0 (legacy pre-v122 hosts) always passed and still does.

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

/// The running game build a host reads from its own install (the mod's `ue_wrap/core/game_build`):
/// `<exe link stamp, 8 hex>-<pak index hash, 16 hex>`, or `<stamp>-nopak` when its pak could not be read;
/// empty from a host that predates the field. A parsing concern like `game_shape_ok`: the browser compares
/// it byte for byte and names it, so nothing else may reach that field.
pub fn game_build_shape_ok(key: &str) -> bool {
    if key.is_empty() {
        return true;
    }
    let hex = |s: &str| s.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b));
    match key.split_once('-') {
        Some((stamp, rest)) => {
            stamp.len() == 8 && hex(stamp) && (rest == "nopak" || (rest.len() == 16 && hex(rest)))
        }
        None => false,
    }
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
pub fn version_gate(game: &str, proto: i64, game_build: &str) -> Result<(), String> {
    if !game.is_empty() && !game_shape_ok(game) {
        return Err(format!("bad game version '{}'", game));
    }
    if !game_build_shape_ok(game_build) {
        return Err("bad game build key".to_string());
    }
    let _ = proto;
    Ok(())
}

/// The first `kProtocolVersion` whose host signs its announce (the lobby proof): a create from a
/// build at or above it must carry a valid proof, a create from an older build cannot sign and is
/// listed unproved. A fact about the protocol, so a code constant; the maintainer's floor
/// (`COOP_MIN_LISTED_PROTO`) is the config that retires the unproved tier.
///
/// This is the kProtocolVersion of the build that first carries the proof's client half. A server
/// test reads `protocol.h` and fails when this exceeds the tree's own kProtocolVersion, so a value
/// no build can reach (no build is then required to prove) cannot be committed.
pub const MIN_PROVED_PROTO: i64 = 230;

/// A create from this build cannot be listed without a valid proof.
pub fn proof_required(proto: i64) -> bool {
    proto >= MIN_PROVED_PROTO
}

/// A create from this build is below the maintainer's floor (`COOP_MIN_LISTED_PROTO`).
pub fn below_floor(proto: i64, floor: i64) -> bool {
    proto < floor
}

pub fn version_gate_env(game: &str, proto: i64, game_build: &str) -> Result<(), String> {
    // No env left to resolve: the ceiling and the allowlist are retired. Kept as a named
    // seam so the call site still reads as a gate and a future policy has an obvious home.
    version_gate(game, proto, game_build)
}

#[cfg(test)]
mod tests {
    use super::{below_floor, game_build_shape_ok, game_shape_ok, proof_required, version_gate, MIN_PROVED_PROTO};

    #[test]
    fn a_build_that_signs_must_prove_and_the_floor_is_a_plain_bound() {
        assert!(!proof_required(MIN_PROVED_PROTO - 1));
        assert!(proof_required(MIN_PROVED_PROTO));
        assert!(proof_required(65535));
        assert!(!below_floor(0, 0), "the default floor refuses nothing");
        assert!(!below_floor(230, 230));
        assert!(below_floor(229, 230));
    }

    #[test]
    fn the_proof_floor_is_a_protocol_the_tree_has_reached() {
        let header = concat!(env!("CARGO_MANIFEST_DIR"), "/../src/votv-coop/include/coop/net/protocol.h");
        let text = std::fs::read_to_string(header).unwrap_or_else(|e| panic!("cannot read {header}: {e}"));
        let line = text
            .lines()
            .find(|l| l.contains("kProtocolVersion =") && l.contains("uint16_t"))
            .expect("protocol.h has no kProtocolVersion definition");
        let digits: String = line.split('=').nth(1).expect("no value").chars().filter(char::is_ascii_digit).collect();
        let version: i64 = digits.parse().expect("kProtocolVersion is not a number");
        assert!(MIN_PROVED_PROTO <= version, "MIN_PROVED_PROTO {MIN_PROVED_PROTO} is above kProtocolVersion {version}");
    }

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
        assert!(version_gate("0.9.0n", 148, "").is_ok());
        assert!(version_gate("0.9.0n", 9999, "").is_ok());
        assert!(version_gate("0.9.0n", 143, "").is_ok());
        assert!(version_gate("0.9.0n", 133, "").is_ok());
    }

    #[test]
    fn legacy_and_unadvertised_pass() {
        assert!(version_gate("", 0, "").is_ok());        // legacy pre-v122 host
        assert!(version_gate("0.9.0n", 0, "").is_ok());  // no build advertised
    }

    #[test]
    fn game_build_shape() {
        for k in ["", "6a08597d-56065b874f3bfc68", "6aa6f055-0bc42acf66e7daaa", "6a08597d-nopak"] {
            assert!(game_build_shape_ok(k), "{k} should pass");
        }
        for k in ["6a08597d", "6A08597D-56065b874f3bfc68", "6a08597d-56065b874f3bfc6", "6a08597-56065b874f3bfc68",
                  "6a08597d-56065b874f3bfc68x", "6a08597d-NOPAK", "<b>-nopak", "6a08597d_nopak"] {
            assert!(!game_build_shape_ok(k), "{k} should fail");
        }
        assert!(version_gate("0.9.0n", 222, "6a08597d-56065b874f3bfc68").is_ok());
        assert!(version_gate("0.9.0n", 222, "lol").is_err());
    }

    #[test]
    fn gate_still_refuses_bad_shape_with_any_proto() {
        // SHAPE survives: this is a parsing concern (keep arbitrary strings out of a
        // field every browser renders), not a version-policy one.
        assert!(version_gate("h4x0r edition", 133, "").is_err());
        assert!(version_gate("h4x0r edition", 9999, "").is_err());
    }
}
