//! Production async P2P signaling server for VOTV coop.
//!
//! Originally a Rust port of `tools/coop_signaling_server.py`, kept wire-compatible
//! with it so both could run during the cutover. That Python relay was RETIRED
//! 2026-08-29 (RULE 2): the cutover was long finished and its only remaining job
//! was to be the local fixture four rig scenarios launched -- so the rig was
//! proving changes against a copy that never ships. `tools/mp.py:signaling_exe()`
//! now builds and launches THIS binary instead. RULE 3: VPS infra, never ships in
//! the mod.
//!
//! Wire protocol (line-oriented, '\n'-terminated, identities are space-free):
//!   greeting (first line) : <token> <identity>      -- identity is `gen:<64 hex>`
//!   server -> client      : nonce <64 hex>
//!   client -> server      : auth <128 hex>
//!   message  (subsequent) : <dest-identity> <hexpayload>
//!   forwarded to dest as  : <sender-identity> <hexpayload>
//!
//! REGISTRATION PROOF (security A59, 2026-08-29). A registered identity IS an
//! Ed25519 public key -- b144 made a peer's durable key its rendezvous name -- so
//! its holder is asked to prove it. Until this landed, the greeting's only
//! credential was `COOP_SIGNALING_TOKEN`, a static bearer handed to every mod
//! user; combined with evict-on-duplicate that meant anyone who had ever played
//! with you could take your permanent name and deny you rendezvous FOREVER, in
//! every future session.
//!
//! THE RULE HAS NO CONDITIONAL, and that is deliberate. An earlier cut proved
//! only identities SHAPED like a key, which would have left the `h<16hex>` /
//! `c<16hex>` names the master minted for b<=133 registering unproved beside
//! proved ones. That cohort is retired instead (user decision, 2026-08-29), so
//! the invariant is total: nothing is in the routing map that its holder did not
//! sign for. An invariant with an exception is one every reader has to re-derive.
//!
//! Registration happens only AFTER the proof, which is also what makes the added
//! server->client line safe: `relay_line` can only reach an identity that is
//! already in `CLIENTS`, so during the challenge window nothing can be delivered
//! to this connection at all, and no peer can forge a line that looks like the
//! challenge. The whole pre-registration phase shares ONE `GREETING_TIMEOUT`
//! budget, so an unauthenticated peer cannot hold a `MAX_PENDING` slot longer
//! than it could before.
//!
//! WHAT THIS DOES NOT CLOSE, and it is why A59 is MITIGATED rather than CLOSED:
//! the mod's signaling leg is still PLAINTEXT (`signaling_client.cpp` is raw
//! WinSock; TLS there is Tier B arc 3, on hold). On a plaintext channel a
//! challenge-response is relayable -- an on-path attacker proxies our nonce,
//! harvests a live `auth`, and registers as the victim. That is the same theorem
//! as P1, one layer down, and the escalation is real: a single moment on-path
//! converts into PERMANENT denial, since the stolen registration can be held from
//! anywhere afterwards. So arc 3 is no longer "may be unnecessary" -- it is the
//! completion of this fix, and the `[listener=...]` field on the registration
//! line below is the positive evidence its flip will need.
//!
//! Concurrency model: ONE task owns each connection's full socket (split into read +
//! write halves) and `tokio::select!`s over (a) the next inbound line and (b) a
//! bounded relay channel that OTHER peers push into. Because a single task owns both
//! halves, task exit closes the whole socket; and dropping a peer's channel Sender
//! (on evict-on-duplicate-identity) closes the channel -> the old task's select sees
//! `recv()==None` -> it stops -> its socket closes IMMEDIATELY. This is cleaner than
//! the Python's `prev.close()` (which relied on the OS to unwedge the old reader).
//!
//! The relay is best-effort: a bounded channel (drop on full) replaces the Python's
//! 5s drain timeout — a slow/stalled destination can never head-of-line block a
//! sender, and memory per destination is bounded by the channel capacity.

use coop_server::common::{ct_eq, env_int, env_str, hex_to_bytes, identity_shape_ok, log, token_hex};
use coop_server::tls;
use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{LazyLock, Mutex};
use std::time::Duration;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt, ReadHalf};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::mpsc;
use tokio::time::{timeout_at, Instant};

const MAX_LINE: usize = 64 * 1024;
const GREETING_TIMEOUT: Duration = Duration::from_secs(15);
const MAX_PENDING: usize = 128;
const MAX_AUTHED: usize = 512;
const MAX_AUTHED_PER_IP: u32 = 32;
// 80, not 64 (raised 2026-08-29): since b144 a peer registers under its DURABLE
// identity, which GNS renders as `gen:` + 64 hex = 68 chars. At 64 the greeting
// was refused outright, which reads as "P2P is down" rather than as a length cap.
const MAX_IDENTITY: usize = 80;
// Bounded relay backlog per destination. Worst-case per-dest heap =
// RELAY_QUEUE * MAX_RELAY_PAYLOAD. Sizing (security audit 2026-07-16, S-1): the
// prior 1024-deep queue of up-to-MAX_LINE (64 KiB) items let ONE token-holder pin
// ~64 MiB per stalled destination and OOM the shared box. Signaling payloads
// (SDP/ICE trickle) are a few KB, so we cap the relayed frame at MAX_RELAY_PAYLOAD
// and keep the queue shallow: 64 * 8 KiB = 512 KiB per dest, hard bound.
const RELAY_QUEUE: usize = 64;
const MAX_RELAY_PAYLOAD: usize = 8 * 1024; // drop a relayed line longer than this
// Domain tag for the registration proof. Deliberately DIFFERENT from the admission
// exchange's `multivoid-peer-admission-v1`: both are signed with the peer's one
// durable key, and the separation is what stops a hostile relay -- which chooses
// the nonce -- from steering a client into producing a signature that would also
// be a valid admission proof. Since the two tags differ in their first bytes, no
// choice of nonce can make one blob equal the other.
//
// The blob is tag ‖ identity ‖ nonce with no separators or lengths, which is
// unambiguous by construction rather than by convention: `identity_shape_ok`
// accepts exactly 68 characters and the nonce is exactly 64, so no two distinct
// (identity, nonce) pairs can produce the same bytes.
const REGISTER_TAG: &[u8] = b"multivoid-signaling-register-v1";

static TOKEN: LazyLock<String> = LazyLock::new(|| env_str("COOP_SIGNALING_TOKEN", ""));
static CLIENTS: LazyLock<Mutex<HashMap<String, (u64, mpsc::Sender<Vec<u8>>)>>> =
    LazyLock::new(|| Mutex::new(HashMap::new()));
static AUTHED_PER_IP: LazyLock<Mutex<HashMap<String, u32>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
static PENDING: AtomicUsize = AtomicUsize::new(0);
static AUTHED: AtomicUsize = AtomicUsize::new(0);
static CONN_SEQ: AtomicU64 = AtomicU64::new(1);

/// Bounded, cancel-safe line reader over the connection's read half. On `select!`
/// cancellation the in-flight `read` consumes nothing (tokio `read` is cancel-safe),
/// and already-buffered bytes persist in `buf` for the next call.
struct LineReader<S> {
    rh: ReadHalf<S>,
    buf: Vec<u8>,
    max: usize,
}

enum LineErr {
    TooLong,
    Closed,
}

impl<S: AsyncRead + Unpin> LineReader<S> {
    fn new(rh: ReadHalf<S>, max: usize) -> LineReader<S> {
        LineReader { rh, buf: Vec::with_capacity(256), max }
    }

    /// Next line INCLUDING its trailing '\n' (matches the Python `readuntil(b"\n")`).
    async fn next_line(&mut self) -> Result<Vec<u8>, LineErr> {
        loop {
            if let Some(i) = self.buf.iter().position(|b| *b == b'\n') {
                let line: Vec<u8> = self.buf.drain(..=i).collect();
                return Ok(line);
            }
            if self.buf.len() > self.max {
                return Err(LineErr::TooLong);
            }
            let mut tmp = [0u8; 4096];
            let n = self.rh.read(&mut tmp).await.map_err(|_| LineErr::Closed)?;
            if n == 0 {
                return Err(LineErr::Closed);
            }
            self.buf.extend_from_slice(&tmp[..n]);
        }
    }
}

struct Reg {
    identity: String,
    conn_id: u64,
}

/// The registration proof's whole DECISION, split from its I/O so it can be
/// exercised with known-answer vectors. A gate whose only exercise is a live
/// drill is one nobody has ever watched REFUSE, and that is indistinguishable
/// from `Ok(())` -- so the negatives below are the test, not the positive.
///
/// `ident` must already have passed `identity_shape_ok`; `nonce` is the 64-hex
/// value this connection was just sent, which is what makes a recorded proof
/// useless against the next one.
fn check_registration_proof(ident: &str, nonce: &str, auth_line: &str) -> Result<(), &'static str> {
    let hex = ident.strip_prefix("gen:").ok_or("not a key identity")?;
    let pubkey = hex_to_bytes::<32>(hex).ok_or("key identity did not decode")?;
    let sig_hex = auth_line.trim().strip_prefix("auth ").ok_or("expected an auth line")?;
    let sig = hex_to_bytes::<64>(sig_hex).ok_or("malformed proof")?;
    let mut blob = Vec::with_capacity(REGISTER_TAG.len() + ident.len() + nonce.len());
    blob.extend_from_slice(REGISTER_TAG);
    blob.extend_from_slice(ident.as_bytes());
    blob.extend_from_slice(nonce.as_bytes());
    ring::signature::UnparsedPublicKey::new(&ring::signature::ED25519, &pubkey)
        .verify(&blob, &sig)
        .map_err(|_| "does not hold the key this identity names")
}

async fn handle<S: AsyncRead + AsyncWrite + Unpin>(stream: S, ip: String, listener: &'static str) {
    // Admission into the bounded PRE-AUTH pool (an anonymous flood can fill only this
    // small pool; each conn is dropped after GREETING_TIMEOUT).
    if PENDING.fetch_add(1, Ordering::Relaxed) >= MAX_PENDING {
        PENDING.fetch_sub(1, Ordering::Relaxed);
        log(&format!("[{ip}] refused: pre-auth pool full"));
        return;
    }

    let mut reg: Option<Reg> = None;
    serve(stream, &ip, listener, &mut reg).await;

    // ---- cleanup (exactly mirrors the Python finally block) ----
    match reg {
        None => {
            PENDING.fetch_sub(1, Ordering::Relaxed);
        }
        Some(reg) => {
            AUTHED.fetch_sub(1, Ordering::Relaxed);
            {
                let mut per = AUTHED_PER_IP.lock().unwrap_or_else(|e| e.into_inner());
                if let Some(c) = per.get_mut(&ip) {
                    *c = c.saturating_sub(1);
                    if *c == 0 {
                        per.remove(&ip);
                    }
                }
            }
            let mut cl = CLIENTS.lock().unwrap_or_else(|e| e.into_inner());
            if cl.get(&reg.identity).map(|(id, _)| *id) == Some(reg.conn_id) {
                cl.remove(&reg.identity);
                log(&format!("[{}@{}] disconnected", reg.identity, ip));
            }
        }
    }
}

/// Run one connection: greet, promote out of the pre-auth pool on success (filling
/// `reg_out`), then the relay loop. Returns when the connection ends.
async fn serve<S: AsyncRead + AsyncWrite + Unpin>(
    stream: S,
    ip: &str,
    listener: &str,
    reg_out: &mut Option<Reg>,
) {
    // NOTE: SO_KEEPALIVE is set on the raw TcpStream at accept time (before any
    // TLS wrap), since socket options belong to the socket, not to the
    // record layer -- see serve_plain/serve_tls.
    let (rh, mut wh) = tokio::io::split(stream);
    let mut lr = LineReader::new(rh, MAX_LINE);

    // ONE budget for the WHOLE pre-registration phase (greeting + proof), not one
    // per line: a peer that answers each line just inside a per-line timeout could
    // otherwise hold a bounded PENDING slot for as long as it kept adding lines.
    let deadline = Instant::now() + GREETING_TIMEOUT;

    // --- greeting: "<token> <identity>", short timeout (anti-slowloris) ---
    let line = match timeout_at(deadline, lr.next_line()).await {
        Ok(Ok(l)) => l,
        _ => {
            log(&format!("[{ip}] no/oversized greeting -- dropped"));
            return;
        }
    };
    let greeting = String::from_utf8_lossy(&line);
    let parts: Vec<&str> = greeting.trim().split(' ').collect();
    if parts.len() != 2 {
        log(&format!("[{ip}] malformed greeting -- dropped"));
        return;
    }
    let (token, ident) = (parts[0], parts[1]);
    if !ct_eq(token.as_bytes(), TOKEN.as_bytes()) {
        log(&format!("[{ip}] bad token -- dropped"));
        return;
    }
    if ident.is_empty() || ident.contains(' ') || ident.len() > MAX_IDENTITY {
        log(&format!("[{ip}] empty/spaced/oversized identity -- dropped"));
        return;
    }

    // --- registration proof (A59): every name here is a KEY, so prove you hold it.
    //
    // The rule has NO conditional, and that is the point. It could have been
    // "prove it IF the identity looks like a key", which would have left the
    // `h<16hex>`/`c<16hex>` names the master minted for b<=133 registering
    // unproved beside proved ones. The b<=133 cohort is retired instead (user
    // decision, 2026-08-29), so the invariant is total: nothing is in the routing
    // map that its holder did not sign for. An invariant with an exception is a
    // thing you have to re-derive at every call site.
    if !identity_shape_ok(ident) {
        log(&format!(
            "[{ip}] REFUSED: identity '{ident}' is not a key (`gen:<64 hex>`). A \
             pre-b145 peer lands here -- that cohort is retired; it must update."
        ));
        return;
    }
    // Runs BEFORE the AUTHED reservation on purpose: a connection that never
    // answers must occupy the small bounded PENDING pool, which exists for exactly
    // this shape of flood, rather than one of the 512 authed slots.
    {
        let nonce = token_hex(32);
        if wh.write_all(format!("nonce {nonce}\n").as_bytes()).await.is_err() {
            return;
        }
        let auth_line = match timeout_at(deadline, lr.next_line()).await {
            Ok(Ok(l)) => l,
            _ => {
                log(&format!("[{ident}@{ip}] REFUSED: no proof before the deadline"));
                return;
            }
        };
        if let Err(why) = check_registration_proof(ident, &nonce, &String::from_utf8_lossy(&auth_line))
        {
            log(&format!("[{ident}@{ip}] REFUSED: {why}"));
            return;
        }
    }

    // Auth OK -> reserve an authed slot ATOMICALLY (audit LOW-1: the prior
    // check-then-increment let concurrent connections over-admit past the caps on
    // the multi-thread runtime). Take the global slot first and roll back if over;
    // then check+take the per-IP slot under one lock.
    if AUTHED.fetch_add(1, Ordering::Relaxed) >= MAX_AUTHED {
        AUTHED.fetch_sub(1, Ordering::Relaxed);
        log(&format!("[{ident}@{ip}] refused: authed cap"));
        return; // pending stays +1 -> handle() cleanup decrements it (None branch)
    }
    {
        let mut per = AUTHED_PER_IP.lock().unwrap_or_else(|e| e.into_inner());
        let cnt = per.entry(ip.to_string()).or_insert(0);
        if *cnt >= MAX_AUTHED_PER_IP {
            drop(per);
            AUTHED.fetch_sub(1, Ordering::Relaxed); // release the global slot we took
            log(&format!("[{ident}@{ip}] refused: per-ip authed cap"));
            return;
        }
        *cnt += 1;
    }
    // Promotion committed: this connection now owns one AUTHED + one per-IP slot,
    // released exactly once by the handle() cleanup Some-branch.
    PENDING.fetch_sub(1, Ordering::Relaxed);

    let identity = ident.to_string();
    let conn_id = CONN_SEQ.fetch_add(1, Ordering::Relaxed);
    let (tx, mut rx) = mpsc::channel::<Vec<u8>>(RELAY_QUEUE);

    // Register (evict-on-duplicate-identity, token-gated). Overwriting the map entry
    // drops the previous Sender -> the previous connection's select sees recv()==None
    // -> it stops and closes its socket.
    {
        let mut cl = CLIENTS.lock().unwrap_or_else(|e| e.into_inner());
        if let Some((prev_id, _)) = cl.get(&identity) {
            if *prev_id != conn_id {
                log(&format!("[{identity}@{ip}] replaced previous connection"));
            }
        }
        cl.insert(identity.clone(), (conn_id, tx));
    }
    *reg_out = Some(Reg { identity: identity.clone(), conn_id });
    // `key proved` is the drill's positive needle AND the operator's answer to
    // "is the A59 gate armed on this deployment". `listener` is the evidence base
    // for the arc-3 flip: the proof is RELAYABLE on a plaintext leg (an on-path
    // attacker proxies the nonce and harvests a live `auth`), so A59 is MITIGATED
    // rather than closed until the client speaks TLS here -- and the day it does,
    // this line is what proves no install still arrives in the clear.
    log(&format!("[{identity}@{ip}] registered (key proved) [listener={listener}]"));

    // --- relay loop: no idle timeout (authed peers stay connected) ---
    loop {
        tokio::select! {
            inbound = lr.next_line() => {
                match inbound {
                    Ok(line) => relay_line(&identity, &line),
                    Err(_) => break, // eof / oversized line / read error
                }
            }
            outbound = rx.recv() => {
                match outbound {
                    Some(bytes) => {
                        if wh.write_all(&bytes).await.is_err() {
                            break;
                        }
                    }
                    None => break, // channel closed => evicted (or shutting down)
                }
            }
        }
    }
}

/// Parse one inbound line ("<dest> <hexpayload>\n") and forward it to the destination
/// as "<sender-identity> <hexpayload>\n" (best-effort; drop if dest absent or its
/// relay queue is full).
fn relay_line(sender: &str, line: &[u8]) {
    // S-1 (audit 2026-07-16): drop over-length lines BEFORE building/queuing the
    // relayed frame, so a single sender can't pin RELAY_QUEUE * MAX_LINE per dest.
    // Real SDP/ICE frames are a few KB; MAX_RELAY_PAYLOAD is generous headroom.
    if line.len() > MAX_RELAY_PAYLOAD {
        return;
    }
    let text = String::from_utf8_lossy(line);
    let sp = match text.find(' ') {
        Some(i) if i > 0 => i,
        _ => return, // no dest, or leading space -> drop (matches `sp <= 0: continue`)
    };
    let dest = text[..sp].trim();
    let payload = &text[sp + 1..]; // keeps the trailing '\n'
    if dest.is_empty() {
        return;
    }
    let msg = format!("{sender} {payload}").into_bytes();
    let cl = CLIENTS.lock().unwrap_or_else(|e| e.into_inner());
    if let Some((_, dest_tx)) = cl.get(dest) {
        // try_send is non-blocking: drop on Full (slow dest) or Closed (gone). This is
        // the bounded-channel form of the Python 5s-drain best-effort relay.
        let _ = dest_tx.try_send(msg);
    }
}

/// Enable SO_KEEPALIVE (dead authed peers are reaped without an app idle timeout).
/// Portable via socket2's SockRef; failure is non-fatal.
fn set_keepalive(stream: &TcpStream) {
    let sock = socket2::SockRef::from(stream);
    let _ = sock.set_keepalive(true);
}

#[tokio::main]
async fn main() {
    if TOKEN.is_empty() {
        log("FATAL: COOP_SIGNALING_TOKEN not set -- refusing to start an open server");
        std::process::exit(1);
    }
    // TLS decision before any bind -- see the master's equivalent.
    let tls_acceptor = tls::acceptor_from_env();

    let port = env_int("COOP_SIGNALING_PORT", 10000) as u16;
    let addr = format!("0.0.0.0:{port}");
    let listener = match TcpListener::bind(&addr).await {
        Ok(l) => l,
        Err(e) => {
            log(&format!("FATAL: bind {addr} failed: {e}"));
            std::process::exit(1);
        }
    };
    log(&format!("signaling listening on {addr} (token auth required)"));

    // TLS listener on its OWN port beside the plaintext one (Tier B arc 1) --
    // see the master's equivalent for why parallel ports rather than a flip.
    match tls_acceptor {
        Some(acceptor) => {
            let tls_port = env_int("COOP_SIGNALING_TLS_PORT", 10442) as u16;
            let tls_addr = format!("0.0.0.0:{tls_port}");
            match TcpListener::bind(&tls_addr).await {
                Ok(l) => {
                    log(&format!("signaling TLS listening on {tls_addr}"));
                    tokio::spawn(serve_tls(l, acceptor));
                }
                Err(e) => {
                    log(&format!("FATAL: bind {tls_addr} failed: {e}"));
                    std::process::exit(1);
                }
            }
        }
        None => log("TLS not configured (COOP_TLS_CERT/COOP_TLS_KEY unset) -- plaintext only"),
    }

    serve_plain(listener).await
}

/// Plaintext accept loop. Every accept is logged with the listener tag -- this
/// is the evidence base for the arc-5 plaintext-retirement gate.
async fn serve_plain(listener: TcpListener) -> ! {
    loop {
        match listener.accept().await {
            Ok((stream, addr)) => {
                let ip = addr.ip().to_string();
                log(&format!("accept [listener=plain] [{ip}]"));
                set_keepalive(&stream);
                tokio::spawn(handle(stream, ip, "plain"));
            }
            Err(e) => log(&format!("accept error: {e}")),
        }
    }
}

/// TLS accept loop. The per-identity "registered" line inside serve() is what
/// gives arc 5 its POSITIVE proof that each install converted, so successful
/// accepts need no extra line here; handshake failures do get one.
async fn serve_tls(listener: TcpListener, acceptor: tokio_rustls::TlsAcceptor) -> ! {
    loop {
        match listener.accept().await {
            Ok((stream, addr)) => {
                let ip = addr.ip().to_string();
                set_keepalive(&stream);
                let acceptor = acceptor.clone();
                tokio::spawn(async move {
                    match acceptor.accept(stream).await {
                        Ok(tls_stream) => handle(tls_stream, ip, "tls").await,
                        Err(e) => log(&format!("tls handshake failed [{ip}]: {e}")),
                    }
                });
            }
            Err(e) => log(&format!("tls accept error: {e}")),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{check_registration_proof, REGISTER_TAG};
    use coop_server::common::identity_shape_ok;
    use ring::rand::SystemRandom;
    use ring::signature::{Ed25519KeyPair, KeyPair};

    /// A peer: its `gen:` identity and the key behind it. Built from a REAL
    /// keypair rather than fixed bytes so the positive arm exercises the same
    /// path a live client does.
    struct Peer {
        ident: String,
        kp: Ed25519KeyPair,
    }

    fn peer() -> Peer {
        let rng = SystemRandom::new();
        let pkcs8 = Ed25519KeyPair::generate_pkcs8(&rng).unwrap();
        let kp = Ed25519KeyPair::from_pkcs8(pkcs8.as_ref()).unwrap();
        let mut ident = String::from("gen:");
        for b in kp.public_key().as_ref() {
            ident.push_str(&format!("{b:02x}"));
        }
        Peer { ident, kp }
    }

    /// Sign `ident ‖ nonce` under the register tag and render the wire line.
    /// `as_ident` is what goes INTO the blob, which is how the squat arm makes a
    /// signature that names one identity while being offered under another.
    fn auth_line(p: &Peer, as_ident: &str, nonce: &str) -> String {
        let mut blob = Vec::new();
        blob.extend_from_slice(REGISTER_TAG);
        blob.extend_from_slice(as_ident.as_bytes());
        blob.extend_from_slice(nonce.as_bytes());
        let sig = p.kp.sign(&blob);
        let mut out = String::from("auth ");
        for b in sig.as_ref() {
            out.push_str(&format!("{b:02x}"));
        }
        out
    }

    const N1: &str = "1111111111111111111111111111111111111111111111111111111111111111";
    const N2: &str = "2222222222222222222222222222222222222222222222222222222222222222";

    #[test]
    fn a_real_holder_registers() {
        let p = peer();
        assert!(
            identity_shape_ok(&p.ident),
            "the generated identity is not the shape the server gates on"
        );
        assert!(check_registration_proof(&p.ident, N1, &auth_line(&p, &p.ident, N1)).is_ok());
    }

    #[test]
    fn the_ways_to_not_hold_the_key_are_all_refused() {
        let victim = peer();
        let attacker = peer();
        assert_ne!(
            victim.ident, attacker.ident,
            "two peers minted the SAME identity -- every arm below would prove nothing"
        );

        // 1. SQUAT -- exactly A59: sign with your own key, register as someone
        //    else. The attacker signs a blob naming the VICTIM, which is the
        //    strongest form (a blob naming itself would never be offered).
        assert!(
            check_registration_proof(&victim.ident, N1, &auth_line(&attacker, &victim.ident, N1))
                .is_err()
        );

        // 2. REPLAY -- a proof recorded from an earlier connection, whose nonce
        //    this one never issued.
        assert!(
            check_registration_proof(&victim.ident, N2, &auth_line(&victim, &victim.ident, N1))
                .is_err()
        );

        // 3. NO PROOF -- not an auth line at all (what a peer that skips the step
        //    produces, and what a relayed peer line would look like).
        assert!(check_registration_proof(&victim.ident, N1, "gen:dead beef").is_err());
        assert!(check_registration_proof(&victim.ident, N1, "").is_err());

        // 4. MALFORMED -- right verb, wrong bytes. Uppercase is its own case: the
        //    identity alphabet is lowercase-only for a measured reason, and the
        //    proof must not be laxer than the name it proves.
        let good = auth_line(&victim, &victim.ident, N1);
        assert!(check_registration_proof(&victim.ident, N1, &good[..good.len() - 2]).is_err());
        assert!(check_registration_proof(&victim.ident, N1, &good.to_uppercase()).is_err());
    }

    #[test]
    fn one_flipped_bit_is_refused() {
        // The arm that fails if verification is ever reduced to a length check.
        let p = peer();
        let mut line = auth_line(&p, &p.ident, N1).into_bytes();
        let last = line.len() - 1;
        line[last] = if line[last] == b'0' { b'1' } else { b'0' };
        assert!(check_registration_proof(&p.ident, N1, &String::from_utf8(line).unwrap()).is_err());
    }
}
