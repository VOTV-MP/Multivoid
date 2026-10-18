//! The master's own environment: the ports and the secrets it refuses to start without.
//! Read once, at first use, and never mutated -- so every module reads one snapshot.

use crate::common::{canon_host_name, env_int, env_str, identity_shape_ok, log};
use std::sync::LazyLock;

pub struct Config {
    pub port: u16,
    pub turn_secret: String,
    pub signaling_token: String,
    pub signaling_url: String,
    pub stun_uri: String,
    pub turn_uri: String,
    /// The names a lobby proof may have been signed for: `COOP_DOMAIN` first, then each of
    /// `COOP_MASTER_NAME_ALIASES`. `COOP_DOMAIN` is required, so a master that started holds one.
    pub public_names: Vec<String>,
    /// `COOP_MIN_LISTED_PROTO`: a create below it is refused. 0 (the default) refuses nothing.
    pub min_listed_proto: i64,
    /// `COOP_BLOCKED_IDENTITIES`: key identities (`gen:<64 hex>`) the operator keeps off the list.
    pub blocked_identities: Vec<String>,
}

/// Whether a (canonical) name points back at the machine that dials it: `localhost`, a 0.0.0.0 or
/// 127.x literal (the short forms `127.1` and `127.0.1` too, which the system resolver reads as
/// loopback), the IPv6 loopback and its IPv4-mapped form. Such a name is a dev master's, never a
/// production one's.
pub fn is_loopback_name(name: &str) -> bool {
    if name == "localhost" || name.ends_with(".localhost") || name == "0.0.0.0" {
        return true;
    }
    let inner = name.strip_prefix('[').and_then(|n| n.strip_suffix(']')).unwrap_or(name);
    if inner == "::1" {
        return true;
    }
    let v4 = inner.strip_prefix("::ffff:").unwrap_or(inner);
    let octets: Vec<&str> = v4.split('.').collect();
    (2..=4).contains(&octets.len())
        && octets.iter().all(|o| !o.is_empty() && o.len() <= 3 && o.bytes().all(|b| b.is_ascii_digit()))
        && octets[0].parse::<u16>() == Ok(127)
}

/// The names a master answers to: `domain` (canonicalised, kept whatever it is) then the comma
/// list `aliases`, each trimmed and lower-cased. An alias that is empty is skipped, a loopback
/// one is refused and named in the second list (the caller logs it), a repeat is dropped.
pub fn parse_public_names(domain: &str, aliases: &str) -> (Vec<String>, Vec<String>) {
    let mut names = Vec::new();
    let mut refused = Vec::new();
    let domain = canon_host_name(domain);
    if !domain.is_empty() {
        names.push(domain);
    }
    for alias in aliases.split(',').map(canon_host_name).filter(|a| !a.is_empty()) {
        if is_loopback_name(&alias) {
            refused.push(alias);
        } else if !names.contains(&alias) {
            names.push(alias);
        }
    }
    (names, refused)
}

/// Why a written name is not a bare host name, or None. The name a client signs is the host of the
/// URL it dialled, so a URL, a path, a login, a bracketed literal or a ':' left after the optional
/// port would match no client for good while the master ran and answered `/healthz`: a production
/// master's name is a DNS name or an IPv4 literal. `with_port` lets one trailing ':digits' through
/// (an alias is often copied from a `host:port` entry; it is stripped when names are compared).
fn name_fault(written: &str, with_port: bool) -> Option<&'static str> {
    let s = written.trim();
    if s.contains("://") || s.contains(['/', '@', '[', ']']) || s.chars().any(char::is_whitespace) {
        return Some("is not a bare host name (no scheme, path, login, brackets or spaces)");
    }
    if !s.is_ascii() {
        return Some("is not a bare host name (plain ASCII only)");
    }
    let host = match s.rsplit_once(':') {
        Some((h, p)) if with_port && !p.is_empty() && p.bytes().all(|b| b.is_ascii_digit()) => h,
        _ => s,
    };
    if host.contains(':') {
        return Some(if with_port {
            "is not a bare host name (no IPv6 literal; a port is digits only)"
        } else {
            "is not a bare host name (no ':' at all: no port, no IPv6 literal)"
        });
    }
    if host.strip_suffix('.').unwrap_or(host).is_empty() {
        return Some("is not a bare host name (empty)");
    }
    None
}

/// What is wrong with the master's settings, one line each; empty when it may start. `domain` is
/// `COOP_DOMAIN` as written and is required (aliases are additional names, never a substitute);
/// `aliases` is `COOP_MASTER_NAME_ALIASES` as written, every non-empty entry held to the same rules
/// as the domain bar an allowed ':port'. `floor` is `COOP_MIN_LISTED_PROTO` as written ("" when
/// unset): a value that is not a decimal number would otherwise read as 0 and silently lift it.
pub fn config_faults(domain: &str, aliases: &str, floor: &str) -> Vec<String> {
    let mut faults = Vec::new();
    let d = domain.trim();
    if d.is_empty() {
        faults.push("COOP_DOMAIN not set -- a lobby proof names its master and this one has no name".to_string());
    } else if let Some(why) = name_fault(d, false) {
        faults.push(format!("COOP_DOMAIN '{d}' {why}"));
    }
    for a in aliases.split(',').map(str::trim).filter(|a| !a.is_empty()) {
        if let Some(why) = name_fault(a, true) {
            faults.push(format!("COOP_MASTER_NAME_ALIASES entry '{a}' {why}"));
        }
    }
    let f = floor.trim();
    if !f.is_empty() && !(f.bytes().all(|b| b.is_ascii_digit()) && f.parse::<i64>().is_ok()) {
        faults.push(format!("COOP_MIN_LISTED_PROTO '{f}' is not a decimal number"));
    }
    faults
}

/// The identities of `COOP_BLOCKED_IDENTITIES` as written: a comma list, each entry trimmed, an empty
/// one skipped, a repeat dropped. An entry that is not `gen:<64 lower-case hex>` is named in the
/// second list (the caller refuses to start on it: a block that silently matched nothing is worse
/// than none).
pub fn parse_blocked_identities(spec: &str) -> (Vec<String>, Vec<String>) {
    let mut blocked = Vec::new();
    let mut invalid = Vec::new();
    for entry in spec.split(',').map(str::trim).filter(|e| !e.is_empty()) {
        if !identity_shape_ok(entry) {
            invalid.push(entry.to_string());
        } else if !blocked.iter().any(|b| b == entry) {
            blocked.push(entry.to_string());
        }
    }
    (blocked, invalid)
}

/// What is wrong with `COOP_BLOCKED_IDENTITIES` as written, one line each; empty when it may start.
pub fn blocked_faults(spec: &str) -> Vec<String> {
    parse_blocked_identities(spec)
        .1
        .iter()
        .map(|e| format!("COOP_BLOCKED_IDENTITIES entry '{e}' is not a key identity (gen: and 64 lower-case hex)"))
        .collect()
}

impl Config {
    fn from_env() -> Config {
        let (public_names, refused) =
            parse_public_names(&env_str("COOP_DOMAIN", ""), &env_str("COOP_MASTER_NAME_ALIASES", ""));
        for alias in &refused {
            log(&format!("WARN: COOP_MASTER_NAME_ALIASES entry '{alias}' refused (a loopback name)"));
        }
        Config {
            port: env_int("COOP_MASTER_PORT", 10001) as u16,
            turn_secret: env_str("COOP_TURN_SECRET", ""),
            signaling_token: env_str("COOP_SIGNALING_TOKEN", ""),
            signaling_url: env_str("COOP_SIGNALING_URL", ""),
            stun_uri: env_str("COOP_STUN_URI", ""),
            turn_uri: env_str("COOP_TURN_URI", ""),
            public_names,
            min_listed_proto: env_int("COOP_MIN_LISTED_PROTO", 0).max(0),
            blocked_identities: parse_blocked_identities(&env_str("COOP_BLOCKED_IDENTITIES", "")).0,
        }
    }
}

pub static CFG: LazyLock<Config> = LazyLock::new(Config::from_env);

#[cfg(test)]
mod tests {
    use super::{blocked_faults, config_faults, is_loopback_name, parse_blocked_identities, parse_public_names};

    #[test]
    fn loopback_names_are_recognised() {
        for n in [
            "localhost", "dev.localhost", "127.0.0.1", "127.1.2.3", "0.0.0.0", "::1", "[::1]", "127.1", "127.0.1",
            "::ffff:127.0.0.1", "[::ffff:127.0.0.1]",
        ] {
            assert!(is_loopback_name(n), "{n} should be loopback");
        }
        for n in [
            "master.multivoid.dev", "203.0.113.7", "128.0.0.1", "1270.0.0.1", "127", "128.1", "::ffff:128.0.0.1",
            "localhost.example",
        ] {
            assert!(!is_loopback_name(n), "{n} should not be loopback");
        }
    }

    #[test]
    fn the_domain_comes_first_and_aliases_are_trimmed_and_lower_cased() {
        let (names, refused) =
            parse_public_names(" Master.Multivoid.dev ", " 203.0.113.7:10443 , Dev.Example ,, master.multivoid.dev");
        assert_eq!(names, vec!["master.multivoid.dev", "203.0.113.7", "dev.example"]);
        assert!(refused.is_empty());
    }

    #[test]
    fn a_loopback_alias_is_refused_and_named() {
        let (names, refused) = parse_public_names("master.multivoid.dev", "localhost,127.0.0.1:1,203.0.113.7");
        assert_eq!(names, vec!["master.multivoid.dev", "203.0.113.7"]);
        assert_eq!(refused, vec!["localhost", "127.0.0.1"]);
    }

    #[test]
    fn no_domain_and_no_alias_is_no_name() {
        assert!(parse_public_names("", "").0.is_empty());
        assert!(parse_public_names("  ", " , ").0.is_empty());
    }

    #[test]
    fn a_domain_must_be_a_bare_host_and_a_floor_a_decimal_number() {
        for d in ["master.multivoid.dev", " Master.Multivoid.dev ", "master.multivoid.dev.", "203.0.113.7"] {
            assert!(config_faults(d, "", "").is_empty(), "'{d}' was refused");
        }
        for d in [
            "https://master.multivoid.dev",
            "master.multivoid.dev/v1",
            "user@master.multivoid.dev",
            "master.multivoid.dev:10443",
            "master.multivoid.dev:abc",
            "[::1]:10443",
            "[2001:db8::1]",
            "2001:db8::1",
            "master multivoid.dev",
            "b\u{fc}cher.example",
            ".",
        ] {
            assert_eq!(config_faults(d, "", "").len(), 1, "'{d}' was accepted");
        }
        for f in ["", " ", "0", "230", " 230 "] {
            assert!(config_faults("a.example", "", f).is_empty(), "floor '{f}' was refused");
        }
        for f in ["abc", "23x", "-1", "2.5", "99999999999999999999"] {
            assert_eq!(config_faults("a.example", "", f).len(), 1, "floor '{f}' was accepted");
        }
        assert_eq!(config_faults("https://a.example", "", "x").len(), 2, "both faults are named");
    }

    #[test]
    fn the_domain_is_required_and_aliases_do_not_stand_in_for_it() {
        for d in ["", "  "] {
            assert_eq!(config_faults(d, "", "").len(), 1, "'{d}' was accepted");
            assert_eq!(config_faults(d, "203.0.113.7", "").len(), 1, "an alias stood in for '{d}'");
        }
    }

    #[test]
    fn an_alias_is_held_to_the_domains_rules_and_may_carry_a_port() {
        for a in ["", " , ,", "203.0.113.7:10443", " Dev.Example , 203.0.113.7:1 ", "dev.example.", "localhost,127.0.0.1:1"] {
            assert!(config_faults("a.example", a, "").is_empty(), "aliases '{a}' were refused");
        }
        for a in [
            "https://dev.example",
            "dev.example/v1",
            "user@dev.example",
            "dev.example:abc",
            "dev.example:",
            "dev.example:1:2",
            "[::1]",
            "[2001:db8::1]:10443",
            "2001:db8::1",
            "dev example",
            "b\u{fc}cher.example",
        ] {
            assert_eq!(config_faults("a.example", a, "").len(), 1, "alias '{a}' was accepted");
        }
        let two = config_faults("a.example", "ok.example,https://x.example,y.example:abc", "");
        assert_eq!(two.len(), 2, "each bad entry is named: {two:?}");
        assert!(two[0].contains("https://x.example") && two[1].contains("y.example:abc"));
    }

    #[test]
    fn the_block_list_is_a_trimmed_comma_list_of_key_identities() {
        let (a, b) = (format!("gen:{}", "ab".repeat(32)), format!("gen:{}", "01".repeat(32)));
        let (blocked, invalid) = parse_blocked_identities(&format!(" {a} ,, {b},{a}"));
        assert_eq!(blocked, vec![a, b]);
        assert!(invalid.is_empty());
        assert!(parse_blocked_identities("").0.is_empty() && parse_blocked_identities(" , ").0.is_empty());
        assert!(blocked_faults("").is_empty());
    }

    #[test]
    fn a_block_entry_that_is_not_a_key_identity_refuses_the_start() {
        let good = format!("gen:{}", "ab".repeat(32));
        for bad in [
            "h0123456789abcdef".to_string(),
            "ab".repeat(32),
            format!("gen:{}", "AB".repeat(32)),
            format!("gen:{}", "ab".repeat(31)),
            format!("{good}0"),
        ] {
            let faults = blocked_faults(&format!("{good},{bad}"));
            assert_eq!(faults.len(), 1, "'{bad}' was accepted: {faults:?}");
            assert!(faults[0].contains(&bad));
        }
        assert_eq!(blocked_faults("x,y").len(), 2, "each bad entry is named");
    }
}
