//! The lobby announce proof. A lobby's identity IS an Ed25519 public key, so its host signs a
//! challenge the master issued, together with the master's own name, before the row may be
//! marked proved. The decision lives here, apart from the handler's I/O, so known-answer vectors
//! can watch it refuse (the shape of `registration.rs`). The nonces it signs over are minted and
//! spent by `Nonces`, below.

use crate::common::{ct_eq, hex_to_bytes};
use crate::version_gate::proof_required;
use ring::hmac;
use ring::rand::{SecureRandom, SystemRandom};
use std::collections::{HashSet, VecDeque};
use std::time::{Duration, Instant};

// MTA's announce is one unsigned GET (CMasterServerAnnouncer.h:235-267) that every client then
// checks by querying the announced port (Client/core/ServerBrowser/CServerList.cpp:490-595). A P2P
// host is reached only through the relay and cannot be dialled, so the host signs a challenge the master
// issued, the direction of MTA's own server-side challenge route (CHTTPD.cpp:131-170).

// Domain tag of the announce proof. It differs from the signaling registration's
// (`multivoid-signaling-register-v1`) and the peer admission's (`multivoid-peer-admission-v1`) in
// its first bytes, so no choice of nonce can make a blob signed for one the blob of another: all
// three are signed with the peer's one durable key.
//
// The blob is tag || identity || nonce || master_name. The identity is exactly 68 characters and
// the nonce exactly 64, so the master name that follows cannot shift into either. The name is
// what defeats a relay through a rogue master: a host that dialled the rogue signs the rogue's
// name, which no honest master accepts.
pub const ANNOUNCE_TAG: &[u8] = b"multivoid-lobby-announce-v1";

/// The bytes an announce signs: tag || identity || nonce || master_name.
pub fn blob(ident: &str, nonce: &str, master_name: &str) -> Vec<u8> {
    let mut blob = Vec::with_capacity(ANNOUNCE_TAG.len() + ident.len() + nonce.len() + master_name.len());
    blob.extend_from_slice(ANNOUNCE_TAG);
    blob.extend_from_slice(ident.as_bytes());
    blob.extend_from_slice(nonce.as_bytes());
    blob.extend_from_slice(master_name.as_bytes());
    blob
}

/// The announce proof's whole DECISION. `ident` must already have passed `identity_shape_ok`;
/// `nonce` is the 64-hex value this master issued to the caller's address bucket, and
/// `master_name` the name the caller is taken to have dialled (lower-case, port stripped);
/// `proof_hex` is the 128-hex signature the host sent.
pub fn check_lobby_proof(
    ident: &str,
    nonce: &str,
    master_name: &str,
    proof_hex: &str,
) -> Result<(), &'static str> {
    let hex = ident.strip_prefix("gen:").ok_or("not a key identity")?;
    let pubkey = hex_to_bytes::<32>(hex).ok_or("key identity did not decode")?;
    let sig = hex_to_bytes::<64>(proof_hex).ok_or("malformed proof")?;
    ring::signature::UnparsedPublicKey::new(&ring::signature::ED25519, &pubkey)
        .verify(&blob(ident, nonce, master_name), &sig)
        .map_err(|_| "does not hold the key this identity names")
}

// ---- the nonce ----------------------------------------------------------------------------

/// How long, at the least, an issued nonce may be answered. The expiry is carried in whole
/// seconds of the master's own clock, so a nonce lives between `NONCE_LIFE` and one second more.
pub const NONCE_LIFE: Duration = Duration::from_secs(30);
/// The most spent nonces the master remembers, sized to MEMORY, not to the worst inflow: the master
/// runs under `MemoryMax=128M` (`tools/vps_provision.sh`'s unit template), and an entry costs about
/// 150 bytes across the set and its expiry queue, so 65,536 entries are about 10 MB. A set that is
/// full answers `503 busy` to creates and does NOT forget: this is not MTA's flood table, which
/// stops counting past 100 entries (`reference/mtasa-blue/Server/mods/deathmatch/logic/ASE.cpp:183`) because a forgotten
/// counter costs it only a count, whereas a nonce forgotten before its expiry spends twice. No
/// finite size covers every inflow, since the rate table forgets its oldest buckets at its cap, so
/// the residual is said: filling the set takes more than 2,100 proved-shape creates a second for
/// 31 s (a create spends one nonce, remembered `NONCE_LIFE` + 1 s), about 12,700 address buckets
/// each at its own create budget -- a botnet's price, and only creates are refused meanwhile.
pub const MAX_SPENT: usize = 65_536;
/// The bytes one remembered nonce is budgeted at (the set's entry and its queue entry), for the
/// memory pin below.
pub const SPENT_ENTRY_BYTES: usize = 160;

// Domain tag of the nonce's MAC: a nonce is not a signature over anything else this key makes.
const NONCE_TAG: &[u8] = b"multivoid-lobby-nonce-v1";
const MAC_LEN: usize = 16;

/// Why a nonce was not spent.
#[derive(Debug, PartialEq, Eq)]
pub enum NonceFault {
    /// Not minted here for this address bucket (its MAC fails), expired, or already spent.
    Invalid,
    /// The spent set holds `MAX_SPENT` live entries.
    Full,
}

/// The nonces a create signs. A nonce is `expiry (8, big-endian seconds) || random (8) || MAC
/// (16)`, the MAC an HMAC-SHA256 under a secret drawn when the master starts, over the tag, the
/// address bucket it was issued to, the expiry and the random part. Issuing stores nothing, so
/// no number of challenges can fill anything; only a nonce that was SPENT is kept, until its
/// expiry, so it spends once. MTA's announce carries no nonce (CMasterServerAnnouncer.h:235-267), so
/// there is no precedent to follow; the shape is the SYN cookie's: the server answers a token it can
/// re-derive. A restart draws a new secret, which voids every outstanding nonce (a `410`, and
/// the client asks again). The clock is passed in; seconds count from the master's start.
pub struct Nonces {
    rng: SystemRandom,
    key: hmac::Key,
    origin: Instant,
    spent: HashSet<[u8; 32]>,
    /// The spent nonces by expiry, ascending: the sweep pops the expired from the front.
    order: VecDeque<(u64, [u8; 32])>,
}

impl Nonces {
    pub fn new() -> Nonces {
        let rng = SystemRandom::new();
        let mut secret = [0u8; 32];
        rng.fill(&mut secret).expect("OS CSPRNG unavailable");
        Nonces {
            rng,
            key: hmac::Key::new(hmac::HMAC_SHA256, &secret),
            origin: Instant::now(),
            spent: HashSet::new(),
            order: VecDeque::new(),
        }
    }

    fn secs(&self, now: Instant) -> u64 {
        now.saturating_duration_since(self.origin).as_secs()
    }

    fn mac(&self, bucket: &str, expiry_and_random: &[u8]) -> [u8; MAC_LEN] {
        let mut msg = Vec::with_capacity(NONCE_TAG.len() + bucket.len() + expiry_and_random.len());
        msg.extend_from_slice(NONCE_TAG);
        msg.extend_from_slice(bucket.as_bytes());
        msg.extend_from_slice(expiry_and_random);
        let mut out = [0u8; MAC_LEN];
        out.copy_from_slice(&hmac::sign(&self.key, &msg).as_ref()[..MAC_LEN]);
        out
    }

    /// A fresh 64-hex nonce for `bucket`. Stores nothing.
    pub fn issue(&self, bucket: &str, now: Instant) -> String {
        let mut nonce = [0u8; 32];
        nonce[..8].copy_from_slice(&(self.secs(now) + NONCE_LIFE.as_secs()).to_be_bytes());
        self.rng.fill(&mut nonce[8..16]).expect("OS CSPRNG unavailable");
        let mac = self.mac(bucket, &nonce[..16]);
        nonce[16..].copy_from_slice(&mac);
        nonce.iter().map(|b| format!("{b:02x}")).collect()
    }

    /// Spend `nonce` for `bucket`: only a nonce this master minted for this bucket, not yet
    /// expired, never spent before. A try that fails changes nothing, so a caller cannot burn a
    /// nonce it was not issued.
    pub fn take(&mut self, nonce: &[u8; 32], bucket: &str, now: Instant) -> Result<(), NonceFault> {
        let now_s = self.secs(now);
        while let Some(&(expiry, old)) = self.order.front() {
            if expiry >= now_s {
                break;
            }
            self.order.pop_front();
            self.spent.remove(&old);
        }
        let expiry = u64::from_be_bytes(nonce[..8].try_into().expect("8 bytes"));
        if !ct_eq(&self.mac(bucket, &nonce[..16]), &nonce[16..]) || expiry < now_s || self.spent.contains(nonce) {
            return Err(NonceFault::Invalid);
        }
        if self.spent.len() >= MAX_SPENT {
            return Err(NonceFault::Full);
        }
        self.spent.insert(*nonce);
        let at = self.order.partition_point(|&(e, _)| e <= expiry);
        self.order.insert(at, (expiry, *nonce));
        Ok(())
    }

    /// How many spent nonces are remembered.
    pub fn spent_len(&self) -> usize {
        self.spent.len()
    }
}


// ---- the create's verdict -----------------------------------------------------------------

/// Why a create's proof is refused, each with its HTTP answer.
#[derive(Debug, PartialEq, Eq)]
pub enum Refusal {
    /// A build that signs sent no proof.
    ProofRequired,
    /// A proof that is malformed, or does not verify under any name this master answers to.
    BadProof,
    /// The nonce is not one this master minted for this address bucket, or it expired or was
    /// already spent.
    NonceExpired,
    /// The spent set is full.
    Busy,
}

impl Refusal {
    pub fn answer(&self) -> (u16, &'static str) {
        match self {
            Refusal::ProofRequired => (409, "proof-required"),
            Refusal::BadProof => (400, "bad-proof"),
            Refusal::NonceExpired => (410, "nonce-expired"),
            Refusal::Busy => (503, "busy"),
        }
    }
}

/// The verdict on a create body's `nonce` and `proof`: `Ok(true)` for a proved row, `Ok(false)`
/// for an unproved one (a build below `MIN_PROVED_PROTO` that sent no proof), else the refusal.
/// A proof that is present must verify whatever the build. The nonce is spent as soon as a
/// well-formed proof presents it, whether or not the proof then verifies. The proof is tried
/// against each name the master answers to: the body names none, the signature does.
pub fn judge(
    nonces: &mut Nonces,
    names: &[String],
    bucket: &str,
    now: Instant,
    body: &serde_json::Value,
    identity: &str,
    proto: i64,
) -> Result<bool, Refusal> {
    let proof = match body.get("proof") {
        None | Some(serde_json::Value::Null) => None,
        Some(serde_json::Value::String(s)) => Some(s.as_str()),
        Some(_) => return Err(Refusal::BadProof),
    };
    let Some(proof) = proof else {
        return if proof_required(proto) { Err(Refusal::ProofRequired) } else { Ok(false) };
    };
    let nonce = body.get("nonce").and_then(serde_json::Value::as_str).unwrap_or("");
    let (Some(nonce_bytes), Some(_)) = (hex_to_bytes::<32>(nonce), hex_to_bytes::<64>(proof)) else {
        return Err(Refusal::BadProof);
    };
    match nonces.take(&nonce_bytes, bucket, now) {
        Ok(()) => {}
        Err(NonceFault::Invalid) => return Err(Refusal::NonceExpired),
        Err(NonceFault::Full) => return Err(Refusal::Busy),
    }
    if names.iter().any(|name| check_lobby_proof(identity, nonce, name, proof).is_ok()) {
        Ok(true)
    } else {
        Err(Refusal::BadProof)
    }
}

#[cfg(test)]
mod tests {
    use super::{
        blob, check_lobby_proof, judge, NonceFault, Nonces, Refusal, ANNOUNCE_TAG, MAX_SPENT, NONCE_LIFE,
    };
    use crate::common::{hex_to_bytes, identity_shape_ok};
    use ring::rand::SystemRandom;
    use ring::signature::{Ed25519KeyPair, KeyPair};
    use std::collections::HashSet;
    use std::time::{Duration, Instant};

    // The vector the client's selftest reads as well: one fixed seed, its identity, a nonce, a
    // master name, the blob and the signature an independent Ed25519 implementation produced.
    const VECTOR: &str = include_str!("../tests/vectors/lobby_proof.json");

    const N1: &str = "1111111111111111111111111111111111111111111111111111111111111111";
    const N2: &str = "2222222222222222222222222222222222222222222222222222222222222222";
    const NAME: &str = "master.multivoid.dev";

    struct Peer {
        ident: String,
        kp: Ed25519KeyPair,
    }

    fn ident_of(kp: &Ed25519KeyPair) -> String {
        let mut ident = String::from("gen:");
        for b in kp.public_key().as_ref() {
            ident.push_str(&format!("{b:02x}"));
        }
        ident
    }

    /// A peer built from a REAL generated keypair, so the positive arm takes the path a live
    /// client does.
    fn peer() -> Peer {
        let pkcs8 = Ed25519KeyPair::generate_pkcs8(&SystemRandom::new()).unwrap();
        let kp = Ed25519KeyPair::from_pkcs8(pkcs8.as_ref()).unwrap();
        Peer { ident: ident_of(&kp), kp }
    }

    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|b| format!("{b:02x}")).collect()
    }

    /// Sign `as_ident || nonce || name` under the announce tag and render the wire proof.
    fn proof(p: &Peer, as_ident: &str, nonce: &str, name: &str) -> String {
        hex(p.kp.sign(&blob(as_ident, nonce, name)).as_ref())
    }

    #[test]
    fn the_shared_known_answer_vector_verifies_and_reproduces() {
        let v: serde_json::Value = serde_json::from_str(VECTOR).unwrap();
        let field = |k: &str| v[k].as_str().unwrap_or_else(|| panic!("vector has no {k}")).to_string();
        assert_eq!(field("tag").as_bytes(), ANNOUNCE_TAG);
        let (ident, nonce, name, sig) = (field("identity"), field("nonce"), field("master_name"), field("signature"));
        assert!(identity_shape_ok(&ident));
        assert_eq!(hex(&blob(&ident, &nonce, &name)), field("blob_hex"), "the blob layout moved");
        assert!(check_lobby_proof(&ident, &nonce, &name, &sig).is_ok(), "the vector's own proof was refused");
        // The seed behind it, signed here by ring, gives the same bytes: Ed25519 is deterministic.
        let seed: Vec<u8> = (0..32).map(|i| u8::from_str_radix(&field("seed")[i * 2..i * 2 + 2], 16).unwrap()).collect();
        let kp = Ed25519KeyPair::from_seed_unchecked(&seed).unwrap();
        assert_eq!(ident_of(&kp), ident);
        assert_eq!(hex(kp.sign(&blob(&ident, &nonce, &name)).as_ref()), sig);
        // Each input of the blob is load-bearing: the same proof under any other one is refused.
        assert!(check_lobby_proof(&ident, &nonce, "other.example", &sig).is_err());
        assert!(check_lobby_proof(&ident, N1, &name, &sig).is_err());
    }

    #[test]
    fn a_real_holder_proves() {
        let p = peer();
        assert!(identity_shape_ok(&p.ident), "the generated identity is not the shape the master gates on");
        assert!(check_lobby_proof(&p.ident, N1, NAME, &proof(&p, &p.ident, N1, NAME)).is_ok());
    }

    #[test]
    fn the_ways_to_not_hold_the_key_are_all_refused() {
        let victim = peer();
        let attacker = peer();
        assert_ne!(victim.ident, attacker.ident, "two peers minted one identity -- every arm proves nothing");

        // 1. SQUAT: the attacker signs a blob naming the VICTIM with its own key and lists the
        //    victim's identity.
        assert!(check_lobby_proof(&victim.ident, N1, NAME, &proof(&attacker, &victim.ident, N1, NAME)).is_err());
        // 2. REPLAY: a proof recorded for another nonce.
        assert!(check_lobby_proof(&victim.ident, N2, NAME, &proof(&victim, &victim.ident, N1, NAME)).is_err());
        // 3. ANOTHER MASTER: a proof minted for a rogue master's name verifies nowhere else.
        assert!(check_lobby_proof(&victim.ident, N1, NAME, &proof(&victim, &victim.ident, N1, "rogue.example")).is_err());
        // 4. ANOTHER KEY: the victim's valid proof offered for the attacker's identity.
        assert!(check_lobby_proof(&attacker.ident, N1, NAME, &proof(&victim, &victim.ident, N1, NAME)).is_err());
        // 5. NO PROOF, and the registration's wire form, which is not this one.
        assert!(check_lobby_proof(&victim.ident, N1, NAME, "").is_err());
        let good = proof(&victim, &victim.ident, N1, NAME);
        assert!(check_lobby_proof(&victim.ident, N1, NAME, &format!("auth {good}")).is_err());
        // 6. MALFORMED: short, long, uppercase (the alphabet is lowercase-only like the name it proves).
        assert!(check_lobby_proof(&victim.ident, N1, NAME, &good[..good.len() - 2]).is_err());
        assert!(check_lobby_proof(&victim.ident, N1, NAME, &format!("{good}00")).is_err());
        assert!(check_lobby_proof(&victim.ident, N1, NAME, &good.to_uppercase()).is_err());
        // 7. NOT A KEY IDENTITY.
        assert!(check_lobby_proof("h0123456789abcdef", N1, NAME, &good).is_err());
    }

    #[test]
    fn one_flipped_bit_is_refused() {
        // The arm that fails if verification is ever reduced to a length check.
        let p = peer();
        let mut line = proof(&p, &p.ident, N1, NAME).into_bytes();
        let last = line.len() - 1;
        line[last] = if line[last] == b'0' { b'1' } else { b'0' };
        assert!(check_lobby_proof(&p.ident, N1, NAME, &String::from_utf8(line).unwrap()).is_err());
    }

    #[test]
    fn the_tag_differs_from_the_other_two_in_its_first_bytes() {
        for other in [&b"multivoid-signaling-register-v1"[..], &b"multivoid-peer-admission-v1"[..]] {
            let at = ANNOUNCE_TAG.iter().zip(other).position(|(a, b)| a != b).expect("tags share a prefix");
            assert!(at <= 12, "the announce tag and {:?} share {at} leading bytes", String::from_utf8_lossy(other));
        }
    }

    const A: &str = "198.51.100.1";
    const B: &str = "198.51.100.2";
    // Past the life by a margin the whole-second expiry cannot straddle.
    const DEAD: Duration = Duration::from_secs(NONCE_LIFE.as_secs() + 2);

    fn raw(nonce: &str) -> [u8; 32] {
        hex_to_bytes::<32>(nonce).expect("a nonce is 64 lower-case hex")
    }

    /// `nonce` with the hex digit at `at` changed to another digit.
    fn flip(nonce: &str, at: usize) -> String {
        let mut b = nonce.as_bytes().to_vec();
        b[at] = if b[at] == b'0' { b'1' } else { b'0' };
        String::from_utf8(b).unwrap()
    }

    #[test]
    fn a_nonce_is_64_hex_carries_its_expiry_and_is_spent_once() {
        let (mut n, t0) = (Nonces::new(), Instant::now());
        let nonce = n.issue(A, t0);
        let bytes = raw(&nonce);
        let expiry = u64::from_be_bytes(bytes[..8].try_into().unwrap());
        assert_eq!(expiry, t0.saturating_duration_since(n.origin).as_secs() + NONCE_LIFE.as_secs());
        assert_eq!(n.take(&bytes, A, t0), Ok(()));
        assert_eq!(n.take(&bytes, A, t0), Err(NonceFault::Invalid), "a nonce was spent twice");
        assert_eq!(n.spent_len(), 1);
    }

    #[test]
    fn issuing_stores_nothing_and_never_repeats() {
        let (n, t0) = (Nonces::new(), Instant::now());
        let all: HashSet<String> = (0..2000).map(|_| n.issue(A, t0)).collect();
        assert_eq!(all.len(), 2000, "two challenges answered one nonce");
        assert_eq!(n.spent_len(), 0, "issuing left something behind");
    }

    #[test]
    fn a_nonce_lives_thirty_seconds_and_not_much_longer() {
        let (mut n, t0) = (Nonces::new(), Instant::now());
        let (early, late) = (n.issue(A, t0), n.issue(A, t0));
        assert_eq!(n.take(&raw(&early), A, t0 + NONCE_LIFE), Ok(()), "a nonce died before its thirty seconds");
        assert_eq!(n.take(&raw(&late), A, t0 + DEAD), Err(NonceFault::Invalid), "an expired nonce was spent");
        assert_eq!(n.spent_len(), 0, "the sweep kept an expired nonce");
    }

    #[test]
    fn another_buckets_nonce_is_refused_and_its_owner_keeps_it() {
        let (mut n, t0) = (Nonces::new(), Instant::now());
        let nonce = raw(&n.issue(A, t0));
        assert_eq!(n.take(&nonce, B, t0), Err(NonceFault::Invalid));
        assert_eq!(n.spent_len(), 0, "a stranger's try burnt the owner's nonce");
        assert_eq!(n.take(&nonce, A, t0), Ok(()));
    }

    #[test]
    fn a_nonce_with_any_flipped_digit_is_refused() {
        let (mut n, t0) = (Nonces::new(), Instant::now());
        let nonce = n.issue(A, t0);
        // The expiry, the random part and the MAC: each part is covered by the MAC.
        for at in [0, 15, 16, 31, 32, 47, 63] {
            assert_eq!(n.take(&raw(&flip(&nonce, at)), A, t0), Err(NonceFault::Invalid), "digit {at}");
        }
        assert_eq!(n.take(&raw(&nonce), A, t0), Ok(()), "a refused try burnt the real nonce");
    }

    #[test]
    fn a_master_with_another_secret_does_not_accept_the_nonce() {
        let (mut one, mut two, t0) = (Nonces::new(), Nonces::new(), Instant::now());
        let nonce = raw(&one.issue(A, t0));
        assert_eq!(two.take(&nonce, A, t0), Err(NonceFault::Invalid));
        assert_eq!(one.take(&nonce, A, t0), Ok(()));
    }

    #[test]
    fn a_spent_nonce_is_forgotten_at_its_expiry_and_in_expiry_order() {
        let (mut n, t0) = (Nonces::new(), Instant::now());
        // Spent out of issue order: the later-expiring one first.
        let later = raw(&n.issue(A, t0 + Duration::from_secs(10)));
        let sooner = raw(&n.issue(A, t0));
        let t = t0 + Duration::from_secs(12);
        assert_eq!((n.take(&later, A, t), n.take(&sooner, A, t)), (Ok(()), Ok(())));
        assert_eq!(n.order.iter().map(|e| e.0).collect::<Vec<_>>(), {
            let mut v: Vec<u64> = n.order.iter().map(|e| e.0).collect();
            v.sort_unstable();
            v
        });
        // Past the first's expiry only the first goes; past both, only the newest is left.
        let fresh = raw(&n.issue(A, t0 + Duration::from_secs(35)));
        assert_eq!(n.take(&fresh, A, t0 + Duration::from_secs(35)), Ok(()));
        assert_eq!(n.spent_len(), 2, "the sweep dropped a live nonce or kept a dead one");
        let last = raw(&n.issue(A, t0 + Duration::from_secs(100)));
        assert_eq!(n.take(&last, A, t0 + Duration::from_secs(100)), Ok(()));
        assert_eq!(n.spent_len(), 1);
        assert_eq!(n.order.len(), 1);
    }

    #[test]
    fn a_full_spent_set_answers_busy_until_the_sweep_frees_it() {
        let (mut n, t0) = (Nonces::new(), Instant::now());
        for _ in 0..MAX_SPENT {
            assert_eq!(n.take(&raw(&n.issue(A, t0)), A, t0), Ok(()));
        }
        let next = raw(&n.issue(A, t0));
        assert_eq!(n.take(&next, A, t0), Err(NonceFault::Full));
        assert_eq!(n.spent_len(), MAX_SPENT);
        let after = t0 + DEAD;
        let fresh = raw(&n.issue(A, after));
        assert_eq!(n.take(&fresh, A, after), Ok(()), "the sweep did not free a full set");
        assert_eq!(n.spent_len(), 1);
    }

    #[test]
    fn judge_answers_each_nonce_fault_with_its_status() {
        let (mut n, t0, p) = (Nonces::new(), Instant::now(), peer());
        let names = vec![NAME.to_string()];
        let body = |nonce: &str| serde_json::json!({"nonce": nonce, "proof": proof(&p, &p.ident, nonce, NAME)});
        let nonce = n.issue(A, t0);
        let go = |n: &mut Nonces, bucket: &str, b: &serde_json::Value| judge(n, &names, bucket, t0, b, &p.ident, 230);
        assert_eq!(go(&mut n, B, &body(&nonce)), Err(Refusal::NonceExpired));
        assert_eq!(go(&mut n, A, &body(&nonce)), Ok(true));
        assert_eq!(go(&mut n, A, &body(&nonce)), Err(Refusal::NonceExpired), "a replay was judged");
        assert_eq!(Refusal::NonceExpired.answer(), (410, "nonce-expired"));
        assert_eq!(Refusal::Busy.answer(), (503, "busy"));
    }
}
