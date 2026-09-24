// coop/net/ice_policy.cpp -- see coop/net/ice_policy.h.

#include "coop/net/ice_policy.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "ue_wrap/core/log.h"

namespace coop::net {
namespace {

// The refused value as a detail shows it: whatever the ini or the environment held, so printable
// ASCII only and capped, since it lands in a dialog and a log.
std::string Printable(const std::string& raw) {
    constexpr size_t kMax = 32;
    std::string s;
    for (const char c : raw) {
        if (s.size() == kMax) { s += "..."; break; }
        s += (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    return s;
}

// What an unreadable policy's refusal names: the file, or the value and where it came from.
std::string UnreadableDetail(const IcePolicy& p) {
    if (p.fileUnreadable) return p.origin + " " + p.fileFault;
    return "net.ice=" + Printable(p.refused) + " from " + p.origin;
}

}  // namespace

IcePolicy ResolveIcePolicy() {
    static const bool kSelftestOk = RunIcePolicySelftest();
    (void)kSelftestOk;
    IcePolicy p;
    std::string token;
    coop::config::IniFault fault = coop::config::IniFault::None;
    const coop::config::FailClosedRead read = coop::config::ResolveFailClosed(
        coop::config_registry::rows::net_ice, token, &p.refused, &p.origin, &fault);
    p.readable = read == coop::config::FailClosedRead::Value;
    p.fileUnreadable = read == coop::config::FailClosedRead::Unreadable;
    if (p.fileUnreadable) p.fileFault = coop::config::IniFaultWords(fault);
    p.relayOnly = p.readable && token == "relay";
    return p;
}

Refusal IcePolicyRefusal(const IcePolicy& policy, const Config& cfg) {
    const bool p2p = cfg.topology == Topology::P2P;
    const bool directDial = !p2p && cfg.role == Role::Client;
    if (!p2p && !directDial) return {};  // a direct listen
    if (!policy.readable) return {EndReason::IcePolicyUnreadable, UnreadableDetail(policy)};
    if (!policy.relayOnly) return {};
    if (directDial) return {EndReason::RelayRefusesDirect, {}};
    if (cfg.turnList.empty())
        return {EndReason::RelayWithoutServer,
                "no TURN server in this session: a lobby's comes from its master, a dial with no "
                "master uses net.turn"};
    return {};
}

bool RunIcePolicySelftest() {
    enum Pol { Unreadable, All, Relay };
    struct Case {
        const char* what;
        Pol         pol;
        Topology    topology;
        Role        role;
        const char* turn;
        EndReason   want;
    };
    constexpr auto P2P = Topology::P2P, Lan = Topology::LanDirect;
    constexpr auto H = Role::Host, C = Role::Client;
    constexpr EndReason kNone = EndReason::None, kJ28 = EndReason::IcePolicyUnreadable,
                        kJ29 = EndReason::RelayWithoutServer, kJ30 = EndReason::RelayRefusesDirect;
    // Positives for every refusal and the negatives that stop a verdict answering one code for
    // everything: `all` with no TURN server, relay-only with one, and a direct listen under both.
    // A direct dial is refused whether or not a TURN server is known, so the order of the two
    // relay checks is pinned both ways.
    static const Case kCases[] = {
        {"unreadable p2p host",          Unreadable, P2P, H, "turn:t:3478", kJ28},
        {"unreadable p2p join",          Unreadable, P2P, C, "turn:t:3478", kJ28},
        {"unreadable direct dial",       Unreadable, Lan, C, "",            kJ28},
        {"unreadable direct host",       Unreadable, Lan, H, "",            kNone},
        {"relay p2p host no turn",       Relay,      P2P, H, "",            kJ29},
        {"relay p2p join no turn",       Relay,      P2P, C, "",            kJ29},
        {"relay p2p host turn",          Relay,      P2P, H, "turn:t:3478", kNone},
        {"relay p2p join turn",          Relay,      P2P, C, "turn:t:3478", kNone},
        {"relay direct dial turn",       Relay,      Lan, C, "turn:t:3478", kJ30},
        {"relay direct dial no turn",    Relay,      Lan, C, "",            kJ30},
        {"relay direct host",            Relay,      Lan, H, "",            kNone},
        {"all p2p join no turn",         All,        P2P, C, "",            kNone},
        {"all direct dial",              All,        Lan, C, "",            kNone},
    };
    int pass = 0, total = 0;
    for (const Case& c : kCases) {
        ++total;
        IcePolicy p;
        p.readable = c.pol != Unreadable;
        p.relayOnly = c.pol == Relay;
        if (!p.readable) { p.refused = "disable"; p.origin = "multivoid.ini"; }
        Config cfg;
        cfg.topology = c.topology;
        cfg.role = c.role;
        cfg.turnList = c.turn;
        const Refusal got = IcePolicyRefusal(p, cfg);
        if (got.code == c.want) { ++pass; continue; }
        UE_LOGW("ice-policy selftest: '%s' -> %s, expected %s", c.what,
                got.code == kNone ? "none" : Describe(got.code).id,
                c.want == kNone ? "none" : Describe(c.want).id);
    }
    // The detail says what was refused and where it came from, as a dialog can show it: bytes
    // outside printable ASCII replaced and the length capped, whatever the ini held; an empty
    // value named as one; an unreadable file named as such rather than as a value, with why, in
    // the words the config layer gives each fault.
    struct Detail {
        const char* what;
        bool fileUnreadable;
        std::string refused, origin, fault, want;
    };
    using coop::config::IniFault;
    using coop::config::IniFaultWords;
    const Detail kDetails[] = {
        {"a refused value, capped", false, std::string("re\xC3lay") + std::string(40, 'x'),
         "multivoid.ini", "", "net.ice=re?lay" + std::string(26, 'x') + "... from multivoid.ini"},
        {"an empty value", false, "", "multivoid.ini", "", "net.ice= from multivoid.ini"},
        {"an unreadable ini", true, "", "multivoid.ini", IniFaultWords(IniFault::ReadFailed),
         "multivoid.ini could not be read"},
        {"an ini that is not text", true, "", "multivoid.ini", IniFaultWords(IniFault::NotText),
         "multivoid.ini is not UTF-8 text (saved as UTF-16?)"},
    };
    for (const Detail& d : kDetails) {
        ++total;
        IcePolicy p;
        p.readable = false;
        p.fileUnreadable = d.fileUnreadable;
        p.refused = d.refused;
        p.origin = d.origin;
        p.fileFault = d.fault;
        Config cfg;
        cfg.topology = Topology::P2P;
        const Refusal got = IcePolicyRefusal(p, cfg);
        if (got.detail == d.want) { ++pass; continue; }
        UE_LOGW("ice-policy selftest: %s printed as '%s', expected '%s'", d.what,
                got.detail.c_str(), d.want.c_str());
    }
    const bool ok = (pass == total);
    if (ok) UE_LOGI("ice-policy selftest: PASS (%d/%d cases)", pass, total);
    else    UE_LOGE("ice-policy selftest: FAIL (%d/%d cases)", pass, total);
    return ok;
}

}  // namespace coop::net
