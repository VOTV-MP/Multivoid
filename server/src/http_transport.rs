//! The master's HTTP plumbing: the request head, the response, the connection cap. Nothing here
//! knows what a lobby is -- the routes live in the binary and the lobby domain in `lobby`.

use crate::admission::Pool;
use serde_json::Value;
use std::sync::LazyLock;
use std::time::Duration;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::time::timeout;

/// One budget per connection for everything before the answer: the TLS handshake, the head and
/// the body. The answer's write is bounded separately (`write_response`).
pub const HTTP_TIMEOUT: Duration = Duration::from_secs(15);
pub const MAX_HEADER: usize = 16 * 1024;
/// The largest body a client of ours sends is the host announce (coop/net/lobby_announcer.cpp):
/// about 2 KiB at its worst, a name at the native host window's 96-byte cap
/// (host_session_settings.cpp:59) fully escaped (576 bytes), plus the identity, the build claim
/// and the lobby proof's nonce and signature (about 450 hex digits, with their field names); the server then
/// clamps names to 63 characters (`lobby::MAX_NAME`).
/// 4 KiB leaves room and keeps a body's parse small: a `serde_json::Value` can cost 16x its bytes
/// (tests/conn_cost.rs: a 4 KiB array of zeros parses to 65 KiB).
pub const MAX_BODY: usize = 4 * 1024;
/// The pool, sized against the unit's 128 MiB. One connection holds at most ~87 KiB: the larger of
/// rustls' handshake buffer (64.8 KiB) and, after it, one record held in 32 KiB plus 16 KiB of
/// received plaintext, 16 KiB unsent (`tls::SEND_BUFFER_LIMIT`) and the 16 KiB head (80 KiB), plus
/// the task's ~7 KiB. 512 of them ~44 MiB; the connect history adds up to ~11 MiB at its setting's
/// ceiling (~6 MiB at the default; tests/conn_cost.rs), and the lobby maps, the rate map and the
/// list cache an estimated ~16 MiB: the master's worst case is ~71 MiB.
pub const MAX_CONNS: usize = 512;
/// Per source address. A browser open is three requests at once (lobbies, latest, thanks) and the
/// list refreshes every 5 s (`server_browser_native.cpp:123`), so a household of four needs 12;
/// 512 / 16 = 32 addresses to fill the pool.
pub const MAX_CONNS_PER_ADDR: u32 = 16;

/// Every connection the master holds, the ones still in their TLS handshake included.
pub static CONNS: LazyLock<Pool> =
    LazyLock::new(|| Pool::new("connection", MAX_CONNS, MAX_CONNS_PER_ADDR));

pub fn reason_phrase(status: u16) -> &'static str {
    match status {
        200 => "OK",
        400 => "Bad Request",
        403 => "Forbidden",
        404 => "Not Found",
        405 => "Method Not Allowed",
        413 => "Payload Too Large",
        429 => "Too Many Requests",
        500 => "Internal Server Error",
        503 => "Service Unavailable",
        _ => "OK",
    }
}

pub async fn write_response<S: AsyncWrite + Unpin>(stream: &mut S, status: u16, body: &[u8]) {
    let head = format!(
        "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
        status,
        reason_phrase(status),
        body.len()
    );
    // Bound the write (audit L6): a client that stops reading a large /v1/lobbies body
    // must not pin its CONNS admission slot until the OS TCP timeout. Drop on expiry.
    let _ = timeout(HTTP_TIMEOUT, async {
        stream.write_all(head.as_bytes()).await?;
        stream.write_all(body).await?;
        stream.flush().await
    })
    .await;
}

pub fn json_bytes(v: &Value) -> Vec<u8> {
    serde_json::to_vec(v).unwrap_or_else(|_| b"{}".to_vec())
}

pub fn find_subsequence(hay: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || hay.len() < needle.len() {
        return None;
    }
    hay.windows(needle.len()).position(|w| w == needle)
}

/// Read the header block up to (and consuming) the terminating CRLFCRLF, which must end within
/// `max + 4` bytes. Returns (head_bytes, leftover_body_bytes_already_read). The buffer is sized
/// once and no read goes past it, so a head costs `max + 4` bytes at most, never a grown copy.
pub async fn read_head<S: AsyncRead + Unpin>(
    stream: &mut S,
    max: usize,
) -> Result<(Vec<u8>, Vec<u8>), HeadErr> {
    let cap = max + 4;
    let mut buf: Vec<u8> = Vec::with_capacity(cap);
    let mut tmp = [0u8; 4096];
    // Search only what arrived since the last look, less the terminator's length so one split
    // across two reads is still found. Rescanning the whole buffer made a head trickled a byte
    // at a time cost quadratic CPU: 43 ms for 16 KiB, per connection.
    let mut from = 0;
    loop {
        if let Some(i) = find_subsequence(&buf[from..], b"\r\n\r\n").map(|i| i + from) {
            let leftover = buf[i + 4..].to_vec();
            buf.truncate(i);
            return Ok((buf, leftover));
        }
        let room = (cap - buf.len()).min(tmp.len());
        if room == 0 {
            return Err(HeadErr::TooLarge);
        }
        from = buf.len().saturating_sub(3);
        let n = stream.read(&mut tmp[..room]).await.map_err(|_| HeadErr::Closed)?;
        if n == 0 {
            return Err(HeadErr::Closed);
        }
        buf.extend_from_slice(&tmp[..n]);
    }
}

pub enum HeadErr {
    TooLarge,
    Closed,
}

/// The parts of a request head the master reads. Every other header line is skipped as it is
/// parsed and nothing is kept for it, so a head costs its own bytes however many lines it holds.
pub struct Head<'a> {
    pub method: &'a str,
    pub path: &'a str,
    pub content_length: Option<&'a str>,
    pub x_real_ip: Option<&'a str>,
    pub x_forwarded_for: Option<&'a str>,
}

pub enum HeadReject {
    /// Not UTF-8: an HTTP head from a client of ours is ASCII.
    NotText,
    BadRequestLine,
}

/// Parse a head `read_head` returned. A repeated header keeps its last value.
pub fn parse_head(head: &[u8]) -> Result<Head<'_>, HeadReject> {
    let text = std::str::from_utf8(head).map_err(|_| HeadReject::NotText)?;
    let mut lines = text.split("\r\n");
    let mut rl = lines.next().unwrap_or("").splitn(3, ' ');
    let (method, raw_path) = match (rl.next(), rl.next(), rl.next()) {
        (Some(m), Some(p), Some(_)) => (m, p),
        _ => return Err(HeadReject::BadRequestLine),
    };
    let path = raw_path.split_once('?').map_or(raw_path, |(p, _)| p);
    let mut h = Head { method, path, content_length: None, x_real_ip: None, x_forwarded_for: None };
    for ln in lines {
        let Some((k, v)) = ln.split_once(':') else { continue };
        let k = k.trim();
        let slot = if k.eq_ignore_ascii_case("content-length") {
            &mut h.content_length
        } else if k.eq_ignore_ascii_case("x-real-ip") {
            &mut h.x_real_ip
        } else if k.eq_ignore_ascii_case("x-forwarded-for") {
            &mut h.x_forwarded_for
        } else {
            continue;
        };
        *slot = Some(v.trim());
    }
    Ok(h)
}

#[cfg(test)]
mod tests {
    use super::{parse_head, read_head, HeadErr, HeadReject};
    use tokio::io::AsyncWriteExt;

    #[test]
    fn a_head_keeps_only_the_headers_the_master_reads() {
        let h = parse_head(b"POST /v1/host?x=1 HTTP/1.1\r\nContent-Length: 12\r\nX-Real-IP: 1.2.3.4\r\nA: b")
            .ok()
            .expect("a well-formed head was refused");
        assert_eq!((h.method, h.path), ("POST", "/v1/host"), "the query string stayed in the path");
        assert_eq!((h.content_length, h.x_real_ip, h.x_forwarded_for), (Some("12"), Some("1.2.3.4"), None));
        let again = parse_head(b"GET / HTTP/1.1\r\ncontent-length: 1\r\nCONTENT-LENGTH: 2").ok().unwrap();
        assert_eq!(again.content_length, Some("2"), "a repeated header did not keep its last value");
        // Thousands of tiny header lines cost nothing past the head's own bytes: nothing is kept.
        let mut many = b"GET / HTTP/1.1".to_vec();
        for _ in 0..4000 {
            many.extend_from_slice(b"\r\na:");
        }
        assert!(parse_head(&many).is_ok());
    }

    #[test]
    fn a_head_that_is_not_text_or_has_no_request_line_is_refused() {
        assert!(matches!(parse_head(b"GET / HTTP/1.1\r\nA: \xff\xfe"), Err(HeadReject::NotText)));
        assert!(matches!(parse_head(b"GET /"), Err(HeadReject::BadRequestLine)));
    }

    #[tokio::test]
    async fn a_head_never_reads_past_its_cap() {
        // One write far past the cap: the reader takes at most cap bytes, then refuses.
        let (mut tx, mut rx) = tokio::io::duplex(1 << 16);
        tx.write_all(&[b'a'; 40_000]).await.unwrap();
        assert!(matches!(read_head(&mut rx, 1024).await, Err(HeadErr::TooLarge)));
        let mut left = vec![0u8; 40_000];
        let n = tokio::io::AsyncReadExt::read(&mut rx, &mut left).await.unwrap();
        assert!(n >= 40_000 - 1028, "the reader consumed {} bytes past a 1028-byte cap", 40_000 - n - 1028);
    }

    #[tokio::test]
    async fn a_terminator_split_across_reads_is_found() {
        let (mut tx, mut rx) = tokio::io::duplex(64);
        // The writer ends after the second part, so a missed terminator reads as EOF.
        tokio::spawn(async move {
            tx.write_all(b"GET / HTTP/1.1\r\nHost: x\r\n\r").await.unwrap();
            tokio::time::sleep(std::time::Duration::from_millis(20)).await;
            tx.write_all(b"\nBODY").await.unwrap();
        });
        let (head, rest) = read_head(&mut rx, 1024).await.ok().expect("the split terminator was missed");
        assert_eq!(head, b"GET / HTTP/1.1\r\nHost: x");
        assert_eq!(rest, b"BODY");
    }

    #[tokio::test]
    async fn a_head_past_the_cap_is_refused() {
        let (mut tx, mut rx) = tokio::io::duplex(4096);
        tx.write_all(&[b'a'; 200]).await.unwrap();
        assert!(matches!(read_head(&mut rx, 64).await, Err(HeadErr::TooLarge)));
    }
}
