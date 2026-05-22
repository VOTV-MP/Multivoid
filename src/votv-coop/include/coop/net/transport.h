// coop/net/transport.h -- pure UDP I/O (Phase 3, methodology 3.3).
//
// "It sends bytes, receives bytes. It does NOT know what game data is."
// A thin Winsock UDP wrapper: one socket, bind, sendto, recvfrom. No threads,
// no peer tracking, no protocol -- the Session (application layer) owns all of
// that and pumps this from its own net thread.
//
// Engine-independent: nothing here touches UObjects or the game thread.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {

// An opaque resolved peer address (IPv4 host:port). Built by Transport::Resolve.
struct Endpoint {
    uint32_t addrBE = 0;  // IPv4 address, network byte order
    uint16_t portBE = 0;  // port, network byte order
    bool valid() const { return portBE != 0; }
    bool operator==(const Endpoint& o) const { return addrBE == o.addrBE && portBE == o.portBE; }
};

class Transport {
public:
    Transport() = default;
    ~Transport();
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    // Create a UDP socket bound to `bindPort` (0 = any ephemeral port, for a
    // client that only ever sends). Non-blocking. Returns false on failure.
    bool Open(uint16_t bindPort);
    void Close();
    bool IsOpen() const { return sock_ != ~0ull; }

    // Resolve "a.b.c.d" + port into an Endpoint (numeric IPv4 only -- LAN, no DNS).
    static Endpoint Resolve(const std::string& ipv4, uint16_t port);

    // Send `len` bytes to `to`. Returns true if the datagram was handed to the OS.
    bool SendTo(const Endpoint& to, const void* data, int len);

    // Receive one pending datagram into `buf` (capacity `cap`). Returns the byte
    // count (>0) and fills `from` with the sender; 0 if nothing is pending
    // (would-block); -1 on a real error. Non-blocking -- call it in a drain loop.
    int RecvFrom(void* buf, int cap, Endpoint& from);

private:
    unsigned long long sock_ = ~0ull;  // SOCKET (UINT_PTR); ~0 == INVALID_SOCKET
};

// Process-wide Winsock init/teardown (refcounted). Open() calls Startup; the last
// Close() calls Cleanup. Exposed for tests that resolve before opening.
bool WinsockStartup();
void WinsockCleanup();

}  // namespace coop::net
