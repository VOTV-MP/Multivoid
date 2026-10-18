// coop/net/http_client.h -- a tiny synchronous HTTP/1.1 client (WinHTTP).
//
// Used by the master-server lobby plane (lobby_client GET /v1/lobbies + POST
// /v1/join, lobby_announcer POST /v1/host + /v1/heartbeat + /v1/leave). WinHTTP
// ships with Windows -- no new dependency (RULE 3 standalone). The design's
// eventual WinHTTP-WebSocket signaling transport will reuse the same substrate.
//
// SYNCHRONOUS + BLOCKING: every call self-contains a WinHttpOpen..close (no shared
// handle -> thread-safe by construction). Callers MUST run requests on a worker
// thread, never the game/render thread -- a network round-trip would stall a frame.
// lobby_client / lobby_announcer own those worker threads.
//
// Plain HTTP only (the master listens on a spare port; the wss://:443 fronting is a
// later transport change). No proxy (a direct VPS/LAN IP must not be routed through
// a system proxy).

#pragma once

#include <cstddef>
#include <string>

namespace coop::net::http {

// How much of a response body is read when the caller names no cap: a master answer is small, so a
// rogue endpoint cannot grow our buffer without bound.
inline constexpr size_t kDefaultBodyCap = 1u << 20;

struct Response {
    bool ok = false;     // transport-level success (got a complete HTTP response)
    int status = 0;      // HTTP status code (200, 404, 429, ...); 0 if !ok
    std::string body;    // response body (the JSON)
};

// One request. `hostPort` is "host:port" (an optional "http://" prefix is stripped);
// `path` begins with '/'. `method` is "GET" or "POST". For POST, `body` is the JSON
// payload (Content-Type: application/json); for GET pass an empty body. `timeoutMs`
// bounds resolve+connect+send+receive each. `bodyCap` bounds the response body read (bytes past it
// are dropped and the answer is still ok); a caller whose answer can be larger than a master's
// usual one names its own. Never throws; on any failure returns {ok=false}.
Response Request(const std::string& hostPort, const std::string& path,
                 const char* method, const std::string& body, int timeoutMs,
                 size_t bodyCap = kDefaultBodyCap);

// The host part of `hostPort` as Request extracts it (the same extractor), in the form the master
// compares: ASCII lower-cased, port, scheme, login and one trailing dot stripped, IPv6 brackets
// kept. Empty when Request would refuse the string or the host is not ASCII. The rules and their
// cases are shared with the master (server/tests/vectors/master_names.json).
std::string HostName(const std::string& hostPort);

// Convenience wrappers.
inline Response Get(const std::string& hostPort, const std::string& path, int timeoutMs,
                    size_t bodyCap = kDefaultBodyCap) {
    return Request(hostPort, path, "GET", std::string(), timeoutMs, bodyCap);
}
inline Response Post(const std::string& hostPort, const std::string& path,
                     const std::string& body, int timeoutMs) {
    return Request(hostPort, path, "POST", body, timeoutMs);
}

}  // namespace coop::net::http
