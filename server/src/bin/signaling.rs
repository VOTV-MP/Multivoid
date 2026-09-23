//! Production async P2P signaling server for VOTV coop.
//!
//! The rig launches this same binary for its scenarios (`tools/mp.py` builds it), so a
//! line-protocol change is always proven against the copy that ships.
//! RULE 3: VPS infra, never ships in
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
//! budget, counted from accept and covering the TLS handshake, so an
//! unauthenticated peer cannot hold a pre-auth slot longer than it could before.
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

use coop_server::admission::{Pool, Slot};
use coop_server::common::{clamp_str, ct_eq, env_int, env_str, identity_shape_ok, log, token_hex};
use coop_server::registration::check_registration_proof;
use coop_server::tls;
use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, LazyLock, Mutex};
use std::time::Duration;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt, ReadHalf};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::mpsc;
use tokio::time::{timeout_at, Instant};

// Before registration the only lines are the greeting (the token, a space, a 68-character
// identity: about 120 bytes) and the proof (134 bytes). 512 leaves room and keeps an
// unauthenticated peer from making the relay buffer more than that per slot.
const MAX_PREAUTH_LINE: usize = 512;
const GREETING_TIMEOUT: Duration = Duration::from_secs(15);
// The relay loop has no app idle timeout, so keepalive IS the reap of a dead authed peer -- and at
// the OS default (Linux waits two hours for a first probe) that reap is far too late to matter. A
// registration outlives its socket for the whole of that window, and `relay_line` then routes a
// joiner's rendezvous into a channel nobody reads: the dial dies at `Connecting` with nothing said
// on either end, which is what the field measured against a host that had been idle here for an
// hour with its listener still open. 30 s idle, a probe every 5 s -- with Linux's default nine
// probes that is a ~75 s reap, not the minute it reads like; TCP_KEEPCNT would need socket2's
// `all` feature, which this crate does not enable. The client arms its own pair for the other half
// of the same defect (coop/net/signaling_client.cpp, kKeepAliveIdleMs).
const KEEPALIVE_IDLE: Duration = Duration::from_secs(30);
const KEEPALIVE_INTERVAL: Duration = Duration::from_secs(5);
// 80, not 64 (raised 2026-08-29): since b144 a peer registers under its DURABLE
// identity, which GNS renders as `gen:` + 64 hex = 68 chars. At 64 the greeting
// was refused outright, which reads as "P2P is down" rather than as a length cap.
const MAX_IDENTITY: usize = 80;
// Bounded relay backlog per destination. Sizing (security audit 2026-07-16, S-1): the
// prior 1024-deep queue of up-to-64 KiB items let ONE token-holder pin ~64 MiB per
// stalled destination. Signaling payloads (SDP/ICE trickle) are a few KB, so the
// relayed frame is capped at MAX_RELAY_PAYLOAD -- the longest line a registered peer
// may send at all -- and the queue at RELAY_QUEUE frames AND MAX_QUEUED_PER_DEST
// bytes. The byte cap is what bounds the whole relay: frames alone (64 x 8 KiB)
// times the 512 authed slots came to 256 MiB, twice the unit's MemoryMax, so a few
// sources that register and stop reading could have it OOM-killed. 128 KiB per
// destination is 64 MiB over every slot, and still holds 64 small ICE frames.
const RELAY_QUEUE: usize = 64;
const MAX_RELAY_PAYLOAD: usize = 8 * 1024;
const MAX_QUEUED_PER_DEST: usize = 128 * 1024;

static TOKEN: LazyLock<String> = LazyLock::new(|| env_str("COOP_SIGNALING_TOKEN", ""));
static CLIENTS: LazyLock<Mutex<HashMap<String, Route>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
// Connections that have not proved their key yet: the small pool an anonymous flood can fill,
// taken at accept, and one address may hold only a few of its slots.
static PENDING: LazyLock<Pool> = LazyLock::new(|| Pool::new("pre-auth", 128, 8));
// Registered peers: one per install while it hosts or joins, so 32 per address is a large
// household or a carrier NAT.
static AUTHED: LazyLock<Pool> = LazyLock::new(|| Pool::new("authed", 512, 32));
static CONN_SEQ: AtomicU64 = AtomicU64::new(1);

/// Bounded, cancel-safe line reader over the connection's read half. On `select!`
/// cancellation the in-flight `read` consumes nothing (tokio `read` is cancel-safe),
/// and already-buffered bytes persist in `buf` for the next call.
struct LineReader<S> {
    rh: ReadHalf<S>,
    buf: Vec<u8>,
    // How much of `buf` has been searched for a newline, so each byte is scanned once:
    // rescanning the whole buffer made a line trickled a byte at a time cost quadratic
    // CPU (588 ms for a 64 KiB line, per connection).
    scanned: usize,
    max: usize,
}

enum LineErr {
    TooLong,
    Closed,
}

impl<S: AsyncRead + Unpin> LineReader<S> {
    fn new(rh: ReadHalf<S>, max: usize) -> LineReader<S> {
        LineReader { rh, buf: Vec::with_capacity(256), scanned: 0, max }
    }

    /// Next line INCLUDING its trailing '\n' (matches the Python `readuntil(b"\n")`).
    async fn next_line(&mut self) -> Result<Vec<u8>, LineErr> {
        loop {
            if let Some(i) = self.buf[self.scanned..].iter().position(|b| *b == b'\n') {
                let line: Vec<u8> = self.buf.drain(..=self.scanned + i).collect();
                self.scanned = 0;
                return Ok(line);
            }
            self.scanned = self.buf.len();
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

/// Where a registered identity's relayed lines go: its connection's queue.
struct Route {
    conn_id: u64,
    tx: mpsc::Sender<Queued>,
    // Bytes waiting in `tx`, against MAX_QUEUED_PER_DEST.
    queued: Arc<AtomicUsize>,
}

/// One relayed frame in a destination's queue. Its bytes count against that queue's budget
/// until it is written out or dropped with the queue, so no path can leave them charged.
struct Queued {
    bytes: Vec<u8>,
    budget: Arc<AtomicUsize>,
}

impl Queued {
    /// Charge `bytes` to a destination's budget, or refuse them when it is spent.
    fn charge(bytes: Vec<u8>, budget: &Arc<AtomicUsize>) -> Option<Queued> {
        let n = bytes.len();
        if budget.fetch_add(n, Ordering::Relaxed) + n > MAX_QUEUED_PER_DEST {
            budget.fetch_sub(n, Ordering::Relaxed);
            return None;
        }
        Some(Queued { bytes, budget: Arc::clone(budget) })
    }
}

impl Drop for Queued {
    fn drop(&mut self) {
        self.budget.fetch_sub(self.bytes.len(), Ordering::Relaxed);
    }
}

/// A live registration. Dropping it -- on return or on an unwind -- takes its route out
/// of CLIENTS (unless a newer connection under the same identity has replaced it) and
/// then gives its authed slot back.
struct Reg {
    identity: String,
    conn_id: u64,
    ip: String,
    _authed: Slot<'static>,
}

impl Drop for Reg {
    fn drop(&mut self) {
        let mut cl = CLIENTS.lock().unwrap_or_else(|e| e.into_inner());
        if cl.get(&self.identity).map(|r| r.conn_id) == Some(self.conn_id) {
            cl.remove(&self.identity);
            log(&format!("[{}@{}] disconnected", self.identity, self.ip));
        }
    }
}

/// Run one connection, admitted at accept into the pre-auth pool: greet, prove, promote
/// into the authed pool, then relay. Returns when the connection ends. What it holds
/// gives itself back when it drops -- the pre-auth slot unless promotion took it first,
/// the registration with its route and authed slot -- and a drop also runs on an unwind,
/// so a panicking task leaks none of them (a leaked slot refuses every later accept once
/// its pool is full, until a restart).
async fn handle<S: AsyncRead + AsyncWrite + Unpin>(
    stream: S,
    ip: String,
    listener: &'static str,
    pending: Slot<'static>,
    deadline: Instant,
) {
    let ip = ip.as_str();
    let mut pending = Some(pending);
    // NOTE: SO_KEEPALIVE is set on the raw TcpStream at accept time (before any
    // TLS wrap), since socket options belong to the socket, not to the
    // record layer -- see serve_plain/serve_tls.
    let (rh, mut wh) = tokio::io::split(stream);
    let mut lr = LineReader::new(rh, MAX_PREAUTH_LINE);

    // ONE budget for the WHOLE pre-registration phase (handshake + greeting + proof),
    // set at accept, not one per line: a peer that answers each line just inside a
    // per-line timeout could otherwise hold a pre-auth slot for as long as it kept
    // adding lines.

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
        // clamp_str, because THIS is the one place an identity reaches the log
        // without having passed identity_shape_ok -- that is what the branch
        // MEANS. A raw echo lets a greeter embed CR and ANSI and forge a
        // "registered (key proved)" line in the very log that is the operator's
        // answer to "is the A59 gate armed" (post-ship audit M2).
        log(&format!(
            "[{ip}] REFUSED: identity '{}' is not a key (`gen:<64 hex>`). A \
             pre-b145 peer lands here -- that cohort is retired; it must update.",
            clamp_str(ident, MAX_IDENTITY)
        ));
        return;
    }
    // Runs BEFORE the AUTHED reservation on purpose: a connection that never
    // answers must occupy the small bounded PENDING pool, which exists for exactly
    // this shape of flood, rather than one of the 512 authed slots.
    {
        // TEST AFFORDANCE, off by default (0). The ONE ordering no loopback run
        // can stage: on a real relay the nonce costs an RTT, during which GNS
        // emits its own rendezvous signals -- and if the client lets one overtake
        // the proof, we read it as a malformed proof and refuse. Delaying the
        // nonce reproduces that on loopback. It sits on the RELAY, i.e. the
        // ENVIRONMENT, never on the gate under test (the client's send ordering) --
        // the same discipline authdrill uses when it puts its sabotage entirely on
        // the client side. Bounded by the pre-registration deadline either way.
        let delay = env_int("COOP_SIGNALING_NONCE_DELAY_MS", 0);
        if delay > 0
            && timeout_at(deadline, tokio::time::sleep(Duration::from_millis(delay as u64))).await.is_err()
        {
            return;
        }
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

    // Auth OK -> an authed slot. The total and the per-address cap are checked and
    // taken under one lock, so concurrent promotions cannot over-admit past either
    // (audit LOW-1). Then the pre-auth slot goes back.
    let authed = match AUTHED.admit(ip) {
        Ok(slot) => slot,
        Err(why) => {
            log(&format!("[{ident}@{ip}] refused: {why}"));
            return;
        }
    };
    pending.take();

    let identity = ident.to_string();
    let conn_id = CONN_SEQ.fetch_add(1, Ordering::Relaxed);
    let (tx, mut rx) = mpsc::channel::<Queued>(RELAY_QUEUE);
    let queued = Arc::new(AtomicUsize::new(0));

    // Register (evict-on-duplicate-identity, token-gated). Overwriting the map entry
    // drops the previous Sender -> the previous connection's select sees recv()==None
    // -> it stops and closes its socket.
    {
        let mut cl = CLIENTS.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(prev) = cl.get(&identity) {
            if prev.conn_id != conn_id {
                log(&format!("[{identity}@{ip}] replaced previous connection"));
            }
        }
        cl.insert(identity.clone(), Route { conn_id, tx, queued });
    }
    let _reg = Reg { identity: identity.clone(), conn_id, ip: ip.to_string(), _authed: authed };
    // A registered peer's lines are relay frames: one longer than the relay forwards
    // ends the connection instead of being buffered.
    lr.max = MAX_RELAY_PAYLOAD;
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
                    Some(frame) => {
                        if wh.write_all(&frame.bytes).await.is_err() {
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
///
/// A line a peer addresses to ITSELF is routed like any other, and that is LOAD-BEARING, not an
/// accident to tidy away: it is the only way a registered peer can learn that its name still
/// resolves to its connection, which a live socket cannot tell it (see the keepalive note on
/// KEEPALIVE_IDLE for what that cost in the field). The client sends one every 20 s with an empty
/// payload and retires its registration after 45 s of silence, so a self-route guard here would
/// make every host reconnect on a timer forever.
fn relay_line(sender: &str, line: &[u8]) {
    // S-1 (audit 2026-07-16): drop over-length lines BEFORE building/queuing the
    // relayed frame. The reader already ends a connection whose line outgrows
    // MAX_RELAY_PAYLOAD, but a line that arrives whole in one read can pass that
    // check by up to a read's length. Real SDP/ICE frames are a few KB.
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
    if let Some(route) = cl.get(dest) {
        // Non-blocking and best-effort: drop when the destination's byte budget is
        // spent, its queue is Full (slow dest) or Closed (gone) -- the bounded form of
        // the Python 5s-drain relay. A refused frame comes back in the error and drops,
        // which gives its bytes back.
        if let Some(frame) = Queued::charge(msg, &route.queued) {
            let _ = route.tx.try_send(frame);
        }
    }
}

/// Enable TCP keepalive with an explicit idle/interval, so a dead authed peer is reaped in about a
/// minute instead of at the OS default (see KEEPALIVE_IDLE for what the default costs). Portable
/// via socket2's SockRef; failure is non-fatal, and falls back to the bare flag so the reap still
/// arrives eventually.
fn set_keepalive(stream: &TcpStream) {
    let sock = socket2::SockRef::from(stream);
    let ka = socket2::TcpKeepalive::new()
        .with_time(KEEPALIVE_IDLE)
        .with_interval(KEEPALIVE_INTERVAL);
    if sock.set_tcp_keepalive(&ka).is_err() {
        let _ = sock.set_keepalive(true);
    }
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
                    tokio::spawn(serve_tls(l, acceptor, &PENDING, GREETING_TIMEOUT));
                }
                Err(e) => {
                    log(&format!("FATAL: bind {tls_addr} failed: {e}"));
                    std::process::exit(1);
                }
            }
        }
        None => log("TLS not configured (COOP_TLS_CERT/COOP_TLS_KEY unset) -- plaintext only"),
    }

    serve_plain(listener, &PENDING, GREETING_TIMEOUT).await
}

/// Plaintext accept loop. Every admitted accept is logged with the listener tag --
/// this is the evidence base for the arc-5 plaintext-retirement gate; a refused one
/// is counted by the pool's refusal line. Admission into the pre-auth pool is taken
/// at accept, before a byte is read (see `admission`); a refused connection is
/// dropped unanswered.
async fn serve_plain(listener: TcpListener, pool: &'static Pool, budget: Duration) -> ! {
    loop {
        match listener.accept().await {
            Ok((stream, addr)) => {
                let ip = addr.ip().to_string();
                let Some(pending) = pool.admit_or_log(&ip) else { continue };
                log(&format!("accept [listener=plain] [{ip}]"));
                set_keepalive(&stream);
                let deadline = Instant::now() + budget;
                tokio::spawn(handle(stream, ip, "plain", pending, deadline));
            }
            Err(e) => log(&format!("accept error: {e}")),
        }
    }
}

/// TLS accept loop. The per-identity "registered" line inside serve() is what
/// gives arc 5 its POSITIVE proof that each install converted, so successful
/// accepts need no extra line here; handshake failures do get one. The handshake
/// runs inside the pre-auth slot and its deadline.
async fn serve_tls(
    listener: TcpListener,
    acceptor: tokio_rustls::TlsAcceptor,
    pool: &'static Pool,
    budget: Duration,
) -> ! {
    loop {
        match listener.accept().await {
            Ok((stream, addr)) => {
                let ip = addr.ip().to_string();
                let Some(pending) = pool.admit_or_log(&ip) else { continue };
                set_keepalive(&stream);
                let deadline = Instant::now() + budget;
                let acceptor = acceptor.clone();
                tokio::spawn(async move {
                    match timeout_at(deadline, acceptor.accept(stream)).await {
                        Ok(Ok(tls_stream)) => handle(tls_stream, ip, "tls", pending, deadline).await,
                        Ok(Err(e)) => log(&format!("tls handshake failed [{ip}]: {e}")),
                        Err(_) => log(&format!("tls handshake timed out [{ip}]")),
                    }
                });
            }
            Err(e) => log(&format!("tls accept error: {e}")),
        }
    }
}

#[cfg(test)]
mod admission_tests {
    use super::{serve_plain, serve_tls};
    use coop_server::admission::Pool;
    use std::sync::{Arc, LazyLock};
    use std::time::Duration;
    use tokio::io::AsyncReadExt;
    use tokio::net::{TcpListener, TcpStream};
    use tokio_rustls::rustls::server::{ClientHello, ResolvesServerCert};
    use tokio_rustls::rustls::sign::CertifiedKey;
    use tokio_rustls::rustls::ServerConfig;
    use tokio_rustls::TlsAcceptor;

    /// No certificate at all: enough to build an acceptor, since these clients never send the
    /// ClientHello a certificate would answer.
    #[derive(Debug)]
    struct NoCert;
    impl ResolvesServerCert for NoCert {
        fn resolve(&self, _: ClientHello<'_>) -> Option<Arc<CertifiedKey>> {
            None
        }
    }

    fn acceptor() -> TlsAcceptor {
        let cfg = ServerConfig::builder().with_no_client_auth().with_cert_resolver(Arc::new(NoCert));
        TlsAcceptor::from(Arc::new(cfg))
    }

    /// True when the server closes this idle client within `within`.
    async fn closed_within(s: &mut TcpStream, within: Duration) -> bool {
        let mut b = [0u8; 1];
        matches!(tokio::time::timeout(within, s.read(&mut b)).await, Ok(Ok(0)) | Ok(Err(_)))
    }

    async fn listen() -> (TcpListener, std::net::SocketAddr) {
        let l = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let a = l.local_addr().unwrap();
        (l, a)
    }

    #[tokio::test]
    async fn a_client_that_never_sends_a_client_hello_is_dropped_at_the_deadline() {
        static POOL: LazyLock<Pool> = LazyLock::new(|| Pool::new("test", 8, 8));
        let (l, addr) = listen().await;
        tokio::spawn(serve_tls(l, acceptor(), &POOL, Duration::from_millis(400)));
        let mut c = TcpStream::connect(addr).await.unwrap();
        assert!(!closed_within(&mut c, Duration::from_millis(100)).await, "dropped before its deadline");
        assert!(
            closed_within(&mut c, Duration::from_secs(3)).await,
            "a client that never sent a ClientHello was held past the deadline"
        );
    }

    #[tokio::test]
    async fn a_silent_plaintext_greeter_is_dropped_at_the_deadline() {
        static POOL: LazyLock<Pool> = LazyLock::new(|| Pool::new("test", 8, 8));
        let (l, addr) = listen().await;
        tokio::spawn(serve_plain(l, &POOL, Duration::from_millis(400)));
        let mut c = TcpStream::connect(addr).await.unwrap();
        assert!(!closed_within(&mut c, Duration::from_millis(100)).await, "dropped before its deadline");
        assert!(closed_within(&mut c, Duration::from_secs(3)).await, "a silent greeter was held past the deadline");
    }

    #[tokio::test]
    async fn one_address_over_its_pre_auth_cap_is_dropped_before_any_handshake() {
        // The accept loop takes connections in order, so the third one is the one over the cap.
        static POOL: LazyLock<Pool> = LazyLock::new(|| Pool::new("test", 8, 2));
        let (l, addr) = listen().await;
        tokio::spawn(serve_tls(l, acceptor(), &POOL, Duration::from_secs(20)));
        let mut a = TcpStream::connect(addr).await.unwrap();
        let mut b = TcpStream::connect(addr).await.unwrap();
        let mut c = TcpStream::connect(addr).await.unwrap();
        assert!(closed_within(&mut c, Duration::from_secs(3)).await, "a third connection from one address was held");
        assert!(!closed_within(&mut a, Duration::from_millis(300)).await, "an admitted connection was dropped");
        assert!(!closed_within(&mut b, Duration::from_millis(300)).await, "an admitted connection was dropped");
    }
}

#[cfg(test)]
mod tests {
    use coop_server::registration::blob;
    use ring::rand::SystemRandom;
    use ring::signature::{Ed25519KeyPair, KeyPair};

    /// A throwaway key and the `gen:` identity it names.
    fn peer() -> (String, Ed25519KeyPair) {
        let pkcs8 = Ed25519KeyPair::generate_pkcs8(&SystemRandom::new()).unwrap();
        let kp = Ed25519KeyPair::from_pkcs8(pkcs8.as_ref()).unwrap();
        let ident: String =
            std::iter::once("gen:".to_string()).chain(kp.public_key().as_ref().iter().map(|b| format!("{b:02x}"))).collect();
        (ident, kp)
    }

    /// The proof line `kp` signs for `ident` and the relay's `nonce`.
    fn auth_line(kp: &Ed25519KeyPair, ident: &str, nonce: &str) -> String {
        let sig = kp.sign(&blob(ident, nonce));
        std::iter::once("auth ".to_string()).chain(sig.as_ref().iter().map(|b| format!("{b:02x}"))).collect()
    }

    /// Poll `cond` for up to three seconds: the relay moves the slots on its own task.
    async fn wait_for(what: &str, cond: impl Fn() -> bool) {
        for _ in 0..300 {
            if cond() {
                return;
            }
            tokio::time::sleep(std::time::Duration::from_millis(10)).await;
        }
        panic!("timed out waiting for: {what}");
    }

    #[test]
    fn a_destination_queue_is_capped_in_bytes_and_gives_them_back() {
        use super::{Queued, MAX_QUEUED_PER_DEST};
        use std::sync::atomic::{AtomicUsize, Ordering};
        use std::sync::Arc;

        let budget = Arc::new(AtomicUsize::new(0));
        let frame = || vec![0u8; MAX_QUEUED_PER_DEST / 4];
        let held: Vec<Queued> = (0..4).map(|_| Queued::charge(frame(), &budget).expect("under budget")).collect();
        assert!(Queued::charge(frame(), &budget).is_none(), "a frame past the byte budget was queued");
        assert_eq!(budget.load(Ordering::Relaxed), MAX_QUEUED_PER_DEST, "a refused frame stayed charged");
        drop(held);
        assert_eq!(budget.load(Ordering::Relaxed), 0, "dropped frames kept their bytes charged");
    }

    #[tokio::test]
    async fn a_line_split_across_reads_is_found_and_an_overlong_one_refused() {
        use super::{LineErr, LineReader};
        use tokio::io::AsyncWriteExt;

        let (mut tx, rx) = tokio::io::duplex(64);
        let (rh, _wh) = tokio::io::split(rx);
        let mut lr = LineReader::new(rh, 16);
        tx.write_all(b"abc").await.unwrap();
        tx.write_all(b"def\nxyz\n").await.unwrap();
        assert_eq!(lr.next_line().await.ok().unwrap(), b"abcdef\n");
        assert_eq!(lr.next_line().await.ok().unwrap(), b"xyz\n");
        tx.write_all(&[b'a'; 40]).await.unwrap();
        assert!(matches!(lr.next_line().await, Err(LineErr::TooLong)), "a line past the cap was buffered on");
    }

    #[tokio::test]
    async fn a_registration_trades_its_pre_auth_slot_for_an_authed_one_and_gives_it_back() {
        use super::{serve_plain, AUTHED, CLIENTS};
        use coop_server::admission::Pool;
        use std::sync::LazyLock;
        use std::time::Duration;
        use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
        use tokio::net::{TcpListener, TcpStream};

        // The token is read once, at the first greeting, and no other test in this binary
        // sends one.
        std::env::set_var("COOP_SIGNALING_TOKEN", "test-token");
        static PENDING: LazyLock<Pool> = LazyLock::new(|| Pool::new("test", 8, 8));
        let l = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = l.local_addr().unwrap();
        tokio::spawn(serve_plain(l, &PENDING, Duration::from_secs(10)));

        let (ident, kp) = peer();
        let (rd, mut wr) = TcpStream::connect(addr).await.unwrap().into_split();
        let mut rd = BufReader::new(rd);
        wr.write_all(format!("test-token {ident}\n").as_bytes()).await.unwrap();
        let mut nonce = String::new();
        rd.read_line(&mut nonce).await.unwrap();
        let nonce = nonce.trim().strip_prefix("nonce ").expect("no nonce line").to_string();
        wr.write_all(format!("{}\n", auth_line(&kp, &ident, &nonce)).as_bytes()).await.unwrap();

        let routed = || CLIENTS.lock().unwrap().contains_key(&ident);
        wait_for("the registration's route, one authed slot and no pre-auth slot", || {
            routed() && AUTHED.held_by("127.0.0.1") == 1 && PENDING.held() == 0
        })
        .await;
        drop((rd, wr));
        wait_for("the route and the authed slot to go with the connection", || {
            !routed() && AUTHED.held_by("127.0.0.1") == 0
        })
        .await;
    }
}
