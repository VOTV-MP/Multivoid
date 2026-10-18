//! The master's per-(address, class) sliding windows, in a table that never refuses a key.
//!
//! The table holds at most `cap` keys. When a key it does not hold arrives at the cap, the key
//! seen LEAST RECENTLY is forgotten to make room, so a flood of new addresses fills the table
//! with the flooders and costs the oldest of them their window; it never refuses a request from
//! an address the table does not hold. The precedent bounds its mitigation too: once its own
//! table holds 100 flooding entries MTA stops counting queries per address and answers the rest
//! (`reference/mtasa-blue/Server/mods/deathmatch/logic/ASE.cpp:183`); this table keeps counting
//! every key it holds and forgets the oldest, the stricter shape, so the mitigation cannot become
//! the sink.
//!
//! Recency is a queue of (stamp, key) pushed on every request; a key's live entry is the one
//! whose stamp equals the stamp its slot holds, and the older ones are dead. Eviction pops the
//! front until it finds a live entry, so each pushed entry is popped once (O(1) amortised, no
//! scan per request), and the queue is rebuilt when it outgrows twice the table, which keeps
//! its memory bounded by the same amortisation. Entries that stopped counting (older than every
//! window) are the least recently seen, so they leave first; no separate expiry scan is needed.

use std::collections::{HashMap, VecDeque};
use std::sync::Arc;
use std::time::{Duration, Instant};

/// Dead entries the queue may hold beyond twice the table before it is rebuilt.
const SLACK: usize = 1024;

struct Slot {
    name: Arc<str>,
    stamps: Vec<Instant>,
    /// The stamp of this key's live entry in the recency queue.
    seq: u64,
}

pub struct RateTable {
    cap: usize,
    slots: HashMap<Arc<str>, Slot>,
    recency: VecDeque<(u64, Arc<str>)>,
    next_seq: u64,
    forgotten: u64,
}

impl RateTable {
    pub fn new(cap: usize) -> RateTable {
        RateTable { cap: cap.max(1), slots: HashMap::new(), recency: VecDeque::new(), next_seq: 0, forgotten: 0 }
    }

    pub fn len(&self) -> usize {
        self.slots.len()
    }

    pub fn is_empty(&self) -> bool {
        self.slots.is_empty()
    }

    pub fn contains(&self, key: &str) -> bool {
        self.slots.contains_key(key)
    }

    /// Count one event of `key` against `limit` events per `window`: true when it is within the
    /// limit. A request that is refused still counts as the key being seen.
    pub fn hit(&mut self, key: &str, now: Instant, window: Duration, limit: usize) -> bool {
        if !self.slots.contains_key(key) {
            self.make_room();
            let name: Arc<str> = Arc::from(key);
            self.slots.insert(Arc::clone(&name), Slot { name, stamps: Vec::new(), seq: 0 });
        }
        let seq = self.next_seq;
        self.next_seq += 1;
        let slot = self.slots.get_mut(key).expect("the slot was just ensured");
        slot.seq = seq;
        self.recency.push_back((seq, Arc::clone(&slot.name)));
        slot.stamps.retain(|ts| now.duration_since(*ts) < window);
        let within = slot.stamps.len() < limit;
        if within {
            slot.stamps.push(now);
        }
        if self.recency.len() > 2 * self.slots.len() + SLACK {
            self.rebuild_recency();
        }
        within
    }

    /// Forget every key whose last counted event is older than `max_window` (the longest window
    /// of any class). The master's sweeper calls this on a timer.
    pub fn prune(&mut self, now: Instant, max_window: Duration) {
        self.slots.retain(|_, s| s.stamps.last().is_some_and(|last| now.duration_since(*last) < max_window));
        self.rebuild_recency();
    }

    /// How many keys were forgotten for room since the last call (the sweeper logs it).
    pub fn take_forgotten(&mut self) -> u64 {
        std::mem::take(&mut self.forgotten)
    }

    fn live(&self, seq: u64, name: &Arc<str>) -> bool {
        self.slots.get(&**name).is_some_and(|s| s.seq == seq)
    }

    /// Make room for one more key: forget the least recently seen until the table is under its cap.
    fn make_room(&mut self) {
        while self.slots.len() >= self.cap {
            let Some((seq, name)) = self.recency.pop_front() else { break };
            if self.live(seq, &name) {
                self.slots.remove(&*name);
                self.forgotten += 1;
            }
        }
    }

    fn rebuild_recency(&mut self) {
        let slots = &self.slots;
        self.recency.retain(|(seq, name)| slots.get(&**name).is_some_and(|s| s.seq == *seq));
    }
}

#[cfg(test)]
mod tests {
    use super::{RateTable, SLACK};
    use std::time::{Duration, Instant};

    const W: Duration = Duration::from_secs(60);

    fn hit(t: &mut RateTable, key: &str, now: Instant) -> bool {
        t.hit(key, now, W, 10)
    }

    #[test]
    fn a_window_counts_its_limit_and_then_refuses() {
        let (mut t, now) = (RateTable::new(8), Instant::now());
        for _ in 0..10 {
            assert!(hit(&mut t, "a|create", now));
        }
        assert!(!hit(&mut t, "a|create", now), "the eleventh event inside the window passed");
        assert!(hit(&mut t, "a|create", now + W), "the window did not slide");
    }

    #[test]
    fn a_new_key_at_the_cap_is_admitted_and_the_least_recently_seen_forgotten() {
        let (mut t, now) = (RateTable::new(3), Instant::now());
        for k in ["a", "b", "c"] {
            assert!(hit(&mut t, k, now));
        }
        assert!(hit(&mut t, "a", now), "a known key was refused");
        assert!(hit(&mut t, "d", now), "a new key was refused at the cap");
        assert_eq!(t.len(), 3);
        assert!(!t.contains("b"), "the least recently seen key was kept");
        assert!(t.contains("a") && t.contains("c") && t.contains("d"));
        assert_eq!(t.take_forgotten(), 1);
        assert_eq!(t.take_forgotten(), 0);
    }

    #[test]
    fn a_key_that_stopped_counting_is_forgotten_before_a_live_one() {
        let (mut t, t0) = (RateTable::new(3), Instant::now());
        assert!(hit(&mut t, "old", t0));
        assert!(hit(&mut t, "mid", t0 + Duration::from_secs(50)));
        assert!(hit(&mut t, "new", t0 + Duration::from_secs(100)));
        assert!(hit(&mut t, "fresh", t0 + Duration::from_secs(100)));
        assert!(!t.contains("old") && t.contains("mid") && t.contains("new"));
    }

    #[test]
    fn a_flood_of_new_keys_never_refuses_one() {
        let (mut t, now) = (RateTable::new(100), Instant::now());
        for i in 0..5000 {
            assert!(hit(&mut t, &format!("flood{i}|challenge"), now), "flood key {i} was refused");
        }
        assert_eq!(t.len(), 100);
        assert!(hit(&mut t, "honest|create", now));
    }

    #[test]
    fn a_flooder_that_stays_active_keeps_its_limit() {
        let (mut t, now) = (RateTable::new(3), Instant::now());
        for _ in 0..10 {
            assert!(hit(&mut t, "loud", now));
        }
        for i in 0..50 {
            assert!(!hit(&mut t, "loud", now), "the flooder passed after {i} refusals");
            assert!(hit(&mut t, &format!("quiet{i}"), now));
        }
        assert!(t.contains("loud"), "a refused request did not count as seen");
    }

    #[test]
    fn the_recency_queue_stays_bounded_under_many_touches_of_few_keys() {
        let (mut t, now) = (RateTable::new(50), Instant::now());
        for i in 0..20_000 {
            hit(&mut t, if i % 2 == 0 { "x" } else { "y" }, now);
        }
        assert!(t.recency.len() <= 2 * t.len() + SLACK + 1, "the queue held {} entries", t.recency.len());
    }

    #[test]
    fn prune_forgets_the_idle_and_keeps_the_queue_and_table_one_to_one() {
        let (mut t, t0) = (RateTable::new(10), Instant::now());
        hit(&mut t, "idle", t0);
        hit(&mut t, "busy", t0 + Duration::from_secs(100));
        hit(&mut t, "busy", t0 + Duration::from_secs(101));
        t.prune(t0 + Duration::from_secs(110), W);
        assert!(!t.contains("idle") && t.contains("busy"));
        assert_eq!(t.recency.len(), t.len());
    }
}
