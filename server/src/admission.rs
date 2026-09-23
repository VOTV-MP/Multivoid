//! Connection admission, shared by both binaries: a pool with a total cap and a per-address cap. A
//! slot is taken at ACCEPT -- before a byte is read or a TLS handshake starts -- and is given back
//! when it drops, which also happens on an unwind: a panicking connection task cannot leak one.
//!
//! Why at accept: a cap checked inside the request handler counts only the connections that have
//! already finished their TLS handshake. A peer that opened sockets and never sent a ClientHello
//! was counted by nothing and bounded only by the process's file limit. The per-address cap is what
//! stops ONE source from filling the whole pool; the address is coarsened by `ip_bucket` (a full
//! IPv4 address, an IPv6 /64), like every other per-source control in this crate.

use crate::common::{ip_bucket, log};
use std::collections::HashMap;
use std::sync::Mutex;
use std::time::{Duration, Instant};

/// A refusal is logged at most this often per pool. Every refused socket used to cost a
/// synchronous log write on the runtime's worker -- one worker on a 1-vCPU box -- at whatever rate
/// the flood connected, so the line now carries how many refusals it stands for.
const REFUSAL_LOG_EVERY: Duration = Duration::from_secs(1);

pub struct Pool {
    name: &'static str,
    max_total: usize,
    max_per_addr: u32,
    // One lock over both counts, so a slot is taken or refused against a consistent pair. An entry
    // exists only while it holds a slot, so the map never outgrows `max_total`.
    held: Mutex<Held>,
    refusals: Mutex<Refusals>,
}

#[derive(Default)]
struct Held {
    total: usize,
    per_addr: HashMap<String, u32>,
}

#[derive(Default)]
struct Refusals {
    last_logged: Option<Instant>,
    unlogged: u64,
}

/// One admitted connection. Dropping it gives the slot back.
pub struct Slot<'a> {
    pool: &'a Pool,
    key: String,
}

impl Pool {
    pub fn new(name: &'static str, max_total: usize, max_per_addr: u32) -> Pool {
        Pool {
            name,
            max_total,
            max_per_addr,
            held: Mutex::new(Held::default()),
            refusals: Mutex::new(Refusals::default()),
        }
    }

    /// Admit a connection from `ip`, or say which cap refused it.
    pub fn admit(&self, ip: &str) -> Result<Slot<'_>, String> {
        let key = ip_bucket(ip);
        let mut held = self.held.lock().unwrap_or_else(|e| e.into_inner());
        if held.total >= self.max_total {
            return Err(format!("{} cap ({})", self.name, self.max_total));
        }
        let n = held.per_addr.get(&key).copied().unwrap_or(0);
        if n >= self.max_per_addr {
            return Err(format!("per-address {} cap ({})", self.name, self.max_per_addr));
        }
        held.per_addr.insert(key.clone(), n + 1);
        held.total += 1;
        Ok(Slot { pool: self, key })
    }

    /// `admit`, with a refusal logged at most once per `REFUSAL_LOG_EVERY`; the line that is
    /// written counts the refusals since the last one.
    pub fn admit_or_log(&self, ip: &str) -> Option<Slot<'_>> {
        match self.admit(ip) {
            Ok(slot) => Some(slot),
            Err(why) => {
                if let Some(n) = self.note_refusal(Instant::now()) {
                    let tail = if n > 1 { format!(" ({n} refusals since the last line)") } else { String::new() };
                    log(&format!("[{ip}] refused: {why}{tail}"));
                }
                None
            }
        }
    }

    /// Count one refusal; `Some(n)` when a line is due, `n` being the refusals it reports.
    fn note_refusal(&self, now: Instant) -> Option<u64> {
        let mut r = self.refusals.lock().unwrap_or_else(|e| e.into_inner());
        r.unlogged += 1;
        let due = r.last_logged.map_or(true, |t| now.duration_since(t) >= REFUSAL_LOG_EVERY);
        if !due {
            return None;
        }
        r.last_logged = Some(now);
        Some(std::mem::take(&mut r.unlogged))
    }

    /// Slots held in the whole pool.
    pub fn held(&self) -> usize {
        self.held.lock().unwrap_or_else(|e| e.into_inner()).total
    }

    /// Slots held by `ip`'s address bucket.
    pub fn held_by(&self, ip: &str) -> u32 {
        let held = self.held.lock().unwrap_or_else(|e| e.into_inner());
        held.per_addr.get(&ip_bucket(ip)).copied().unwrap_or(0)
    }

    #[cfg(test)]
    fn addresses(&self) -> usize {
        self.held.lock().unwrap().per_addr.len()
    }
}

impl Drop for Slot<'_> {
    fn drop(&mut self) {
        let mut held = self.pool.held.lock().unwrap_or_else(|e| e.into_inner());
        held.total = held.total.saturating_sub(1);
        if let Some(n) = held.per_addr.get_mut(&self.key) {
            *n = n.saturating_sub(1);
            if *n == 0 {
                held.per_addr.remove(&self.key);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{Pool, REFUSAL_LOG_EVERY};
    use std::time::Instant;

    #[test]
    fn one_address_cannot_take_the_whole_pool() {
        let pool = Pool::new("test", 10, 2);
        let _a = pool.admit("203.0.113.7").ok().unwrap();
        let _b = pool.admit("203.0.113.7").ok().unwrap();
        assert!(pool.admit("203.0.113.7").is_err(), "a third slot for one address was admitted");
        assert!(pool.admit("198.51.100.1").is_ok(), "another address was refused by the first one's cap");
    }

    #[test]
    fn the_total_cap_holds_across_addresses() {
        let pool = Pool::new("test", 3, 2);
        let _held: Vec<_> =
            ["192.0.2.1", "192.0.2.2", "192.0.2.3"].iter().map(|ip| pool.admit(ip).ok().unwrap()).collect();
        assert!(pool.admit("192.0.2.4").is_err(), "a slot past the total cap was admitted");
    }

    #[test]
    fn a_dropped_slot_is_given_back_and_leaves_no_entry() {
        let pool = Pool::new("test", 10, 1);
        let slot = pool.admit("192.0.2.9").ok().unwrap();
        assert_eq!((pool.held(), pool.held_by("192.0.2.9"), pool.addresses()), (1, 1, 1));
        drop(slot);
        assert_eq!(
            (pool.held(), pool.held_by("192.0.2.9"), pool.addresses()),
            (0, 0, 0),
            "the slot or its address entry outlived the drop"
        );
        assert!(pool.admit("192.0.2.9").is_ok(), "the given-back slot was not admissible again");
    }

    #[test]
    fn a_v6_prefix_is_one_address() {
        // Rotating inside one /64 is free for its owner, so the cap counts the prefix.
        let pool = Pool::new("test", 10, 1);
        let _a = pool.admit("2001:db8:1:2::1").ok().unwrap();
        assert!(pool.admit("2001:db8:1:2::ffff").is_err(), "a second address in the same /64 was admitted");
        assert!(pool.admit("2001:db8:1:3::1").is_ok(), "a different /64 was refused");
    }

    #[test]
    fn refusals_are_logged_at_most_once_a_period_and_counted() {
        let pool = Pool::new("test", 1, 1);
        let t0 = Instant::now();
        assert_eq!(pool.note_refusal(t0), Some(1), "the first refusal was not logged");
        assert_eq!(pool.note_refusal(t0), None, "a second refusal inside the period was logged");
        assert_eq!(pool.note_refusal(t0), None);
        assert_eq!(pool.note_refusal(t0 + REFUSAL_LOG_EVERY), Some(3), "the line did not count the unlogged ones");
    }
}
