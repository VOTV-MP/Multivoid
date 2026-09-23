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


use coop_server::admission::Pool;
use coop_server::common::{clamp_str, env_int, env_str, log};
use coop_server::http_transport::{
    json_bytes, read_head, write_response, HeadErr, CONNS, HTTP_TIMEOUT, MAX_BODY, MAX_HEADER,
};
use coop_server::lobby::{
    dispatch_post, filter_lobbies, lobbies_snapshot, lock_state, resolve_client_ip, sweeper,
    LATEST_MOD, LATEST_PROTO, LATEST_URL, MAX_VERSION,
};
use coop_server::master_config::CFG;
use coop_server::thanks::{thanks_answer, Answer};
use coop_server::tls;
use serde_json::{json, Value};
use std::sync::LazyLock;
use std::time::Duration;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite};
use tokio::time::{timeout_at, Instant};
use std::collections::HashMap;
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

    let head_str = String::from_utf8_lossy(&head);
    let mut lines = head_str.split("\r\n");
    let request_line = lines.next().unwrap_or("");
    let mut rl = request_line.splitn(3, ' ');
    let (method, raw_path) = match (rl.next(), rl.next(), rl.next()) {
        (Some(m), Some(p), Some(_)) => (m, p),
        _ => {
            write_response(&mut stream, 400, &json_bytes(&json!({"error": "bad request line"}))).await;
            return;
        }
    };

    let mut headers: HashMap<String, String> = HashMap::new();
    for ln in lines {
        if ln.is_empty() {
            continue;
        }
        if let Some((k, v)) = ln.split_once(':') {
            headers.insert(k.trim().to_lowercase(), v.trim().to_string());
        }
    }

    let (path, query) = match raw_path.split_once('?') {
        Some((p, q)) => (p, q),
        None => (raw_path, ""),
    };
    let client_ip = resolve_client_ip(&peer_ip, &headers);

    // ---- body (POST only, bounded) ----
    let mut body_obj = Value::Object(serde_json::Map::new());
    if method == "POST" {
        let clen: i64 = headers
            .get("content-length")
            .and_then(|s| s.trim().parse::<i64>().ok())
            .unwrap_or(-1);
        if clen < 0 || clen as usize > MAX_BODY {
            write_response(&mut stream, 413, &json_bytes(&json!({"error": "body too large"}))).await;
            return;
        }
        let clen = clen as usize;
        let mut raw = leftover;
        if raw.len() < clen {
            let need = clen - raw.len();
            let mut rest = vec![0u8; need];
            match timeout_at(deadline, stream.read_exact(&mut rest)).await {
                Ok(Ok(_)) => raw.extend_from_slice(&rest),
                _ => return, // eof / timeout
            }
        }
        raw.truncate(clen);
        if !raw.is_empty() {
            match serde_json::from_slice::<Value>(&raw) {
                Ok(v) if v.is_object() => body_obj = v,
                _ => {
                    write_response(&mut stream, 400, &json_bytes(&json!({"error": "bad json"}))).await;
                    return;
                }
            }
        }
    }

    // ---- route ----
    if method == "GET" && path == "/v1/lobbies" {
        let mut vf = String::new();
        for kv in query.split('&') {
            if let Some(rest) = kv.strip_prefix("version=") {
                vf = clamp_str(rest, MAX_VERSION);
            }
        }
        // Refresh + snapshot under the lock; serialize the response OFF the lock (L5).
        let (all_body, rows) = {
            let mut state = lock_state();
            lobbies_snapshot(&mut state)
        };
        if vf.is_empty() {
            write_response(&mut stream, 200, &all_body).await;
        } else {
            let body = filter_lobbies(&rows, &vf);
            write_response(&mut stream, 200, &body).await;
        }
    } else if method == "GET" && path == "/v1/latest" {
        // Env-overridable release info (resolved once; see the LATEST_* comment above).
        static LATEST: LazyLock<(i64, String, String)> = LazyLock::new(|| {
            (
                env_int("COOP_LATEST_PROTO", LATEST_PROTO),
                env_str("COOP_LATEST_MOD", LATEST_MOD),
                env_str("COOP_LATEST_URL", LATEST_URL),
            )
        });
        let (proto, mod_str, url) = &*LATEST;
        write_response(
            &mut stream,
            200,
            &json_bytes(&json!({"proto": proto, "mod": mod_str, "url": url})),
        )
        .await;
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
    } else if method == "POST" {
        match dispatch_post(path, &client_ip, &body_obj) {
            Some((status, resp)) => write_response(&mut stream, status, &json_bytes(&resp)).await,
            None => write_response(&mut stream, 404, &json_bytes(&json!({"error": "not found"}))).await,
        }
    } else {
        write_response(&mut stream, 404, &json_bytes(&json!({"error": "not found"}))).await;
    }
}

#[tokio::main]
async fn main() {
    // fail fast on missing secrets (same as the Python FATAL exits)
    if CFG.turn_secret.is_empty() {
        log("FATAL: COOP_TURN_SECRET not set -- refusing to mint TURN creds");
        std::process::exit(1);
    }
    if CFG.signaling_token.is_empty() {
        log("FATAL: COOP_SIGNALING_TOKEN not set -- clients could not reach signaling");
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
        "master listening on {addr} (signaling={} stun={} turn={})",
        if CFG.signaling_url.is_empty() { "?" } else { &CFG.signaling_url },
        if CFG.stun_uri.is_empty() { "?" } else { &CFG.stun_uri },
        if CFG.turn_uri.is_empty() { "off" } else { "on" }
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
                let Some(slot) = pool.admit_or_log(&peer_ip) else { continue };
                let deadline = Instant::now() + budget;
                let acceptor = acceptor.clone();
                tokio::spawn(async move {
                    let _slot = slot;
                    match timeout_at(deadline, acceptor.accept(stream)).await {
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
