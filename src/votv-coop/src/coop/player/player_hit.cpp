// coop/player/player_hit.cpp -- see coop/player/player_hit.h.

#include "coop/player/player_hit.h"

#include "coop/element/element.h"
#include "coop/element/mirror_managers.h"
#include "coop/element/npc.h"
#include "coop/element/player.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/obj_keyed_cache.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine_player_verbs.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>

namespace coop::player_hit {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace EN = ue_wrap::engine;
namespace EL = coop::element;

std::atomic<coop::net::Session*> g_session{nullptr};

// punch's `bn1` across the wire: the six names the game's own call sites pass, in the wire's order.
constexpr const wchar_t* kBones[coop::net::kPlayerHitBoneCount] = {L"None", L"chest", L"head", L"pelvis", L"torso",
                                                                   L"lowlegs"};

constexpr float kMaxComponent = 1e7f;  // a world coordinate or an impulse; far past any legitimate one

int  g_applyDepth = 0;  // game thread
uint32_t g_applied = 0;
uint32_t g_droppedNoBody = 0;
uint32_t g_droppedSource = 0;
uint32_t g_failedApplies = 0;
uint32_t g_refusedSends = 0;
bool g_boneSaid = false;
bool g_noneSaid = false;                          // the first hit with no source creature has been said
ue_wrap::ObjKeyedCache<bool, 24> g_appliedSaid;   // the source classes whose first applied hit has been said

bool FiniteWithin(const float (&v)[3]) {
    for (const float c : v)
        if (!std::isfinite(c) || std::fabs(c) > kMaxComponent) return false;
    return true;
}

// The format check of OnReceived: the host authors the hit, so nothing here weighs its size.
bool WellFormed(const coop::net::PlayerHitPayload& p) {
    return (p.verb == coop::net::kPlayerHitVerbAddDamage || p.verb == coop::net::kPlayerHitVerbPunch) &&
           p.bone < coop::net::kPlayerHitBoneCount && std::isfinite(p.damage) && p.damage >= 0.f &&
           FiniteWithin(p.v0) && FiniteWithin(p.v1) && FiniteWithin(p.v2);
}

ue_wrap::FVector Vec(const float (&v)[3]) { return ue_wrap::FVector{v[0], v[1], v[2]}; }

struct ApplyScope {
    ApplyScope() { ++g_applyDepth; }
    ~ApplyScope() { --g_applyDepth; }
};

// The verb on this machine's own player. Game thread.
void Apply(const coop::net::PlayerHitPayload& p) {
    auto& reg = coop::players::Registry::Get();
    const EL::ElementId localEid = reg.LocalPlayerElementId();
    // PlayerHit is sent point-to-point to our slot, so it is ours; the check guards a fan-out or relay bug. A
    // definite mismatch against a valid local id drops; an id not allocated yet (the boot window) does not.
    if (localEid != EL::kInvalidId && p.targetElementId != localEid) {
        UE_LOGW("player_hit: hit targetEid=%u != our localEid=%u -- dropping (not addressed to us)", p.targetElementId,
                localEid);
        return;
    }
    void* const body = reg.Local();
    if (!body || !R::IsLive(body)) {
        if (++g_droppedNoBody == 1) UE_LOGW("player_hit: a hit but no live local player to apply it to -- dropped");
        return;
    }
    // addDamage's `actor` is the creature's mirror on this machine, so the Blueprint's null-vs-valid branch is kept.
    // A source whose mirror is not resolved (the joiner's adoption window, a hidden mirror, a creature gone before
    // the hit arrived) drops the hit rather than run the other branch.
    void* source = nullptr;
    if (p.sourceEid != 0) {
        EL::Npc* mirror = EL::NpcMirrors().Get(p.sourceEid);
        source = mirror ? mirror->LiveActor() : nullptr;
        if (!source) {
            if (++g_droppedSource == 1)
                UE_LOGW("player_hit: a hit from creature eid=%u whose mirror is not resolved -- dropped", p.sourceEid);
            return;
        }
    }
    bool ok = false;
    {
        const ApplyScope scope;
        if (p.verb == coop::net::kPlayerHitVerbAddDamage)
            ok = EN::InvokeAddDamage(body, source, p.damage, Vec(p.v0));
        else
            ok = EN::InvokePunch(body, Vec(p.v0), Vec(p.v1), kBones[p.bone], p.damage, Vec(p.v2));
    }
    if (ok) ++g_applied;
    else if (++g_failedApplies == 1) UE_LOGW("player_hit: the verb did not run on the local player");
    // The first hit of a source class says what it was; the rest are counted (g_applied).
    void* const sourceClass = source ? R::ClassOf(source) : nullptr;
    const bool first = source ? !g_appliedSaid.Find(sourceClass) : !g_noneSaid;
    if (!first) return;
    if (source) g_appliedSaid.Insert(sourceClass, true); else g_noneSaid = true;
    UE_LOGI("[HIT] applied verb=%s damage=%.2f source=%ls ok=%d (first of its source class; the rest are counted)",
            p.verb == coop::net::kPlayerHitVerbAddDamage ? "addDamage" : "punch", p.damage,
            source ? R::ClassNameOf(source).c_str() : L"none", ok ? 1 : 0);
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

bool Deliver(uint8_t slot, const coop::net::PlayerHitPayload& payload) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) {
        UE_LOGW("player_hit: Deliver but the session is unset");
        return false;
    }
    if (slot == 0 || slot >= coop::players::kMaxPeers) {
        UE_LOGW("player_hit: Deliver invalid slot=%u", static_cast<unsigned>(slot));
        return false;
    }
    coop::element::Player* victim = coop::players::Registry::Get().GetPlayerElement(slot);
    if (!victim) {
        UE_LOGW("player_hit: Deliver slot=%u has no Player Element yet -- skipping (peer not fully identified)",
                static_cast<unsigned>(slot));
        return false;
    }
    coop::net::PlayerHitPayload p = payload;
    p.targetElementId = victim->GetId();
    p.pad = 0;
    if (s->SendReliableToSlot(slot, coop::net::ReliableKind::PlayerHit, &p, sizeof(p), /*senderSlot=*/0)) return true;
    // The session refused the send (the slot is not world-ready, or its backlog dropped it): the hit is lost.
    if (++g_refusedSends == 1)
        UE_LOGW("player_hit: the session refused a PlayerHit send to slot=%u -- the hit is lost (counted from here)",
                static_cast<unsigned>(slot));
    return false;
}

void OnReceived(const uint8_t* data, size_t len) {
    if (len < sizeof(coop::net::PlayerHitPayload)) {
        UE_LOGW("player_hit: PlayerHit payload too short (%zu < %zu)", len, sizeof(coop::net::PlayerHitPayload));
        return;
    }
    coop::net::PlayerHitPayload p{};
    std::memcpy(&p, data, sizeof(p));
    if (!WellFormed(p)) {
        UE_LOGW("player_hit: dropping a malformed hit (verb=%u bone=%u damage=%.3f)", static_cast<unsigned>(p.verb),
                static_cast<unsigned>(p.bone), p.damage);
        return;
    }
    GT::Post([p] { Apply(p); });
}

uint8_t BoneIndex(const std::wstring& name) {
    for (uint8_t i = 0; i < coop::net::kPlayerHitBoneCount; ++i)
        if (_wcsicmp(name.c_str(), kBones[i]) == 0) return i;
    if (!g_boneSaid) {
        g_boneSaid = true;
        UE_LOGW("player_hit: punch bone '%ls' is not in the wire's table -- sent as None", name.c_str());
    }
    return 0;
}

bool InApply() { return g_applyDepth > 0; }

uint32_t AppliedCount() { return g_applied; }

uint32_t RefusedSendCount() { return g_refusedSends; }

}  // namespace coop::player_hit
