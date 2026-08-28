// coop/player_damage.cpp -- see coop/player_damage.h.
//
// vitals Inc3-WIRE relay half: host SENDS PlayerDamage to the hit peer; the owner
// APPLIES it to its own possessed player. The vitals->flash tail (Inc1 health stream
// + Inc3 hurt-flash) is already wired, so this module's job ends at causing the
// owner's local saveSlot.health to drop.

#include "coop/player/player_damage.h"

#include "coop/element/element.h"
#include "coop/element/player.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/actors/vitals.h"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace coop::player_damage {
namespace {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace E = ue_wrap::engine;

std::atomic<coop::net::Session*> g_session{nullptr};  // read in the impact interceptor (any thread)

// Defensive per-hit bound: VOTV max health is ~100, so 1000 is generous headroom for
// any legitimate big hit while rejecting an absurd/exploit value before it reaches the
// damage BP. NaN/Inf/<=0 are rejected too.
constexpr float kMaxDamagePerHit = 1000.f;

bool ValidDamage(float d) { return std::isfinite(d) && d > 0.f && d <= kMaxDamagePerHit; }

// ---- the impact-entry cancel (2026-08-29; header block) ---------------------
// mainPlayer_C's three native->BP impact events, resolved lazily (the class loads
// with gameplay). One shared PRE callback; registered once each.
const wchar_t* const kImpactEntryNames[3] = {
    L"impactDamage", L"impactDamageCPP", L"impactSquishCPP",
};
void*    g_impactFns[3] = {};
bool     g_impactInterceptorsDone = false;
uint32_t g_resolveThrottle = 0;
uint32_t g_canceled = 0;   // rate-latched log counter

// The LOCAL possessed player, published by Tick (game thread) for the
// interceptor's any-thread compare. Post-ship audit 2026-08-29 (CRITICAL):
// the first cut called Registry::IsLocal(self) inside the callback --
// Registry::Local() mutates GT-only cache state, and a ProcessEvent
// interceptor can fire on a parallel-anim worker (game_thread.h's own
// contract). A published atomic pointer keeps the CANCEL effective on every
// dispatching thread with one relaxed load and zero registry access (the
// wisp interceptor's atomic-snapshot shape).
std::atomic<void*> g_localPawn{nullptr};

// PRE cancel: a physical impact dispatched on a mainPlayer BODY that is NOT the
// local possessed player (a peer's puppet, or the F1 skin-preview mannequin) must
// not run the BP body -- it writes the PER-MACHINE saveSlot.health singleton and
// drained the HOST when lethal contact resolved against a puppet in the host's
// world (the "kill a player -> the host dies -> server falls" field report). The
// victim's own machine computes the same contact on its own possessed player
// (self==local -> pass-through), so nothing is lost -- MTA's victim-authoritative
// shape, no relay. Coop-session gated: solo SP has only the local body anyway,
// and an unset registry must never eat solo damage.
bool OnImpactEntryPre(void* self, void* /*params*/) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return false;
    if (!self) return false;
    void* localPawn = g_localPawn.load(std::memory_order_acquire);
    if (!localPawn) return false;           // pre-resolve boot window: native behavior
    if (self == localPawn) return false;    // own body: native path
    const uint32_t n = ++g_canceled;
    if (n <= 5 || (n % 50) == 0) {
        UE_LOGI("player_damage: canceled impact entry on NON-LOCAL body %p (#%u) -- "
                "the hit body's owner computes its own damage", self, n);
    }
    return true;  // cancel: the singleton saveSlot write must not run for a foreign body
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    // Publish the local pawn for the interceptor's any-thread compare (GT here;
    // Registry::Local() is GT-only and cached).
    g_localPawn.store(coop::players::Registry::Get().Local(), std::memory_order_release);
    if (g_impactInterceptorsDone) return;
    // FindClass walks GUObjectArray -- ~1 Hz of the pump until mainPlayer_C loads
    // (the wisp_attack Install throttle shape).
    if ((g_resolveThrottle++ % 125) != 0) return;
    void* cls = R::FindClass(L"mainPlayer_C");
    if (!cls) return;
    bool all = true;
    for (int i = 0; i < 3; ++i) {
        if (!g_impactFns[i]) g_impactFns[i] = R::FindFunction(cls, kImpactEntryNames[i]);
        if (!g_impactFns[i]) { all = false; continue; }
        if (!GT::RegisterInterceptor(g_impactFns[i], &OnImpactEntryPre)) all = false;
    }
    if (all) {
        g_impactInterceptorsDone = true;
        UE_LOGI("player_damage: impact-entry PRE cancels installed (impactDamage=%p "
                "impactDamageCPP=%p impactSquishCPP=%p) -- non-local bodies never run "
                "the saveSlot damage body",
                g_impactFns[0], g_impactFns[1], g_impactFns[2]);
    }
}

void OnWireDamage(const coop::net::PlayerDamagePayload& p) {
    if (!ValidDamage(p.damage)) {
        UE_LOGW("player_damage: dropping wire damage with invalid amount=%.3f", p.damage);
        return;
    }
    const uint32_t targetEid = p.targetElementId;
    const float damage = p.damage;
    GT::Post([targetEid, damage] {
        auto& reg = coop::players::Registry::Get();
        const coop::element::ElementId localEid = reg.LocalPlayerElementId();
        // Defense-in-depth: PlayerDamage is sent point-to-point to our slot, so it IS
        // for us; the element check guards a future fan-out/relay-path bug. Apply on a
        // match OR when our id isn't allocated yet (boot window); drop ONLY on a
        // definite mismatch against a valid local id.
        if (localEid != coop::element::kInvalidId && targetEid != localEid) {
            UE_LOGW("player_damage: wire damage targetEid=%u != our localEid=%u -- dropping "
                    "(not addressed to us)", targetEid, localEid);
            return;
        }
        void* mp = reg.Local();
        if (!mp || !R::IsLive(mp)) {
            UE_LOGW("player_damage: wire damage but no live local player to apply to -- dropping");
            return;
        }
        const bool possessed = (E::GetController(mp) != nullptr);
        const bool isPup = reg.IsPuppet(mp);
        float before = -1.f, after = -1.f;
        ue_wrap::vitals::Read(ue_wrap::vitals::Field::Health, &before);
        const bool ok = E::InvokeAddPlayerDamage(mp, damage);
        ue_wrap::vitals::Read(ue_wrap::vitals::Field::Health, &after);
        UE_LOGI("player_damage: applied %.1f to own player (targetEid=%u) -> ok=%d, "
                "saveSlot.health %.2f -> %.2f [mp possessed=%d isPuppet=%d]",
                damage, targetEid, ok ? 1 : 0, before, after, possessed ? 1 : 0, isPup ? 1 : 0);
    });
}

void SendPlayerDamage(int ownerSlot, float damage) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) {
        UE_LOGW("player_damage: SendPlayerDamage but session is unset");
        return;
    }
    if (ownerSlot <= 0 || ownerSlot >= coop::players::kMaxPeers) {
        UE_LOGW("player_damage: SendPlayerDamage invalid ownerSlot=%d", ownerSlot);
        return;
    }
    if (!ValidDamage(damage)) {
        UE_LOGW("player_damage: SendPlayerDamage invalid amount=%.3f", damage);
        return;
    }
    auto& reg = coop::players::Registry::Get();
    coop::element::Player* el = reg.GetPlayerElement(static_cast<uint8_t>(ownerSlot));
    if (!el) {
        UE_LOGW("player_damage: SendPlayerDamage slot=%d has no Player Element yet -- skipping "
                "(peer not fully identified)", ownerSlot);
        return;
    }
    coop::net::PlayerDamagePayload p{};
    p.targetElementId = el->GetId();
    p.damage = damage;
    const bool sent = s->SendReliableToSlot(
        ownerSlot, coop::net::ReliableKind::PlayerDamage, &p, sizeof(p), /*senderSlot=*/0);
    UE_LOGI("player_damage: sent PlayerDamage(%.1f) to slot=%d (targetEid=%u) -> sent=%d",
            damage, ownerSlot, p.targetElementId, sent ? 1 : 0);
}

bool DebugForceHitPuppet(int ownerSlot, float damage) {
    if (ownerSlot <= 0 || ownerSlot >= coop::players::kMaxPeers) {
        UE_LOGW("player_damage: DebugForceHitPuppet invalid ownerSlot=%d", ownerSlot);
        return false;
    }
    auto& reg = coop::players::Registry::Get();
    if (!reg.Puppet(static_cast<uint8_t>(ownerSlot))) {
        UE_LOGW("player_damage: DebugForceHitPuppet slot=%d has no puppet (peer not connected) "
                "-- cannot simulate a hit", ownerSlot);
        return false;
    }
    SendPlayerDamage(ownerSlot, damage);
    return true;
}

}  // namespace coop::player_damage
