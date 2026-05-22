#include "coop/net/transport.h"

#include "ue_wrap/log.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <mutex>

namespace coop::net {

namespace {

// Refcounted WSAStartup so multiple Transports (and tests) share one init.
std::mutex g_wsaMutex;
int g_wsaRefs = 0;

}  // namespace

bool WinsockStartup() {
    std::lock_guard<std::mutex> lk(g_wsaMutex);
    if (g_wsaRefs == 0) {
        WSADATA wsa{};
        const int rc = ::WSAStartup(MAKEWORD(2, 2), &wsa);
        if (rc != 0) {
            UE_LOGE("net: WSAStartup failed (%d)", rc);
            return false;
        }
    }
    ++g_wsaRefs;
    return true;
}

void WinsockCleanup() {
    std::lock_guard<std::mutex> lk(g_wsaMutex);
    if (g_wsaRefs > 0 && --g_wsaRefs == 0) ::WSACleanup();
}

Transport::~Transport() { Close(); }

bool Transport::Open(uint16_t bindPort) {
    if (IsOpen()) return true;
    if (!WinsockStartup()) return false;

    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        UE_LOGE("net: socket() failed (%d)", ::WSAGetLastError());
        WinsockCleanup();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;        // bind all interfaces (LAN)
    addr.sin_port = ::htons(bindPort);        // 0 -> OS picks an ephemeral port
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        UE_LOGE("net: bind(port=%u) failed (%d)", bindPort, ::WSAGetLastError());
        ::closesocket(s);
        WinsockCleanup();
        return false;
    }

    // Non-blocking: RecvFrom must return immediately (WSAEWOULDBLOCK) so the net
    // thread can interleave sends + a sleep without parking on recv.
    u_long nb = 1;
    ::ioctlsocket(s, FIONBIO, &nb);

    sock_ = static_cast<unsigned long long>(s);
    UE_LOGI("net: UDP socket open (bindPort=%u)", bindPort);
    return true;
}

void Transport::Close() {
    if (!IsOpen()) return;
    ::closesocket(static_cast<SOCKET>(sock_));
    sock_ = ~0ull;
    WinsockCleanup();
}

Endpoint Transport::Resolve(const std::string& ipv4, uint16_t port) {
    Endpoint ep;
    in_addr a{};
    if (::inet_pton(AF_INET, ipv4.c_str(), &a) == 1) {
        ep.addrBE = a.s_addr;
        ep.portBE = ::htons(port);
    } else {
        UE_LOGW("net: Resolve('%s') is not a numeric IPv4 address", ipv4.c_str());
    }
    return ep;
}

bool Transport::SendTo(const Endpoint& to, const void* data, int len) {
    if (!IsOpen() || !to.valid()) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = to.addrBE;
    addr.sin_port = to.portBE;
    const int sent = ::sendto(static_cast<SOCKET>(sock_), static_cast<const char*>(data), len, 0,
                              reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return sent == len;
}

int Transport::RecvFrom(void* buf, int cap, Endpoint& from) {
    if (!IsOpen()) return -1;
    sockaddr_in addr{};
    int alen = sizeof(addr);
    const int n = ::recvfrom(static_cast<SOCKET>(sock_), static_cast<char*>(buf), cap, 0,
                             reinterpret_cast<sockaddr*>(&addr), &alen);
    if (n == SOCKET_ERROR) {
        const int err = ::WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return 0;   // nothing pending
        if (err == WSAECONNRESET) return 0;    // a prior sendto bounced (no listener yet) -- ignore
        return -1;
    }
    from.addrBE = addr.sin_addr.s_addr;
    from.portBE = addr.sin_port;
    return n;
}

}  // namespace coop::net
