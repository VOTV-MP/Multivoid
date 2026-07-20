//! Production async P2P signaling server for VOTV coop.
//!
//! Rust port of `tools/coop_signaling_server.py`, WIRE-COMPATIBLE with it (identical
//! line protocol) so the Python and Rust signaling relays can run in parallel during
//! cutover. RULE 3: VPS infra, never ships in the mod.
//!
//! Wire protocol (line-oriented, '\n'-terminated, identities are space-free):
//!   greeting (first line) : <token> <identity>
//!   message  (subsequent) : <dest-identity> <hexpayload>
//!   forwarded to dest as  : <sender-identity> <hexpayload>
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

use coop_server::common::{ct_eq, env_int, env_str, log};
use coop_server::tls;
use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{LazyLock, Mutex};
use std::time::Duration;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt, ReadHalf};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::mpsc;
use tokio::time::timeout;

const MAX_LINE: usize = 64 * 1024;
const GREETING_TIMEOUT: Duration = Duration::from_secs(15);
const MAX_PENDING: usize = 128;
const MAX_AUTHED: usize = 512;
const MAX_AUTHED_PER_IP: u32 = 32;
const MAX_IDENTITY: usize = 64; // cap the greeting identity length (audit L10)
// Bounded relay backlog per destination. Worst-case per-dest heap =
// RELAY_QUEUE * MAX_RELAY_PAYLOAD. Sizing (security audit 2026-07-16, S-1): the
// prior 1024-deep queue of up-to-MAX_LINE (64 KiB) items let ONE token-holder pin
// ~64 MiB per stalled destination and OOM the shared box. Signaling payloads
// (SDP/ICE trickle) are a few KB, so we cap the relayed frame at MAX_RELAY_PAYLOAD
// and keep the queue shallow: 64 * 8 KiB = 512 KiB per dest, hard bound.
const RELAY_QUEUE: usize = 64;
const MAX_RELAY_PAYLOAD: usize = 8 * 1024; // drop a relayed line longer than this

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

async fn handle<S: AsyncRead + AsyncWrite + Unpin>(stream: S, ip: String) {
    // Admission into the bounded PRE-AUTH pool (an anonymous flood can fill only this
    // small pool; each conn is dropped after GREETING_TIMEOUT).
    if PENDING.fetch_add(1, Ordering::Relaxed) >= MAX_PENDING {
        PENDING.fetch_sub(1, Ordering::Relaxed);
        log(&format!("[{ip}] refused: pre-auth pool full"));
        return;
    }

    let mut reg: Option<Reg> = None;
    serve(stream, &ip, &mut reg).await;

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
    reg_out: &mut Option<Reg>,
) {
    // NOTE: SO_KEEPALIVE is set on the raw TcpStream at accept time (before any
    // TLS wrap), since socket options belong to the socket, not to the
    // record layer -- see serve_plain/serve_tls.
    let (rh, mut wh) = tokio::io::split(stream);
    let mut lr = LineReader::new(rh, MAX_LINE);

    // --- greeting: "<token> <identity>", short timeout (anti-slowloris) ---
    let line = match timeout(GREETING_TIMEOUT, lr.next_line()).await {
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
    log(&format!("[{identity}@{ip}] registered"));

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
                tokio::spawn(handle(stream, ip));
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
                        Ok(tls_stream) => handle(tls_stream, ip).await,
                        Err(e) => log(&format!("tls handshake failed [{ip}]: {e}")),
                    }
                });
            }
            Err(e) => log(&format!("tls accept error: {e}")),
        }
    }
}
