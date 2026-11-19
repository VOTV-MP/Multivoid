// coop/props/fireext_thrust_sync.cpp -- see coop/props/fireext_thrust_sync.h.

#include "coop/props/fireext_thrust_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/props/fireext_sync.h"
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
namespace EL = coop::element;

UE_FACT_FN(kRowShoot, "fireext_thrust_sync", L"prop_fireExt_C", L"shoot", Declared);

constexpr int      kTagShoot = 0x46585400;  // 'FXT'
constexpr size_t   kMaxHeld = 16;
constexpr uint64_t kSayEveryMs = 500;       // a thrust's charge, at most once per this while it runs

std::atomic<coop::net::Session*> g_session{nullptr};
bool     g_watched = false;
uint64_t g_refused = 0;
uint32_t g_clientShootsRun = 0;        // a client copy's shoot let run by the fireext_cancel_off red: the drill reads it
uint32_t g_clientShootsCancelled = 0;  // a client copy's shoot the PRE cancelled: the drill reads it
uint64_t g_overflow = 0;   // the runaways past kMaxHeld this session: left unstreamed, each peer's copy thrusts alone

struct Held {
    void*    actor;
    int32_t  idx;
    uint32_t eid;       // kept, so the end of a thrust is said for an actor that is gone
    uint64_t saidMs;
};
std::vector<Held> g_held;  // the host's extinguishers in runaway thrust

// The drill's red arms, one per half of the sync, each resolved once: no claim on the host while its extinguisher thrusts (the
// flight is not streamed), no refusal of a client copy's own shoot, no ask to the host from it, and no thrust state sent.
bool ClaimOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::fireext_claim_off);
    return off;
}
bool CancelOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::fireext_cancel_off);
    return off;
}
bool AskOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::fireext_ask_off);
    return off;
}
bool SayOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::fireext_thrust_say_off);
    return off;
}
// The red arm of the names' resolve: the lane never resolves them, and nothing else does.
bool NamesOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::fireext_names_off);
    return off;
}
bool g_ticked = false;

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Host;
}

uint32_t EidOf(void* ext) {
    const EL::ElementId eid = EL::Registry::Get().EidForActor(ext);
    return eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid);
}

void Say(const Held& h, bool on, float charge) {
    if (!SayOff()) coop::fireext_sync::SayThrustByEid(h.eid, on, charge, -1);
}

void Refused(const SG::Call& call, const char* why) {
    const uint64_t n = ++g_refused;
    if ((n & (n - 1)) == 0)
        UE_LOGI("fireext_thrust: a runaway start refused on %p (%s, call #%llu)", call.object, why,
                static_cast<unsigned long long>(n));
}

// The runaway's start on a copy: a remote hand mirror never runs it; a client's own held extinguisher never does and asks
// nothing; a client's other copy asks the host, unless the call is its own record apply, and never runs it. The host's runs.
SG::Verdict OnShootPre(const SG::Call& call) {
    void* const a = call.object;
    if (a && coop::hand_item::IsHandAxisActor(a) && !coop::hand_item::IsLocalHandViewActor(a)) {
        Refused(call, "a hand mirror");
        return SG::Verdict::Cancel;
    }
    if (!coop::spawn_authority::IsActiveClientSession()) return SG::Verdict::Run;
    if (a && coop::hand_item::IsLocalHandViewActor(a)) {
        Refused(call, "this player's own hand");
        return SG::Verdict::Cancel;
    }
    if (CancelOff()) {
        ++g_clientShootsRun;
        return SG::Verdict::Run;
    }
    ++g_clientShootsCancelled;
    if (AskOff()) {
        Refused(call, "a client copy, no ask");
        return SG::Verdict::Cancel;
    }
    if (a && !(call.callerFunction && R::NameEquals(R::NameOf(call.callerFunction), L"loadData")))
        coop::fireext_sync::AskRunaway(a);
    Refused(call, "a client copy");
    return SG::Verdict::Cancel;
}

// An extinguisher joins the held list for the stream and the charge. False when the list is full.
bool Hold(void* actor) {
    for (const Held& h : g_held)
        if (h.actor == actor) return true;
    if (g_held.size() >= kMaxHeld) {
        const uint64_t n = ++g_overflow;
        if ((n & (n - 1)) == 0)
            UE_LOGI("fireext_thrust: %zu extinguishers already thrust -- the one on %p is not streamed (#%llu)", g_held.size(),
                    actor, static_cast<unsigned long long>(n));
        return false;
    }
    g_held.push_back({actor, R::InternalIndexOf(actor), EidOf(actor), coop::net::NowMs()});
    return true;
}

// The host's extinguisher went off: it is held for the stream from the next tick, and every peer is told it thrusts.
void OnShootPost(const SG::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Host || !call.object) return;
    for (const Held& h : g_held)
        if (h.actor == call.object) return;
    if (!Hold(call.object)) return;
    UE_LOGI("fireext_thrust: the host's extinguisher %p went off -- streamed while it thrusts", call.object);
    float charge = 0.f;
    if (FX::ReadCharge(call.object, charge)) Say(g_held.back(), true, charge);
}

}  // namespace

void Install(coop::net::Session* s) {
    g_session.store(s, std::memory_order_release);
    if (g_watched) return;
    g_watched = true;
    if (SG::WatchName(kRowShoot, kTagShoot, &OnShootPre, &OnShootPost))
        UE_LOGI("fireext_thrust: prop_fireExt_C::shoot is watched (a client copy asks the host, the host's is streamed and said)");
    else
        UE_LOGE("fireext_thrust: the watch on prop_fireExt_C::shoot was refused -- each peer's copy thrusts on its own");
}

void Tick(coop::net::Session* s) {
    g_ticked = true;
    // Every read below goes through the extinguisher's class names, false until they resolve (a latched bool after).
    if (!NamesOff()) FX::ResolveNames();
    if (g_held.empty()) return;
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    const uint64_t now = coop::net::NowMs();
    for (size_t i = 0; i < g_held.size();) {
        Held& h = g_held[i];
        bool thrusting = false;
        float charge = 0.f;
        const bool live = R::IsLiveByIndex(h.actor, h.idx);
        if (live && !h.eid) h.eid = EidOf(h.actor);
        if (live && FX::ReadThrusting(h.actor, thrusting) && thrusting && FX::ReadCharge(h.actor, charge) && charge > 0.f) {
            // A hand that took it owns it (the held-prop lane streams it); the claim is asked again when it is let go.
            if (!ClaimOff() && !coop::prop_drive_host::InAHand(h.actor)) coop::prop_drive_host::Claim(h.actor, "extinguisher thrust");
            if (now - h.saidMs >= kSayEveryMs) {
                h.saidMs = now;
                Say(h, true, charge);
            }
            ++i;
            continue;
        }
        float left = -1.f;  // an extinguisher that is gone says its end with no charge
        if (live) {
            if (!ClaimOff()) coop::prop_drive_host::Release(h.actor);
            UE_LOGI("fireext_thrust: the host's extinguisher %p is spent -- it coasts to rest", h.actor);
            if (!FX::ReadCharge(h.actor, left) || left < 0.f) left = 0.f;
        }
        Say(h, false, left);
        g_held[i] = g_held.back();
        g_held.pop_back();
    }
}

bool Adopt(void* ext) { return IsHost() && ext && Hold(ext); }

bool HasTicked() { return g_ticked; }

uint32_t ClientShootsRun() { return g_clientShootsRun; }

uint32_t ClientShootsCancelled() { return g_clientShootsCancelled; }

void OnDisconnect() {
    if (g_refused > 0)
        UE_LOGI("fireext_thrust: %llu runaway starts refused on this peer's copies over the session",
                static_cast<unsigned long long>(g_refused));
    g_refused = 0;
    g_clientShootsRun = 0;
    g_clientShootsCancelled = 0;
    g_overflow = 0;
    g_held.clear();
}

}  // namespace coop::fireext_thrust_sync
