// coop/player_damage.cpp -- see coop/player_damage.h.
//
// vitals Inc3-WIRE relay half: host SENDS PlayerDamage to the hit peer; the owner
// APPLIES it to its own possessed player. The vitals->flash tail (Inc1 health stream
// + Inc3 hurt-flash) is already wired, so this module's job ends at causing the
// owner's local saveSlot.health to drop.

#include "coop/player_damage.h"

#include "coop/element/element.h"
#include "coop/element/player.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/players_registry.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/vitals.h"

#include <cmath>
#include <cstdint>

namespace coop::player_damage {
namespace {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace E = ue_wrap::engine;

coop::net::Session* g_session = nullptr;

// Defensive per-hit bound: VOTV max health is ~100, so 1000 is generous headroom for
// any legitimate big hit while rejecting an absurd/exploit value before it reaches the
// damage BP. NaN/Inf/<=0 are rejected too.
constexpr float kMaxDamagePerHit = 1000.f;

bool ValidDamage(float d) { return std::isfinite(d) && d > 0.f && d <= kMaxDamagePerHit; }

}  // namespace

void Install(coop::net::Session* session) {
    g_session = session;
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
    if (!g_session) {
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
    const bool sent = g_session->SendReliableToSlot(
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
