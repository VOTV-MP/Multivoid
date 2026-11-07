// coop/interactables/desk_short_sync.cpp -- see coop/interactables/desk_short_sync.h.

#include "coop/interactables/desk_short_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/desk_short.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <chrono>
#include <vector>

namespace coop::desk_short_sync {
namespace {

namespace CD = ue_wrap::console_desk;
namespace DSh = ue_wrap::desk_short;
using Payload = coop::net::DeskShortPayload;
using Clock = std::chrono::steady_clock;

constexpr float kReachCm = 1500.f;  // a client's short from its body to the desk (a liquid is thrown or poured)
constexpr size_t kQueueCap = 8;

std::atomic<coop::net::Session*> g_session{nullptr};
Clock::time_point g_nextPoll{};
void* g_desk = nullptr;    // the desk the poll read last: a new world's desk is read once before an edge counts
bool g_was = false;        // the desk's flag at the last poll
bool g_primed = false;     // a first read was taken in this world
bool g_fromWire = false;   // this peer's next rising edge is a short the wire asked for
bool g_pendingAsk = false; // CLIENT: a short of its own to send
struct In {
    int slot;
    Payload p;
};
std::vector<In> g_inbox;   // HOST
uint32_t g_sent = 0, g_asked = 0, g_served = 0, g_refused = 0, g_applied = 0;

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

// Runs the desk's own short on this copy, unless it shorts already. The next poll reads the edge.
bool Short(void* desk) {
    bool now = false;
    if (!DSh::ReadShortCircuiting(desk, now) || now) return false;
    return DSh::CallShortCircuit(desk);
}

void HostServe(coop::net::Session& s, void* desk) {
    std::vector<In> inbox;
    inbox.swap(g_inbox);
    for (const In& in : inbox) {
        if (!s.IsSlotWorldReady(in.slot) || !desk) {
            ++g_refused;
            UE_LOGW("desk_short: slot %d's short reached no desk here -- refused", in.slot);
            continue;
        }
        if (!InReach(in.slot, desk)) {
            ++g_refused;
            UE_LOGW("desk_short: slot %d's short came from out of reach -- refused", in.slot);
            continue;
        }
        const bool ran = Short(desk);
        if (ran) ++g_served;
        UE_LOGI("desk_short: slot %d's liquid shorted the desk -- the host's desk shorts (ran=%d, #%u)", in.slot,
                ran ? 1 : 0, g_served);
    }
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    coop::net::Session* s = Live();
    if (!s) return;
    const bool host = s->role() == coop::net::Role::Host;
    if (!host && !coop::net_pump::HasAnnouncedWorldReady()) return;
    void* desk = Desk();
    if (host) HostServe(*s, desk);

    const Clock::time_point now = Clock::now();
    if (desk != g_desk) {
        g_desk = desk;
        g_primed = false;
    }
    if (desk && now >= g_nextPoll) {
        g_nextPoll = now + std::chrono::milliseconds(100);
        bool on = false;
        if (DSh::ReadShortCircuiting(desk, on)) {
            const bool rose = g_primed && on && !g_was;
            g_was = on;
            g_primed = true;
            if (rose) {
                if (host) {
                    Payload p{};
                    p.on = 1;
                    if (s->SendReliable(coop::net::ReliableKind::DeskShortState, &p, sizeof(p))) ++g_sent;
                    UE_LOGI("desk_short: the host's desk shorts -- sent to every client (#%u)", g_sent);
                } else if (!g_fromWire) {
                    g_pendingAsk = true;
                    ++g_asked;
                    UE_LOGI("desk_short: this player's liquid shorted the desk -- the host shorts its own (#%u)", g_asked);
                }
                g_fromWire = false;
            }
        }
    }
    if (!host && g_pendingAsk) {
        Payload p{};
        p.on = 1;
        if (s->SendReliable(coop::net::ReliableKind::DeskShortRequest, &p, sizeof(p))) g_pendingAsk = false;
    }
}

void OnState(const coop::net::DeskShortPayload& p) {
    if (!p.on) return;
    void* desk = Desk();
    if (!desk) return;
    if (Short(desk)) {
        g_fromWire = true;
        ++g_applied;
        UE_LOGI("desk_short: the host's desk shorts -- this desk shorts too (#%u)", g_applied);
    }
}

void OnRequest(int senderSlot, const coop::net::DeskShortPayload& p) {
    if (p.on && g_inbox.size() < kQueueCap) g_inbox.push_back({senderSlot, p});
}

void OnDisconnect() {
    if (g_sent || g_asked || g_served || g_refused || g_applied)
        UE_LOGI("desk_short: session end -- host sent=%u served=%u refused=%u; client asked=%u applied=%u", g_sent,
                g_served, g_refused, g_asked, g_applied);
    g_sent = g_asked = g_served = g_refused = g_applied = 0;
    g_inbox.clear();
    g_was = g_primed = g_fromWire = g_pendingAsk = false;
    g_desk = nullptr;
    g_session.store(nullptr);
}

}  // namespace coop::desk_short_sync
