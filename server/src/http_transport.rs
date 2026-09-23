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
pub const MAX_BODY: usize = 64 * 1024;
pub const MAX_CONNS: usize = 256;
/// Per source address. A client holds at most a few short requests at once (a list refresh, a
/// heartbeat, a join), so 16 leaves room for a household or a carrier NAT while keeping one source
/// from holding the whole pool.
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

/// Read the header block up to (and consuming) the terminating CRLFCRLF, bounded by
/// `max`. Returns (head_bytes, leftover_body_bytes_already_read). Mirrors the Python
/// `readuntil(b"\r\n\r\n")` with the start_server `limit`.
pub async fn read_head<S: AsyncRead + Unpin>(
    stream: &mut S,
    max: usize,
) -> Result<(Vec<u8>, Vec<u8>), HeadErr> {
    let mut buf: Vec<u8> = Vec::with_capacity(2048);
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
        if buf.len() > max {
            return Err(HeadErr::TooLarge);
        }
        from = buf.len().saturating_sub(3);
        let n = stream.read(&mut tmp).await.map_err(|_| HeadErr::Closed)?;
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

#[cfg(test)]
mod tests {
    use super::{read_head, HeadErr};
    use tokio::io::AsyncWriteExt;

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
