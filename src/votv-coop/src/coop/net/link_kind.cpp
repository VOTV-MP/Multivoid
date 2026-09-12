// coop/net/link_kind.cpp -- the link classifier: which kind of link a peer is on, measured
// from its connection (the relay flag, the loopback flag, the address class) and never asserted
// from config, with the selftest that covers the two kinds no drill on one box can reach.
// Session::LinkKindForSlot is defined here beside the classifier it wraps. See
// coop/net/link_kind.h for the question the kind answers.

#include "coop/net/link_kind.h"

#include "coop/net/session.h"
#include "ue_wrap/core/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#pragma warning(pop)

namespace coop::net {

// True for an address reachable only inside a local network: loopback or an RFC1918 range.
// GetIPv4() returns host byte order, so the ranges are literals; a real IPv6 peer yields 0 and
// falls through to "not private".
static bool IsPrivateAddress(const SteamNetworkingIPAddr& addr) {
    if (addr.IsLocalHost()) return true;
    const uint32 v4 = addr.GetIPv4();
    if (v4 == 0) return false;                                  // not IPv4-mapped
    if ((v4 & 0xFF000000u) == 0x0A000000u) return true;         // 10.0.0.0/8
    if ((v4 & 0xFFF00000u) == 0xAC100000u) return true;         // 172.16.0.0/12
    if ((v4 & 0xFFFF0000u) == 0xC0A80000u) return true;         // 192.168.0.0/16
    if ((v4 & 0xFF000000u) == 0x7F000000u) return true;         // 127.0.0.0/8
    if ((v4 & 0xFFFF0000u) == 0xA9FE0000u) return true;         // 169.254.0.0/16 link-local
    return false;
}

// The classifier, split from the connection fetch so the self-test can run it over synthetic
// addresses. Order is load-bearing: GNS leaves m_addrRemote all zero on paths that are not plain
// direct UDP, and an address test alone would read a same-LAN ICE peer as Direct ("public, no
// relay"), so an absent address is answered from GNS's own flags, and with none, Unknown.
static LinkKind ClassifyLink(int infoFlags, const SteamNetworkingIPAddr& addr) {
    // Relay first: a relayed path's remote address is the relay's.
    if (infoFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) return LinkKind::Relayed;
    // Loopback buffers are same-process by definition.
    if (infoFlags & k_nSteamNetworkConnectionInfoFlags_LoopbackBuffers) return LinkKind::Lan;
    if (addr.IsIPv6AllZeros()) {
        // No address: GNS's Fast bit means "internal, localhost or the same LAN", a measurement
        // where we have none; absent that, Unknown.
        return (infoFlags & k_nSteamNetworkConnectionInfoFlags_Fast) ? LinkKind::Lan
                                                                     : LinkKind::Unknown;
    }
    return IsPrivateAddress(addr) ? LinkKind::Lan : LinkKind::Direct;
}

bool RunLinkClassifySelftest() {
    // No port column: ClassifyLink never reads m_port.
    struct Case { const char* what; const char* ip; int flags; LinkKind want; };
    // Known positives and known negatives; the negatives stop a classifier that answers one value
    // for everything.
    static const Case kCases[] = {
        {"loopback v4",        "127.0.0.1", 0, LinkKind::Lan},
        {"rfc1918 10/8",       "10.0.0.5", 0, LinkKind::Lan},
        {"rfc1918 172.16/12",  "172.16.4.9", 0, LinkKind::Lan},
        {"rfc1918 192.168/16", "192.168.1.50", 0, LinkKind::Lan},
        {"link-local",         "169.254.7.7", 0, LinkKind::Lan},
        // 172.32 is outside 172.16/12 and 11.x outside 10/8, the classic off-by-a-mask mistakes;
        // both must read Direct.
        {"public 8.8.8.8",     "8.8.8.8", 0, LinkKind::Direct},
        {"public 172.32.0.1",  "172.32.0.1", 0, LinkKind::Direct},
        {"public 11.0.0.1",    "11.0.0.1", 0, LinkKind::Direct},
        // A real IPv6 peer: GetIPv4() returns 0, which must not read as 0.0.0.0 and private.
        {"public v6",          "2606:4700::1111", 0, LinkKind::Direct},
        {"v6 loopback",        "::1", 0, LinkKind::Lan},
        // The relay flag wins over any address, a private one included.
        {"relayed public",     "8.8.8.8",
             k_nSteamNetworkConnectionInfoFlags_Relayed, LinkKind::Relayed},
        {"relayed private",    "192.168.1.50",
             k_nSteamNetworkConnectionInfoFlags_Relayed, LinkKind::Relayed},
        // No address: GNS leaves m_addrRemote all zero off plain direct UDP; these three pin the
        // fallback ladder.
        {"no addr, no flags",  "::",              0, LinkKind::Unknown},
        {"no addr, Fast",      "::",
             k_nSteamNetworkConnectionInfoFlags_Fast, LinkKind::Lan},
        {"loopback buffers",   "::",
             k_nSteamNetworkConnectionInfoFlags_LoopbackBuffers, LinkKind::Lan},
    };
    int pass = 0, total = 0;
    for (const Case& c : kCases) {
        ++total;
        SteamNetworkingIPAddr addr{};
        addr.Clear();
        if (!addr.ParseString(c.ip)) {
            UE_LOGW("link-classify selftest: '%s' did not parse -- case '%s' SKIPPED as FAIL",
                    c.ip, c.what);
            continue;
        }
        const LinkKind got = ClassifyLink(c.flags, addr);
        if (got == c.want) { ++pass; continue; }
        UE_LOGW("link-classify selftest: '%s' (%s flags=0x%x) -> %d, expected %d",
                c.what, c.ip, static_cast<unsigned>(c.flags),
                static_cast<int>(got), static_cast<int>(c.want));
    }
    const bool ok = (pass == total);
    if (ok) UE_LOGI("link-classify selftest: PASS (%d/%d cases)", pass, total);
    else    UE_LOGE("link-classify selftest: FAIL (%d/%d cases)", pass, total);
    return ok;
}

LinkKind Session::LinkKindForSlot(int peerSlot) const {
    // Every kind is measured from the connection: cfg_.topology says how it was established, not
    // how the peer is connected (it labelled a port-forwarded WAN peer LAN), and the relay fact is
    // a documented bit, not a substring of the description string.
    if (peerSlot < 0 || peerSlot >= kMaxPeers) return LinkKind::Unknown;
    const uint32_t hConn = peerConns_[peerSlot].load();
    if (hConn == 0) return LinkKind::Unknown;
    auto* sockets = SteamNetworkingSockets();
    if (!sockets) return LinkKind::Unknown;
    SteamNetConnectionInfo_t info{};
    if (!sockets->GetConnectionInfo(static_cast<HSteamNetConnection>(hConn), &info))
        return LinkKind::Unknown;
    return ClassifyLink(info.m_nFlags, info.m_addrRemote);
}

}  // namespace coop::net
