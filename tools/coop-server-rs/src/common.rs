//! Shared helpers for the coop master + signaling binaries.
//!
//! RULE 3: this is VPS infra, it never ships in the mod. Ported 1:1 from the
//! Python originals (`tools/coop_master_server.py`, `tools/coop_signaling_server.py`)
//! and kept WIRE-COMPATIBLE with them so the old + new services can run in
//! parallel during cutover. The byte-exact spots (the TURN credential HMAC and the
//! identity-string shapes) are called out inline — a mismatch there breaks coturn
//! auth or the signaling rendezvous silently.

use base64::engine::general_purpose::{STANDARD as B64, URL_SAFE_NO_PAD as B64URL};
use base64::Engine;
use hmac::{Hmac, Mac};
use sha1::Sha1;
use std::time::{SystemTime, UNIX_EPOCH};

type HmacSha1 = Hmac<Sha1>;

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

/// A coturn REST time-limited credential (design 7), byte-for-byte identical to the
/// Python `turn_creds()`:
///   username = "<unixExpiry>:<label>"
///   password = base64( HMAC-SHA1( TURN_SECRET, username ) )
/// coturn validates it via `use-auth-secret` / `static-auth-secret=TURN_SECRET`.
/// **Byte-exact spot:** the HMAC digest, the base64 alphabet (STANDARD, with `=`
/// padding), and the `"exp:label"` username format must all match or coturn auth
/// fails. Returns `None` (→ omitted from the JSON, same as the Python empty dict)
/// when TURN is not configured.
/// The base64(HMAC-SHA1(secret, username)) coturn password. Split out so the
/// byte-exact spot is unit-testable against the Python reference independent of the
/// time-based expiry.
pub fn turn_password(turn_secret: &str, username: &str) -> String {
    let mut mac = HmacSha1::new_from_slice(turn_secret.as_bytes()).expect("HMAC accepts any key length");
    mac.update(username.as_bytes());
    B64.encode(mac.finalize().into_bytes())
}

pub fn turn_creds(turn_uri: &str, turn_secret: &str, label: &str, ttl: u64) -> Option<serde_json::Value> {
    if turn_uri.is_empty() || turn_secret.is_empty() {
        return None;
    }
    let exp = now_unix() + ttl;
    let username = format!("{exp}:{label}");
    let password = turn_password(turn_secret, &username);
    let uris = vec![
        format!("{turn_uri}?transport=udp"),
        format!("{turn_uri}?transport=tcp"),
    ];
    Some(serde_json::json!({
        "user": username,
        "pass": password,
        "ttl": ttl,
        "uris": uris,
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn turn_password_matches_python_reference() {
        // Reference produced by the Python turn_creds() HMAC path:
        //   python -c "import hmac,hashlib,base64; u='1700000000:h0011223344556677';
        //   print(base64.b64encode(hmac.new(b'testsecret_abc123', u.encode(),
        //   hashlib.sha1).digest()).decode())"  -> c7pJt+2pR4aVy8LJIi6NtjympwM=
        let pw = turn_password("testsecret_abc123", "1700000000:h0011223344556677");
        assert_eq!(pw, "c7pJt+2pR4aVy8LJIi6NtjympwM=");
    }

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
    fn ct_eq_basic() {
        assert!(ct_eq(b"abc", b"abc"));
        assert!(!ct_eq(b"abc", b"abd"));
        assert!(!ct_eq(b"abc", b"ab"));
    }
}
