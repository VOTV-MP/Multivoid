//! Connection admission, shared by both binaries: a pool with a total cap and a per-address cap. A
//! slot is taken at ACCEPT -- before a byte is read or a TLS handshake starts -- and is given back
//! when it drops, which also happens on an unwind: a panicking connection task cannot leak one.
//!
//! Why at accept: a cap checked inside the request handler counts only the connections that have
//! already finished their TLS handshake. A peer that opened sockets and never sent a ClientHello
//! was counted by nothing and bounded only by the process's file limit. The per-address cap is what
//! stops ONE source from filling the whole pool; the address is coarsened by `ip_bucket` (a full
//! IPv4 address, an IPv6 /64), like every other per-source control in this crate.

use crate::common::ip_bucket;
use std::collections::HashMap;
use std::sync::Mutex;

pub struct Pool {
    name: &'static str,
    max_total: usize,
    max_per_addr: u32,
    // One lock over both counts, so a slot is taken or refused against a consistent pair. An entry
    // exists only while it holds a slot, so the map never outgrows `max_total`.
    held: Mutex<Held>,
}

#[derive(Default)]
struct Held {
    total: usize,
    per_addr: HashMap<String, u32>,
}

/// One admitted connection. Dropping it gives the slot back.
pub struct Slot<'a> {
    pool: &'a Pool,
    key: String,
}

impl Pool {
    pub fn new(name: &'static str, max_total: usize, max_per_addr: u32) -> Pool {
        Pool { name, max_total, max_per_addr, held: Mutex::new(Held::default()) }
    }

    /// Admit a connection from `ip`, or say which cap refused it (for the caller's log line).
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

    #[cfg(test)]
    fn counts(&self, ip: &str) -> (usize, u32, usize) {
        let held = self.held.lock().unwrap();
        (held.total, held.per_addr.get(&ip_bucket(ip)).copied().unwrap_or(0), held.per_addr.len())
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
    use super::Pool;

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
        assert_eq!(pool.counts("192.0.2.9"), (1, 1, 1));
        drop(slot);
        assert_eq!(pool.counts("192.0.2.9"), (0, 0, 0), "the slot or its address entry outlived the drop");
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
}
