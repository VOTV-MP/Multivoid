// coop/props/container_break.cpp -- see coop/props/container_break.h.

#include "coop/props/container_break.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <string>
#include <vector>

namespace coop::props::container_break {
namespace {

namespace R  = ue_wrap::reflection;
namespace ci = ue_wrap::container_inventory;
using Payload = coop::net::ContainerBreakPayload;

constexpr float kReachCm = 1000.f;     // a break's sender from its container, on the host (a throw carries it away)
constexpr float kLethal = 1000000.f;   // a damage past any container's health and resistance
constexpr size_t kQueueCap = 16;

std::atomic<coop::net::Session*> g_session{nullptr};
std::vector<Payload> g_outbox;                      // CLIENT
struct In {
    int slot;
    Payload p;
};
std::vector<In> g_inbox;                            // HOST
uint32_t g_asked = 0, g_broken = 0, g_refused = 0;

std::wstring KeyOf(void* a) {
    std::wstring key = ue_wrap::prop::GetInteractableKeyString(a);
    if (key.empty() || key == L"None" || key.size() >= sizeof(coop::net::WireKey::data)) return {};
    return key;
}

coop::net::Session* Live() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() ? s : nullptr;
}

// The prop's own physicsImpact component.
void* ImpactOf(void* prop) {
    static int32_t s_off = -2;
    if (s_off == -2) {
        void* cls = R::FindClass(L"prop_C");
        if (!cls) return nullptr;
        s_off = R::FindPropertyOffset(cls, L"physicsImpact");
    }
    if (s_off < 0) return nullptr;
    void* comp = *reinterpret_cast<void**>(static_cast<uint8_t*>(prop) + s_off);
    return comp && R::IsLive(comp) ? comp : nullptr;
}

// The host's own copy breaks by the same lethal call: its `broken` spills, its component destroys it.
bool Break(void* container, bool fire) {
    void* comp = ImpactOf(container);
    void* fn = comp ? R::FindFunction(R::ClassOf(comp), fire ? L"fireDamage" : L"impactDamage") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid()) return false;
    f.Set<float>(L"damage", kLethal);
    return ue_wrap::Call(comp, f);
}

bool InReach(int slot, void* container) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    void* body = rp && rp->valid() ? rp->GetActor() : nullptr;
    ue_wrap::FVector at{}, by{};
    if (!body || !ue_wrap::engine::TryGetActorLocation(container, at) ||
        !ue_wrap::engine::TryGetActorLocation(body, by))
        return false;
    const float dx = at.X - by.X, dy = at.Y - by.Y, dz = at.Z - by.Z;
    return dx * dx + dy * dy + dz * dz <= kReachCm * kReachCm;
}

void HostServe(coop::net::Session& s) {
    std::vector<In> inbox;
    inbox.swap(g_inbox);
    for (const In& in : inbox) {
        const std::wstring key = coop::net::StringFromWireKey(in.p.key);
        void* c = coop::prop_key_index::Find(key);
        if (!s.IsSlotWorldReady(in.slot) || !c || !R::IsLive(c) || !ci::IsContainer(c)) {
            ++g_refused;
            UE_LOGW("container_break: slot %d's break of '%ls' names no container here -- refused", in.slot, key.c_str());
            continue;
        }
        if (!InReach(in.slot, c)) {
            ++g_refused;
            UE_LOGW("container_break: slot %d's break of '%ls' is out of reach -- refused", in.slot, key.c_str());
            continue;
        }
        const bool ok = Break(c, in.p.fire != 0);
        if (ok) ++g_broken;
        UE_LOGI("container_break: slot %d broke '%ls' -- the host's copy breaks and spills (%s, ok=%d, #%u)", in.slot,
                key.c_str(), in.p.fire ? "fire" : "impact", ok ? 1 : 0, g_broken);
    }
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

bool RequestFromClient(void* owner, bool fire) {
    if (!owner || !R::IsLive(owner) || !ci::IsContainer(owner)) return false;
    const std::wstring key = KeyOf(owner);
    if (key.empty()) return false;
    if (g_outbox.size() < kQueueCap) {
        Payload p{};
        coop::net::WireKeyFromString(key, p.key);
        p.fire = fire ? 1 : 0;
        g_outbox.push_back(p);
        ++g_asked;
        UE_LOGI("container_break: this player broke '%ls' (%s) -- the host breaks it (#%u)", key.c_str(),
                fire ? "fire" : "impact", g_asked);
    }
    return true;
}

void Tick() {
    coop::net::Session* s = Live();
    if (!s) return;
    if (s->role() == coop::net::Role::Host) {
        HostServe(*s);
        return;
    }
    if (!coop::net_pump::HasAnnouncedWorldReady()) return;
    for (auto it = g_outbox.begin(); it != g_outbox.end();) {
        if (!s->SendReliable(coop::net::ReliableKind::ContainerBreakRequest, &*it, sizeof(*it))) break;
        it = g_outbox.erase(it);
    }
}

void OnRequest(int senderSlot, const coop::net::ContainerBreakPayload& p) {
    if (g_inbox.size() < kQueueCap) g_inbox.push_back({senderSlot, p});
}

void OnDisconnect() {
    if (g_asked || g_broken || g_refused)
        UE_LOGI("container_break: session end -- asked=%u, host broke=%u refused=%u", g_asked, g_broken, g_refused);
    g_asked = g_broken = g_refused = 0;
    g_outbox.clear();
    g_inbox.clear();
    g_session.store(nullptr);
}

}  // namespace coop::props::container_break
