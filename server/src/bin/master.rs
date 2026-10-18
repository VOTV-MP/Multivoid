//! Production async HTTP master / lobby server for VOTV coop (zero-open-ports MP).
//!
//! RULE 3: VPS infra, never ships in the mod. The endpoint list, the security posture
//! and the careful spots are in the README beside this crate; the byte-exact spot (the
//! coturn TURN credential) is called out inline and unit-tested.
//!
//! Concurrency model: a single `Mutex<MasterState>` guards the lobby maps + rate
//! buckets + the /v1/lobbies cache. Handlers are SYNCHRONOUS (pure CPU) and run
//! entirely under the lock; every socket await (read body, write response) happens
//! OUTSIDE the lock. This mirrors the Python asyncio single-loop model — no lock is
//! ever held across an await — while still using the multi-thread tokio runtime.


use coop_server::admission::{Pool, CONNECTS};
use coop_server::common::{env_int, env_str, log};
use coop_server::http_transport::{
    json_bytes, parse_head, read_head, write_response, HeadErr, HeadReject, CONNS, HTTP_TIMEOUT, MAX_BODY,
    MAX_HEADER,
};
use coop_server::lobby::{
    dispatch_post, h_browse, latest_answer, lock_state, parse_latest_games, resolve_client_ip, sweeper,
    LATEST_URL,
};
use coop_server::master_config::{blocked_faults, config_faults, CFG};
use coop_server::thanks::{thanks_answer, Answer};
use coop_server::tls;
use serde_json::{json, Value};
use std::sync::LazyLock;
use std::time::Duration;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite};
use tokio::time::{timeout_at, Instant};
use tokio::net::TcpListener;

/// One request. The connection was admitted at accept, and `deadline` -- set there too -- bounds
/// everything up to the answer: the TLS handshake the caller already did, the head and the body.
async fn handle<S: AsyncRead + AsyncWrite + Unpin>(mut stream: S, peer_ip: String, deadline: Instant) {
    // ---- read + parse header block (bounded + timed) ----
    let (head, leftover) = match timeout_at(deadline, read_head(&mut stream, MAX_HEADER)).await {
        Ok(Ok(v)) => v,
        Ok(Err(HeadErr::TooLarge)) => {
            write_response(&mut stream, 413, &json_bytes(&json!({"error": "headers too large"}))).await;
            return;
        }
        Ok(Err(HeadErr::Closed)) | Err(_) => return, // eof / timeout
    };

    let (method, path, content_length, client_ip) = match parse_head(&head) {
        Ok(h) => (
            h.method.to_string(),
            h.path.to_string(),
            h.content_length.map(str::to_string),
            resolve_client_ip(&peer_ip, h.x_real_ip, h.x_forwarded_for),
        ),
        Err(why) => {
            let error = match why {
                HeadReject::NotText => "head is not text",
                HeadReject::BadRequestLine => "bad request line",
            };
            write_response(&mut stream, 400, &json_bytes(&json!({ "error": error }))).await;
            return;
        }
    };
    // A connection holds its head or its body, never both.
    drop(head);
    let (method, path) = (method.as_str(), path.as_str());

    // ---- POST: read, parse and answer in one expression, so the body and its parse are dropped
    // before the answer's write, which can wait out the deadline on a client that stops reading.
    if method == "POST" {
        let (status, answer) = match read_post_body(&mut stream, leftover, content_length.as_deref(), deadline).await {
            Err(None) => return, // eof / timeout
            Err(Some((status, error))) => (status, json_bytes(&json!({ "error": error }))),
            Ok(body) => match dispatch_post(path, &client_ip, &body) {
                Some((status, resp)) => (status, json_bytes(&resp)),
                None => (404, json_bytes(&json!({"error": "not found"}))),
            },
        };
        write_response(&mut stream, status, &answer).await;
        return;
    }

    // ---- route ----
    if method == "GET" && path == "/v1/lobbies" {
        // Refresh + snapshot under the lock; the write happens OFF the lock (L5). The query string
        // is not read: every client asks for the whole list. The list has its own rate class.
        let body = {
            let mut state = lock_state();
            h_browse(&mut state, &client_ip)
        };
        match body {
            Some(body) => write_response(&mut stream, 200, &body).await,
            None => write_response(&mut stream, 429, &json_bytes(&json!({"error": "rate"}))).await,
        }
    } else if method == "GET" && path == "/v1/latest" {
        write_response(&mut stream, 200, &LATEST).await;
    } else if method == "GET" && path == "/v1/thanks" {
        // The thanks list the mod's main menu rolls. Three answers, and the mod acts on the
        // difference: the text; 404, the only answer that lets a client retire the copy it cached
        // from us; 503, a list we have and could not serve this moment, on which a client keeps
        // what it holds. The body is serialized once per re-read window, not per request.
        match thanks_answer().await {
            Answer::Body(body) => write_response(&mut stream, 200, &body).await,
            Answer::NoList => {
                write_response(&mut stream, 404, &json_bytes(&json!({"error": "no list"}))).await
            }
            Answer::Unavailable => {
                write_response(&mut stream, 503, &json_bytes(&json!({"error": "list unavailable"}))).await
            }
        }
    } else if method == "GET" && path == "/healthz" {
        let n = lock_state().lobbies.len();
        write_response(&mut stream, 200, &json_bytes(&json!({"ok": true, "lobbies": n}))).await;
    } else {
        write_response(&mut stream, 404, &json_bytes(&json!({"error": "not found"}))).await;
    }
}

/// A POST body, read into one buffer of its declared length and parsed as a JSON object.
/// `Err(None)`: the client went away or ran out the deadline; `Err(Some(..))`: the answer that
/// refuses it.
async fn read_post_body<S: AsyncRead + Unpin>(
    stream: &mut S,
    mut raw: Vec<u8>,
    content_length: Option<&str>,
    deadline: Instant,
) -> Result<Value, Option<(u16, &'static str)>> {
    let clen = content_length.and_then(|s| s.trim().parse::<usize>().ok()).filter(|n| *n <= MAX_BODY);
    let Some(clen) = clen else { return Err(Some((413, "body too large"))) };
    let have = raw.len().min(clen);
    raw.resize(clen, 0);
    if have < clen && !matches!(timeout_at(deadline, stream.read_exact(&mut raw[have..])).await, Ok(Ok(_))) {
        return Err(None);
    }
    if raw.is_empty() {
        return Ok(Value::Object(serde_json::Map::new()));
    }
    match serde_json::from_slice::<Value>(&raw) {
        Ok(v) if v.is_object() => Ok(v),
        _ => Err(Some((400, "bad json"))),
    }
}

// The release record, from the env once (see the LATEST_URL comment in lobby.rs). A list entry that does not parse,
// or names a game a second time, is dropped and named; an empty list is said too -- never silent.
static LATEST: LazyLock<Vec<u8>> = LazyLock::new(|| {
    let (games, refused) = parse_latest_games(&env_str("COOP_LATEST_GAMES", ""));
    for entry in &refused {
        log(&format!("WARN: COOP_LATEST_GAMES entry '{}' refused (want <game>:<build>:<tag>, each game once)", entry));
    }
    if games.is_empty() {
        log("WARN: COOP_LATEST_GAMES names no release -- /v1/latest answers with no record");
    }
    json_bytes(&latest_answer(&games, &env_str("COOP_LATEST_URL", LATEST_URL)))
});

#[tokio::main]
async fn main() {
    // Built now, so its settings are logged at start rather than at the first connect.
    LazyLock::force(&CONNECTS);
    // The same for the release record: a typo in COOP_LATEST_GAMES is named at the restart that brought it.
    LazyLock::force(&LATEST);
    // fail fast on missing secrets (same as the Python FATAL exits)
    if CFG.turn_secret.is_empty() {
        log("FATAL: COOP_TURN_SECRET not set -- refusing to mint TURN creds");
        std::process::exit(1);
    }
    if CFG.signaling_token.is_empty() {
        log("FATAL: COOP_SIGNALING_TOKEN not set -- clients could not reach signaling");
        std::process::exit(1);
    }
    // A name missing, or a name or a floor of the wrong shape, would start a master no client can use.
    let mut faults = config_faults(
        &env_str("COOP_DOMAIN", ""),
        &env_str("COOP_MASTER_NAME_ALIASES", ""),
        &env_str("COOP_MIN_LISTED_PROTO", ""),
    );
    // A block-list entry that is not a key identity would match no host and say nothing.
    faults.extend(blocked_faults(&env_str("COOP_BLOCKED_IDENTITIES", "")));
    for fault in &faults {
        log(&format!("FATAL: {fault}"));
    }
    if !faults.is_empty() {
        std::process::exit(1);
    }

    // Resolve the TLS decision BEFORE binding anything: with COOP_REQUIRE_TLS=1
    // and no cert this exits, and it must do so without having briefly bound a
    // cleartext port (Restart=always would otherwise flap one open every cycle).
    let tls_acceptor = tls::acceptor_from_env();

    let addr = format!("0.0.0.0:{}", CFG.port);
    let listener = match TcpListener::bind(&addr).await {
        Ok(l) => l,
        Err(e) => {
            log(&format!("FATAL: bind {addr} failed: {e}"));
            std::process::exit(1);
        }
    };
    log(&format!(
        "master listening on {addr} (signaling={} stun={} turn={} names={} min_listed_proto={} blocked={})",
        if CFG.signaling_url.is_empty() { "?" } else { &CFG.signaling_url },
        if CFG.stun_uri.is_empty() { "?" } else { &CFG.stun_uri },
        if CFG.turn_uri.is_empty() { "off" } else { "on" },
        CFG.public_names.join(","),
        CFG.min_listed_proto,
        CFG.blocked_identities.len()
    ));

    tokio::spawn(sweeper());

    // TLS listener on its OWN port, beside the plaintext one (Tier B arc 1).
    // Parallel ports -- not an in-place flip -- so every client build keeps
    // working through the cutover window and no intermediate state is knowingly
    // broken; the plaintext listener is retired in arc 5, gated on the accept
    // log below showing zero unknown-source plaintext connections.
    match tls_acceptor {
        Some(acceptor) => {
            let tls_port = env_int("COOP_MASTER_TLS_PORT", 10443) as u16;
            let tls_addr = format!("0.0.0.0:{tls_port}");
            match TcpListener::bind(&tls_addr).await {
                Ok(l) => {
                    log(&format!("master TLS listening on {tls_addr}"));
                    tokio::spawn(serve_tls(l, acceptor, &CONNS, HTTP_TIMEOUT));
                }
                Err(e) => {
                    // A configured TLS listener that cannot bind is FATAL: coming
                    // up plaintext-only on a box meant to serve TLS is the silent
                    // downgrade this tier exists to prevent.
                    log(&format!("FATAL: bind {tls_addr} failed: {e}"));
                    std::process::exit(1);
                }
            }
        }
        None => log("TLS not configured (COOP_TLS_CERT/COOP_TLS_KEY unset) -- plaintext only"),
    }

    serve_plain(listener, &CONNS, HTTP_TIMEOUT).await
}

/// Plaintext accept loop. Logs EVERY admitted accept with the listener tag: this
/// log is the evidence base for the arc-5 retirement gate ("zero unknown-source
/// plaintext connections over 24h"). Without it the gate would be a hope -- the
/// 400 path below never logs, so a stray connection could pass unseen. A refused
/// one is counted by the pool's refusal line instead. Admission is taken at accept,
/// before a byte is read (see `admission`); a refused connection is dropped
/// unanswered.
async fn serve_plain(listener: TcpListener, pool: &'static Pool, budget: Duration) -> ! {
    loop {
        match listener.accept().await {
            Ok((stream, addr)) => {
                let peer_ip = addr.ip().to_string();
                if !CONNECTS.allow(&peer_ip, std::time::Instant::now()) {
                    continue;
                }
                let Some(slot) = pool.admit_or_log(&peer_ip) else { continue };
                log(&format!("accept [listener=plain] [{peer_ip}]"));
                let deadline = Instant::now() + budget;
                tokio::spawn(async move {
                    let _slot = slot;
                    handle(stream, peer_ip, deadline).await
                });
            }
            Err(e) => log(&format!("accept error: {e}")),
        }
    }
}

/// TLS accept loop. Handshake failures are logged (they are the observable a
/// client-side cert problem produces); successful accepts are NOT logged per
/// connection -- the lobby-list poll would make that pure noise, and the arc-5
/// positive proof comes from the signaling server's per-identity registration
/// lines instead. The handshake runs inside the connection's admitted slot and
/// its deadline: a peer that never sends a ClientHello holds one slot until then.
async fn serve_tls(
    listener: TcpListener,
    acceptor: tokio_rustls::TlsAcceptor,
    pool: &'static Pool,
    budget: Duration,
) -> ! {
    loop {
        match listener.accept().await {
            Ok((stream, addr)) => {
                let peer_ip = addr.ip().to_string();
                if !CONNECTS.allow(&peer_ip, std::time::Instant::now()) {
                    continue;
                }
                let Some(slot) = pool.admit_or_log(&peer_ip) else { continue };
                let deadline = Instant::now() + budget;
                let acceptor = acceptor.clone();
                tokio::spawn(async move {
                    let _slot = slot;
                    match timeout_at(deadline, acceptor.accept_with(stream, tls::limit_send_buffer)).await {
                        Ok(Ok(tls_stream)) => handle(tls_stream, peer_ip, deadline).await,
                        Ok(Err(e)) => log(&format!("tls handshake failed [{peer_ip}]: {e}")),
                        Err(_) => log(&format!("tls handshake timed out [{peer_ip}]")),
                    }
                });
            }
            Err(e) => log(&format!("tls accept error: {e}")),
        }
    }
}

#[cfg(test)]
mod tests {
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

    /// One request through `handle` over an in-memory pipe: the status and the body it answered.
    async fn ask(request: Vec<u8>) -> (u16, String) {
        use tokio::io::AsyncWriteExt;
        let (mut client, server) = tokio::io::duplex(1 << 16);
        let deadline = tokio::time::Instant::now() + Duration::from_secs(5);
        let served = tokio::spawn(super::handle(server, "192.0.2.50".to_string(), deadline));
        client.write_all(&request).await.unwrap();
        let mut answer = Vec::new();
        client.read_to_end(&mut answer).await.unwrap();
        served.await.unwrap();
        let text = String::from_utf8_lossy(&answer).to_string();
        let status = text.get(9..12).and_then(|s| s.parse().ok()).unwrap_or(0);
        (status, text.split("\r\n\r\n").nth(1).unwrap_or("").to_string())
    }

    fn post(path: &str, body: &[u8]) -> Vec<u8> {
        let mut r = format!("POST {path} HTTP/1.1\r\nHost: x\r\nContent-Length: {}\r\n\r\n", body.len()).into_bytes();
        r.extend_from_slice(body);
        r
    }

    #[tokio::test]
    async fn a_post_is_answered_and_its_limits_hold() {
        let identity = format!("gen:{}", "ab01cd23".repeat(8));
        let host = serde_json::json!({"name": "t", "game": "0.9.0n", "proto": 216, "identity": identity,
                                      "conn": "direct", "direct_port": 7777});
        let (status, body) = ask(post("/v1/host", host.to_string().as_bytes())).await;
        assert_eq!(status, 200, "a direct host announce was refused: {body}");
        let lobby: serde_json::Value = serde_json::from_str(&body).unwrap();
        let id = lobby["lobbyId"].as_str().expect("no lobbyId").to_string();
        let (status, list) = ask(b"GET /v1/lobbies HTTP/1.1\r\nHost: x\r\n\r\n".to_vec()).await;
        assert!(status == 200 && list.contains(&id), "the new lobby is not listed");
        let (status, list) = ask(b"GET /v1/lobbies?version=zzz HTTP/1.1\r\nHost: x\r\n\r\n".to_vec()).await;
        assert!(status == 200 && list.contains(&id), "a query string changed the list");

        let over = vec![b' '; super::MAX_BODY + 1];
        assert_eq!(ask(post("/v1/host", &over)).await.0, 413, "a body past the cap was read");
        assert_eq!(ask(post("/v1/host", b"{nope")).await.0, 400);
        assert_eq!(ask(b"GET / HTTP/1.1\r\nA: \xff\r\n\r\n".to_vec()).await.0, 400, "a non-text head was read");
    }

    #[tokio::test]
    async fn the_challenge_route_answers_a_nonce() {
        let (status, body) = ask(post("/v1/host/challenge", b"{}")).await;
        assert_eq!(status, 200, "the challenge was refused: {body}");
        let answer: serde_json::Value = serde_json::from_str(&body).unwrap();
        let nonce = answer["nonce"].as_str().expect("no nonce");
        assert!(nonce.len() == 64 && nonce.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)));
    }

    #[tokio::test]
    async fn a_post_body_is_read_to_its_declared_length_and_must_be_an_object() {
        let leave = |head_tail: &str, body: &[u8]| {
            let mut r = format!("POST /v1/leave HTTP/1.1\r\nHost: x\r\n{head_tail}\r\n").into_bytes();
            r.extend_from_slice(body);
            r
        };
        assert_eq!(ask(leave("", b"")).await.0, 413, "a POST without a Content-Length was read");
        // An empty body is an empty object: the leave handler answers it as it answers `{}`.
        let (empty, _) = ask(leave("Content-Length: 0\r\n", b"")).await;
        assert_eq!(empty, 403, "an empty body was not treated as an empty object");
        // Bytes past the declared length are not part of the body.
        let (cut, _) = ask(leave("Content-Length: 2\r\n", b"{}trailing junk")).await;
        assert_eq!(cut, empty, "bytes past the Content-Length reached the parser");
        assert_eq!(ask(leave("Content-Length: 2\r\n", b"[]")).await.0, 400, "a JSON array was taken as a body");
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
    async fn a_silent_plaintext_client_is_dropped_at_the_deadline() {
        static POOL: LazyLock<Pool> = LazyLock::new(|| Pool::new("test", 8, 8));
        let (l, addr) = listen().await;
        tokio::spawn(serve_plain(l, &POOL, Duration::from_millis(400)));
        let mut c = TcpStream::connect(addr).await.unwrap();
        assert!(!closed_within(&mut c, Duration::from_millis(100)).await, "dropped before its deadline");
        assert!(closed_within(&mut c, Duration::from_secs(3)).await, "a silent client was held past the deadline");
    }

    #[tokio::test]
    async fn one_address_over_its_cap_is_dropped_before_any_handshake() {
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
