//! Shared helpers for the coop master + signaling binaries.
//!
//! RULE 3: this is VPS infra, it never ships in the mod. The byte-exact spot here, the
//! identity-string shape, is called out inline (the TURN HMAC's is in ice.rs); a mismatch
//! there breaks the signaling rendezvous silently.

use base64::engine::general_purpose::URL_SAFE_NO_PAD as B64URL;
use base64::Engine;
use std::net::Ipv6Addr;
use std::time::{SystemTime, UNIX_EPOCH};

/// stdout line log with an explicit flush (systemd journal picks it up). Mirrors
/// the Python `log()`.
pub fn log(msg: &str) {
    use std::io::Write;
    let mut out = std::io::stdout().lock();
    let _ = writeln!(out, "{msg}");
    let _ = out.flush();
}

/// Read an env var as a String, falling back to `default` when unset/empty-allowed.
pub fn env_str(key: &str, default: &str) -> String {
    std::env::var(key).unwrap_or_else(|_| default.to_string())
}

/// Read an env var as an integer, falling back to `default` on unset/parse-fail.
pub fn env_int(key: &str, default: i64) -> i64 {
    std::env::var(key)
        .ok()
        .and_then(|v| v.trim().parse::<i64>().ok())
        .unwrap_or(default)
}

/// Seconds since the Unix epoch (wall clock). Used ONLY for the TURN credential
/// expiry, which coturn validates against real time — never for rate/last-seen
/// windows (those use a monotonic `Instant`).
pub fn now_unix() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

/// `secrets.token_hex(nbytes)` — `2*nbytes` lowercase hex chars from the OS CSPRNG.
pub fn token_hex(nbytes: usize) -> String {
    let mut buf = vec![0u8; nbytes];
    getrandom::getrandom(&mut buf).expect("OS CSPRNG unavailable");
    let mut s = String::with_capacity(nbytes * 2);
    for b in &buf {
        s.push(char::from_digit((b >> 4) as u32, 16).unwrap());
        s.push(char::from_digit((b & 0xf) as u32, 16).unwrap());
    }
    s
}

/// `secrets.token_urlsafe(nbytes)` — base64url (no padding) of `nbytes` CSPRNG bytes
/// (~1.3*nbytes chars). Used for the host bearer `token` capability.
pub fn token_urlsafe(nbytes: usize) -> String {
    let mut buf = vec![0u8; nbytes];
    getrandom::getrandom(&mut buf).expect("OS CSPRNG unavailable");
    B64URL.encode(&buf)
}

/// Coarsen a client address to the allocation an attacker realistically controls, so
/// abuse controls (rate limits, lobby caps, TURN-cred accounting, connection admission) can't be bypassed by
/// address rotation (audit M2/M3): a full IPv4 address, or an IPv6 /64 (the smallest
/// prefix a host is routably assigned — a single tenant typically owns a whole /64).
pub fn ip_bucket(ip: &str) -> String {
    if ip.contains(':') {
        if let Ok(v6) = ip.parse::<Ipv6Addr>() {
            // A dual-stack listener reports an IPv4 client as ::ffff:a.b.c.d; as a /64 every one
            // of them would share a single bucket.
            if let Some(v4) = v6.to_ipv4_mapped() {
                return v4.to_string();
            }
            let s = v6.segments();
            return format!("{:x}:{:x}:{:x}:{:x}::/64", s[0], s[1], s[2], s[3]);
        }
    }
    ip.to_string()
}

/// `clamp_str(v, maxlen)` — keep only genuinely printable chars (plus ASCII space),
/// clamp to `maxlen` code points. Matches Python `str.isprintable()`'s intent: a
/// host-supplied `name`/`world`/`version` renders in every client's server browser,
/// so it must not carry control chars, whitespace-separators, OR the invisible /
/// bidirectional-format chars (RLO U+202E, zero-width U+200B/U+200C/U+200D, BOM
/// U+FEFF, soft-hyphen, etc.) — those enable lobby-name spoofing/disguise. Rust's
/// `is_control()`+`is_whitespace()` alone MISS the Cf/format + default-ignorable set
/// (security audit 2026-07-16, M3), so we reject those explicitly here.
pub fn clamp_str(s: &str, maxlen: usize) -> String {
    s.chars()
        .filter(|&c| c == ' ' || is_display_safe(c))
        .take(maxlen)
        .collect()
}

/// True for a char that is safe to render in a UI label: not control, not any
/// whitespace (we allow ASCII space separately), and not a bidi-control /
/// zero-width / format / default-ignorable code point. Conservative allow-of-the-
/// rest — the goal is parity with Python `isprintable()` for the categories that
/// matter for spoofing, without pulling a full Unicode-category crate.
fn is_display_safe(c: char) -> bool {
    if c.is_control() || c.is_whitespace() {
        return false;
    }
    // Reject the invisible / format / bidi-control / private-use ranges that Rust's
    // is_control()+is_whitespace() do not cover but Python's isprintable() strips.
    !matches!(c as u32,
        0x00AD |                 // SOFT HYPHEN
        0x200B..=0x200F |        // ZERO WIDTH SPACE/NJ/J, LRM, RLM
        0x202A..=0x202E |        // LRE RLE PDF LRO RLO (bidi overrides)
        0x2060..=0x2064 |        // WORD JOINER / invisible math operators
        0x2066..=0x206F |        // LRI RLI FSI PDI + deprecated format
        0xFE00..=0xFE0F |        // Variation Selectors 1-16
        0xFEFF |                 // ZERO WIDTH NO-BREAK SPACE / BOM
        0xFFF9..=0xFFFB |        // interlinear annotation anchors
        0xE000..=0xF8FF |        // BMP Private Use Area (attacker glyphs)
        0x1D173..=0x1D17A |      // musical format controls
        0xE0000..=0xE01EF |      // tags block + variation-selector supplement
        0xF0000..=0xFFFFD |      // Supplementary Private Use Area-A
        0x100000..=0x10FFFD      // Supplementary Private Use Area-B
    )
}

/// A peer identity as GNS renders a 32-byte `GenericBytes` one: `gen:` + 64
/// lowercase hex. Since b144 a host publishes its durable PUBLIC KEY here and
/// joiners dial exactly this string. The master stores it verbatim and never mints it; this
/// shape check cannot tell whether the sender holds the key (the announce proof in
/// `lobby_proof` does that), but it must not accept a shape that would dial nobody.
/// Lowercase is required, not merely preferred:
/// `[V]` GNS hashes and byte-compares identity strings and its own parser rejects
/// uppercase prefixes for that reason, so `GEN:AB..` would register under one
/// spelling and be dialled under another.
///
/// Lives here rather than in `bin/master.rs` since 2026-08-29 because the
/// signaling relay now needs the SAME predicate for a different reason: this
/// shape means "the identity IS a public key", which is what makes registration
/// provable (A59). Two copies of one predicate is how the two ends drift into
/// disagreeing about which names are keys.
pub fn identity_shape_ok(id: &str) -> bool {
    let Some(hex) = id.strip_prefix("gen:") else {
        return false;
    };
    hex.len() == 64 && hex.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

/// The shape of the build claim a host announces: its binary's SHA-256 (64 lowercase hex), the id of
/// the key that signed it (0 = unsigned, else 1..=255) and the signature (128 lowercase hex, empty
/// only for key 0). Shape only: the master records a claim and verifies nothing.
pub fn build_claim_shape_ok(sha: &str, key: i64, sig: &str) -> bool {
    if hex_to_bytes::<32>(sha).is_none() {
        return false;
    }
    match key {
        0 => sig.is_empty(),
        1..=255 => hex_to_bytes::<64>(sig).is_some(),
        _ => false,
    }
}

/// A host name as the master compares it with the name a lobby proof was signed for: trimmed,
/// lower-case, any login (`name@`) stripped, a trailing `:port` stripped, an IPv6 literal kept with its
/// brackets, one trailing dot stripped. A name with a non-ASCII byte is empty (no master the
/// client can dial has one). The client derives the name it signs from the URL it dialled by the
/// same rule (`server/tests/vectors/master_names.json` holds the shared cases).
pub fn canon_host_name(s: &str) -> String {
    let s = s.trim();
    if !s.is_ascii() {
        return String::new();
    }
    let s = s.rsplit_once('@').map_or(s, |(_, host)| host).to_ascii_lowercase();
    let host = if s.starts_with('[') {
        match s.find(']') {
            Some(end) => s[..=end].to_string(),
            None => s,
        }
    } else {
        match s.rsplit_once(':') {
            Some((host, port)) if !host.contains(':') && !port.is_empty() && port.bytes().all(|b| b.is_ascii_digit()) => {
                host.to_string()
            }
            _ => s,
        }
    };
    match host.strip_suffix('.') {
        Some(bare) => bare.to_string(),
        None => host,
    }
}

/// Decode an EXACTLY-`N`-byte lowercase-hex string. Returns `None` on any wrong
/// length, odd length, or non-hex byte -- there is no partial success, because
/// every caller here is deciding whether to trust a remote peer and a
/// half-decoded key is not a smaller version of a key.
pub fn hex_to_bytes<const N: usize>(s: &str) -> Option<[u8; N]> {
    if s.len() != N * 2 {
        return None;
    }
    let b = s.as_bytes();
    let mut out = [0u8; N];
    for i in 0..N {
        let hi = (b[i * 2] as char).to_digit(16)?;
        let lo = (b[i * 2 + 1] as char).to_digit(16)?;
        if b[i * 2].is_ascii_uppercase() || b[i * 2 + 1].is_ascii_uppercase() {
            return None; // lowercase-only, for the same reason identity_shape_ok is
        }
        out[i] = ((hi << 4) | lo) as u8;
    }
    Some(out)
}

/// Constant-time byte compare, matching Python's `hmac.compare_digest` (which is
/// also length-leaking — an early length mismatch returns fast). Used for the
/// server-side capability tokens (host bearer, signaling shared bearer).
pub fn ct_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut diff = 0u8;
    for (x, y) in a.iter().zip(b.iter()) {
        diff |= x ^ y;
    }
    diff == 0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn token_hex_shape() {
        // secrets.token_hex(8) -> 16 lowercase hex chars.
        let t = token_hex(8);
        assert_eq!(t.len(), 16);
        assert!(t.chars().all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase()));
    }

    #[test]
    fn clamp_str_strips_control_and_clamps() {
        assert_eq!(clamp_str("hello\tworld\n", 64), "helloworld");
        assert_eq!(clamp_str("keep spaces", 64), "keep spaces");
        assert_eq!(clamp_str("abcdef", 3), "abc");
    }

    #[test]
    fn clamp_str_strips_bidi_and_zero_width() {
        // M3 (2026-07-16): must strip bidi overrides / zero-width / BOM / PUA that
        // is_control()+is_whitespace() alone would let through.
        assert_eq!(clamp_str("ab\u{202E}cd", 64), "abcd"); // RLO
        assert_eq!(clamp_str("a\u{200B}b\u{200D}c", 64), "abc"); // ZWSP / ZWJ
        assert_eq!(clamp_str("x\u{FEFF}y", 64), "xy"); // BOM
        assert_eq!(clamp_str("p\u{E000}q", 64), "pq"); // PUA
        assert_eq!(clamp_str("normal Name 123", 64), "normal Name 123"); // real text intact
    }

    #[test]
    fn a_mapped_v4_address_is_its_own_bucket() {
        assert_eq!(ip_bucket("::ffff:203.0.113.7"), "203.0.113.7");
        assert_ne!(ip_bucket("::ffff:203.0.113.7"), ip_bucket("::ffff:198.51.100.1"));
        assert_eq!(ip_bucket("2001:db8:1:2::1"), "2001:db8:1:2::/64");
    }

    #[test]
    fn a_host_name_is_compared_lower_case_without_its_port() {
        assert_eq!(canon_host_name(" Master.Multivoid.DEV "), "master.multivoid.dev");
        assert_eq!(canon_host_name("master.multivoid.dev:10443"), "master.multivoid.dev");
        assert_eq!(canon_host_name("203.0.113.7:10443"), "203.0.113.7");
        assert_eq!(canon_host_name("[2001:DB8::1]:10443"), "[2001:db8::1]");
        assert_eq!(canon_host_name("[2001:db8::1]"), "[2001:db8::1]");
        assert_eq!(canon_host_name("2001:db8::1"), "2001:db8::1", "a bare v6 literal has no port to strip");
        assert_eq!(canon_host_name("host:"), "host:", "an empty port is not stripped");
        assert_eq!(canon_host_name("host:80x"), "host:80x");
        assert_eq!(canon_host_name("alice:pw@master.multivoid.dev:10443"), "master.multivoid.dev");
        assert_eq!(canon_host_name("master.multivoid.dev."), "master.multivoid.dev");
        assert_eq!(canon_host_name("master.multivoid.dev.:10443"), "master.multivoid.dev");
        assert_eq!(canon_host_name("b\u{fc}cher.example:10443"), "", "a non-ASCII host has no name");
        assert_eq!(canon_host_name("user@b\u{fc}cher.example"), "");
    }

    // The case table the client's selftest checks as well (`HostName` in http_client.cpp): a URL as
    // a player writes it, and the name the master compares. The master receives the host part.
    #[test]
    fn the_shared_url_to_name_table_holds() {
        const TABLE: &str = include_str!("../tests/vectors/master_names.json");
        let rows: Vec<serde_json::Value> = serde_json::from_str(TABLE).unwrap();
        assert!(rows.len() >= 10, "the table lost rows");
        for row in rows {
            let url = row["url"].as_str().expect("url");
            let after_scheme = url.split_once("://").map_or(url, |(_, rest)| rest);
            let authority = after_scheme.split('/').next().unwrap();
            assert_eq!(canon_host_name(authority), row["name"].as_str().expect("name"), "{url}");
        }
    }

    #[test]
    fn ct_eq_basic() {
        assert!(ct_eq(b"abc", b"abc"));
        assert!(!ct_eq(b"abc", b"abd"));
        assert!(!ct_eq(b"abc", b"ab"));
    }
}
