// coop/interactables/desk_short_sync.cpp -- see coop/interactables/desk_short_sync.h.

#include "coop/interactables/desk_short_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/desk_short.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <vector>

namespace coop::desk_short_sync {
namespace {

namespace CD = ue_wrap::console_desk;
namespace DSh = ue_wrap::desk_short;
namespace sg = ue_wrap::script_gate;
using Payload = coop::net::DeskShortPayload;

// The desk's own event: a stub that runs the ubergraph from the entry that tests `shortCircuiting`, sets it and starts
// the ten seconds of sparks. A liquid's Blueprint calls it, and so do we.
UE_FACT_FN(kRowShort, "desk_short_sync", L"analogDScreenTest_C", L"shortCircuit", Declared);
constexpr int kTagShort = 0x44530001;   // 'DS'

constexpr float kReachCm = 1500.f;  // a client's short from its body to the desk (a liquid is thrown or poured)
constexpr size_t kQueueCap = 8;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false;    // the watch is registered (it lives for the process: it is keyed on names)
bool g_settled = false;    // its names resolved, and a dead one said
bool g_applying = false;   // OnState's own call of shortCircuit is running: its edge is the host's, not a liquid's
bool g_preKnown = false;   // the pre read the flag
bool g_preShorting = false;
bool g_pendingAsk = false; // CLIENT: a short of its own to send
struct In {
    int slot;
    Payload p;
};
std::vector<In> g_inbox;   // HOST
Counts g_c;

// The drill's red arms: no short is announced (the host tells no client, a client asks no host), and a joiner is not
// told of a short under way.
bool Off() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::desk_short_off);
    return off;
}
bool ReplayOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::desk_short_replay_off);
    return off;
}

coop::net::Session* Live() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() ? s : nullptr;
}

void* Desk() { return CD::EnsureResolved() ? CD::Instance() : nullptr; }

bool InReach(int slot, void* desk) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    void* body = rp && rp->valid() ? rp->GetActor() : nullptr;
    ue_wrap::FVector at{}, by{};
    if (!body || !ue_wrap::engine::TryGetActorLocation(desk, at) || !ue_wrap::engine::TryGetActorLocation(body, by))
        return false;
    const float dx = at.X - by.X, dy = at.Y - by.Y, dz = at.Z - by.Z;
    return dx * dx + dy * dy + dz * dz <= kReachCm * kReachCm;
}

// Runs the desk's own short on this copy, unless it shorts already. The watch sees the call.
bool Short(void* desk) {
    bool now = false;
    if (!DSh::ReadShortCircuiting(desk, now) || now) return false;
    return DSh::CallShortCircuit(desk);
}

// The body's entry: whether the flag was already set, which is the event's own guard.
sg::Verdict OnShortPre(const sg::Call& call) {
    bool on = false;
    g_preKnown = DSh::ReadShortCircuiting(call.object, on);
    g_preShorting = on;
    return sg::Verdict::Run;
}

// The body's end: a flag that was clear and is set now is a short that began. The host tells every client; a client's
// own liquid is asked of the host, and the one the host's word started is not.
void OnShortPost(const sg::Call& call) {
    const bool known = g_preKnown, was = g_preShorting;
    g_preKnown = false;
    if (!known || was || Off()) return;
    bool on = false;
    if (!DSh::ReadShortCircuiting(call.object, on) || !on) return;
    coop::net::Session* s = Live();
    if (!s) return;
    if (s->role() == coop::net::Role::Host) {
        Payload p{};
        p.on = 1;
        if (s->SendReliable(coop::net::ReliableKind::DeskShortState, &p, sizeof(p))) ++g_c.sent;
        UE_LOGI("desk_short: the host's desk shorts -- sent to every client (#%u)", g_c.sent);
    } else if (!g_applying && coop::net_pump::HasAnnouncedWorldReady()) {
        g_pendingAsk = true;
        ++g_c.asked;
        UE_LOGI("desk_short: this player's liquid shorted the desk -- the host shorts its own (#%u)", g_c.asked);
    }
}

void HostServe(coop::net::Session& s, void* desk) {
    std::vector<In> inbox;
    inbox.swap(g_inbox);
    for (const In& in : inbox) {
        if (!s.IsSlotWorldReady(in.slot) || !desk) {
            ++g_c.refused;
            UE_LOGW("desk_short: slot %d's short reached no desk here -- refused", in.slot);
            continue;
        }
        if (!InReach(in.slot, desk)) {
            ++g_c.refused;
            UE_LOGW("desk_short: slot %d's short came from out of reach -- refused", in.slot);
            continue;
        }
        const bool ran = Short(desk);
        if (ran) ++g_c.served;
        UE_LOGI("desk_short: slot %d's liquid shorted the desk -- the host's desk shorts (ran=%d, #%u)", in.slot,
                ran ? 1 : 0, g_c.served);
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched) return;
    g_watched = sg::WatchName(kRowShort, kTagShort, &OnShortPre, &OnShortPost);
    static bool sSaid = false;
    if (!g_watched && !sSaid) UE_LOGE("desk_short: the watch on the desk's shortCircuit was refused -- retried each tick");
    sSaid = !g_watched;
}

void Tick() {
    if (g_watched && !g_settled) {
        sg::ResolvePendingNames();
        g_settled = sg::NameWatchSettled(kRowShort, kTagShort);
        if (g_settled && !sg::NameWatchLive(kRowShort, kTagShort))
            UE_LOGE("desk_short: the watch on the desk's shortCircuit is dead -- a short reaches no other peer");
    }
    coop::net::Session* s = Live();
    if (!s) return;
    if (s->role() == coop::net::Role::Host) {
        if (!g_inbox.empty()) HostServe(*s, Desk());
        return;
    }
    if (g_pendingAsk) {
        Payload p{};
        p.on = 1;
        if (s->SendReliable(coop::net::ReliableKind::DeskShortRequest, &p, sizeof(p))) g_pendingAsk = false;
    }
}

void OnState(const coop::net::DeskShortPayload& p) {
    if (!p.on) return;
    void* desk = Desk();
    if (!desk) return;
    g_applying = true;
    const bool ran = Short(desk);
    g_applying = false;
    if (ran) {
        ++g_c.applied;
        UE_LOGI("desk_short: the host's desk shorts -- this desk shorts too (#%u)", g_c.applied);
    }
}

void OnRequest(int senderSlot, const coop::net::DeskShortPayload& p) {
    if (p.on && g_inbox.size() < kQueueCap) g_inbox.push_back({senderSlot, p});
}

void SendCurrentToSlot(int slot) {
    coop::net::Session* s = Live();
    if (!s || s->role() != coop::net::Role::Host || ReplayOff()) return;
    bool on = false;
    void* desk = Desk();
    // Nothing announces a short to a peer that was not there: the host's flag is read once, here, at the ready edge.
    if (!desk || !DSh::ReadShortCircuiting(desk, on) || !on) return;
    Payload p{};
    p.on = 1;
    if (s->SendReliableToSlot(slot, coop::net::ReliableKind::DeskShortState, &p, sizeof(p))) ++g_c.replayed;
    UE_LOGI("desk_short: slot %d's world is up while the host's desk shorts -- its desk shorts from the start (#%u)", slot,
            g_c.replayed);
}

Counts GetCounts() { return g_c; }

void OnDisconnect() {
    if (g_c.sent || g_c.asked || g_c.served || g_c.refused || g_c.applied || g_c.replayed)
        UE_LOGI("desk_short: session end -- host sent=%u served=%u refused=%u replayed=%u; client asked=%u applied=%u",
                g_c.sent, g_c.served, g_c.refused, g_c.replayed, g_c.asked, g_c.applied);
    g_c = Counts{};
    g_inbox.clear();
    g_preKnown = g_preShorting = g_applying = g_pendingAsk = false;
    g_session.store(nullptr);
}

}  // namespace coop::desk_short_sync
