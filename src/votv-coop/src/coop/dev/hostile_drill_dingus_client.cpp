// coop/dev/hostile_drill_dingus_client.cpp -- the dingus_report arm's sender: see coop/dev/hostile_drill_dingus.h. The client
// waits for the host's two key lines, then sends thirteen raw DingusReport messages through the session's own send, bypassing
// the dingus lane, in one tick and in the table's order, and says it is done.

#include "hostile_drill_dingus_detail.h"  // co-located private header (src tree, not include/)

#include "coop/dev/drill_chat.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/log.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace coop::dev::hostile_drill::dingus::detail {
namespace {

namespace DC = coop::dev::drill_chat;
using Kind = coop::net::ReliableKind;
using Payload = coop::net::DingusReportPayload;

enum class CStep : uint8_t { Run, Done };
CStep             g_step = CStep::Run;
bool              g_idleSaid = false, g_historyOn = false;
Clock::time_point g_since{};
bool              g_waiting = false;

void Inconclusive(const Spec& sp, const std::string& why) {
    Verdict(sp.arm, "INCONCLUSIVE", why);
    g_step = CStep::Done;
}

// True once the wait for the host's key lines has run past the bound.
bool Late() {
    if (!g_waiting) {
        g_waiting = true;
        g_since = Clock::now();
    }
    return Clock::now() - g_since > kBound;
}

bool OwnJoinOver() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

std::wstring Widen(const std::string& ascii) { return std::wstring(ascii.begin(), ascii.end()); }

// One forged run, counting what it sent; a send that fails stops the run.
struct Out {
    coop::net::Session* s;
    uint32_t            n = 0;
    bool                ok = true;
    void Send(const void* p, int len) {
        ok = ok && s->SendReliable(Kind::DingusReport, p, len);
        ++n;
    }
};

Payload With(const std::string& key) {
    Payload p{};
    coop::net::WireKeyFromString(Widen(key), p.key);
    return p;
}

// The arm's reports, in the table's order: three the dispatcher refuses, one on the plain prop, then the far dingus's key once
// and eight times more.
void SendArm(Out& o, const std::string& plain, const std::string& farKey) {
    Payload empty{};
    o.Send(&empty, sizeof(empty));                       // key.len 0
    Payload longKey{};
    std::memset(longKey.key.data, 'x', sizeof(longKey.key.data));
    longKey.key.len = static_cast<uint8_t>(sizeof(longKey.key.data) + 1);   // a key longer than the wire holds
    o.Send(&longKey, sizeof(longKey));
    uint8_t shortPayload[20];
    std::memset(shortPayload, 'x', sizeof(shortPayload));
    o.Send(shortPayload, sizeof(shortPayload));          // the wrong length
    const Payload onPlain = With(plain);
    o.Send(&onPlain, sizeof(onPlain));
    const Payload onFar = With(farKey);
    for (int i = 0; i < 9; ++i) o.Send(&onFar, sizeof(onFar));
}

}  // namespace

void ClientTick(coop::net::Session* s, const Spec& sp) {
    if (g_step == CStep::Done) return;
    // The first read starts the chat history, so a key line the host says before this client's join is over is still seen.
    if (!g_historyOn) {
        g_historyOn = true;
        DC::SeenHas(DoneLine(sp.arm));
    }
    if (!OwnJoinOver()) return;
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    if (!coop::players::Registry::Get().Local() || self == 0 || self >= coop::players::kMaxPeers) return;
    if (self != kSlot) {
        if (!g_idleSaid) {
            g_idleSaid = true;
            UE_LOGI("[HOSTILE-DRILL] %s idles: this client is slot %u, the family acts on slot %u", sp.name,
                    static_cast<unsigned>(self), static_cast<unsigned>(kSlot));
        }
        g_step = CStep::Done;
        return;
    }
    std::string plain, farKey;
    if (!DC::SeenRestOf(KeyLine(sp.arm, "plain"), &plain) || !DC::SeenRestOf(KeyLine(sp.arm, "far"), &farKey)) {
        if (Late()) Inconclusive(sp, "the host never said the fixtures' keys");
        return;
    }
    Out o{s};
    SendArm(o, plain, farKey);
    if (!o.ok || o.n != sp.sent) {
        Inconclusive(sp, "client: a forged message was not sent");
        return;
    }
    UE_LOGI("[HOSTILE-DRILL] %s sent %u messages", sp.name, static_cast<unsigned>(o.n));
    // The done line leaves after the thirteen on the one ordered lane: the host judges when it reads it.
    DC::Say(DoneLine(sp.arm));
    g_waiting = false;
    g_step = CStep::Done;
}

void ClientReset() {
    g_step = CStep::Run;
    g_idleSaid = g_historyOn = false;
    g_since = {};
    g_waiting = false;
}

}  // namespace coop::dev::hostile_drill::dingus::detail
