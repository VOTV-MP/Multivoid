// coop/props/fireext_thrust_sync.cpp -- see coop/props/fireext_thrust_sync.h.

#include "coop/props/fireext_thrust_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/props/prop_drive_host.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/actors/fire_extinguisher.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace coop::fireext_thrust_sync {
namespace {

namespace FX = ue_wrap::fire_extinguisher;
namespace R  = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;

UE_FACT_FN(kRowShoot, "fireext_thrust_sync", L"prop_fireExt_C", L"shoot", Declared);

constexpr int    kTagShoot = 0x46585400;  // 'FXT'
constexpr size_t kMaxHeld = 16;

std::atomic<coop::net::Session*> g_session{nullptr};
bool     g_watched = false;
uint64_t g_refused = 0;
uint64_t g_overflow = 0;   // the runaways past kMaxHeld this session: left unstreamed, each peer's copy thrusts alone

struct Held {
    void*   actor;
    int32_t idx;
};
std::vector<Held> g_held;  // the host's extinguishers in runaway thrust

// The runaway drill's two red arms, one per half of the sync, each resolved once: no claim on the host while its extinguisher
// thrusts (the flight is not streamed), and no refusal of a client copy's own shoot.
bool ClaimOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::fireext_claim_off);
    return off;
}
bool CancelOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::fireext_cancel_off);
    return off;
}

// A client's copy starts no runaway of its own.
SG::Verdict OnShootPre(const SG::Call& call) {
    if (CancelOff() || !coop::spawn_authority::IsActiveClientSession()) return SG::Verdict::Run;
    const uint64_t n = ++g_refused;
    if ((n & (n - 1)) == 0)
        UE_LOGI("fireext_thrust: a client copy's runaway thrust refused on %p (call #%llu)", call.object,
                static_cast<unsigned long long>(n));
    return SG::Verdict::Cancel;
}

// The host's extinguisher went off: it is held for the stream from the next tick.
void OnShootPost(const SG::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (ClaimOff() || !s || !s->running() || s->role() != coop::net::Role::Host || !call.object) return;
    for (const Held& h : g_held)
        if (h.actor == call.object) return;
    if (g_held.size() >= kMaxHeld) {
        const uint64_t n = ++g_overflow;
        if ((n & (n - 1)) == 0)
            UE_LOGI("fireext_thrust: %zu extinguishers already thrust -- the one on %p is not streamed (#%llu)", g_held.size(),
                    call.object, static_cast<unsigned long long>(n));
        return;
    }
    g_held.push_back({call.object, R::InternalIndexOf(call.object)});
    UE_LOGI("fireext_thrust: the host's extinguisher %p went off -- streamed while it thrusts", call.object);
}

}  // namespace

void Install(coop::net::Session* s) {
    g_session.store(s, std::memory_order_release);
    if (g_watched) return;
    g_watched = true;
    if (SG::WatchName(kRowShoot, kTagShoot, &OnShootPre, &OnShootPost))
        UE_LOGI("fireext_thrust: prop_fireExt_C::shoot is watched (refused on a client's copy, streamed from the host's)");
    else
        UE_LOGE("fireext_thrust: the watch on prop_fireExt_C::shoot was refused -- each peer's copy thrusts on its own");
}

void Tick(coop::net::Session* s) {
    if (g_held.empty()) return;
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    for (size_t i = 0; i < g_held.size();) {
        const Held h = g_held[i];
        bool thrusting = false;
        float charge = 0.f;
        const bool live = R::IsLiveByIndex(h.actor, h.idx);
        if (live && FX::ReadThrusting(h.actor, thrusting) && thrusting && FX::ReadCharge(h.actor, charge) && charge > 0.f) {
            // A hand that took it owns it (the held-prop lane streams it); the claim is asked again when it is let go.
            if (!coop::prop_drive_host::InAHand(h.actor)) coop::prop_drive_host::Claim(h.actor, "extinguisher thrust");
            ++i;
            continue;
        }
        if (live) {
            coop::prop_drive_host::Release(h.actor);
            UE_LOGI("fireext_thrust: the host's extinguisher %p is spent -- it coasts to rest", h.actor);
        }
        g_held[i] = g_held.back();
        g_held.pop_back();
    }
}

void OnDisconnect() {
    if (g_refused > 0)
        UE_LOGI("fireext_thrust: %llu runaway starts refused on this client's copies over the session",
                static_cast<unsigned long long>(g_refused));
    g_refused = 0;
    g_overflow = 0;
    g_held.clear();
}

}  // namespace coop::fireext_thrust_sync
