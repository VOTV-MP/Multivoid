// coop/props/hit_target_prop.cpp -- see coop/props/hit_target_prop.h.

#include "coop/props/hit_target_prop.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/player/hit_intent.h"
#include "coop/props/prop_damage_gate.h"

#include "ue_wrap/actors/int_objects.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"

#include <cstdint>

namespace coop::hit_target_prop {
namespace {

namespace EL = coop::element;
namespace sg = ue_wrap::script_gate;
using ue_wrap::FVector;

// Each swing is said; a session's first kMaxSaid lines of each side are, the rest are not.
constexpr int kMaxSaid = 20;
int  g_sentSaid = 0;
int  g_appliedSaid = 0;
bool g_unboundSaid = false;

// Developer red arm: the host takes no prop as a target, so a client's swing at a prop reaches nobody.
bool RefusesProps() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::hit_intent_refuse_prop);
    return s;
}

}  // namespace

uint32_t ClientClaim(const sg::Call& c, coop::hit_intent::Swing& swing) {
    if (!c.object || !ue_wrap::prop::IsDescendantOfProp(c.object)) return EL::kInvalidId;

    if (!coop::hit_intent::ReadLocalSwing(c, swing)) return EL::kInvalidId;
    if (coop::props::prop_damage_gate::AuthoredHere(c.object)) return EL::kInvalidId;

    // The element the actor is bound to, O(1): a host element is the claim, a client's own row is refused by the host's
    // type check.
    const EL::ElementId eid = EL::Registry::Get().EidForActor(c.object);
    if (eid == EL::kInvalidId) {
        if (!g_unboundSaid) {
            g_unboundSaid = true;
            UE_LOGI("hit_target_prop: a swing at a prop not yet bound to the host's is lost");
        }
        return coop::hit_intent::kClaimedUnsent;
    }
    return static_cast<uint32_t>(eid);
}

void NoteSent(uint32_t eid, float damage) {
    if (g_sentSaid >= kMaxSaid) return;
    ++g_sentSaid;
    UE_LOGI("hit_target_prop: sent eid=%u dmg=%.1f", static_cast<unsigned>(eid), static_cast<double>(damage));
}

void* HostTarget(uint32_t eid) {
    if (RefusesProps()) return nullptr;
    void* actor = EL::LivePropActor(static_cast<EL::ElementId>(eid));
    return actor && ue_wrap::prop::IsDescendantOfProp(actor) ? actor : nullptr;
}

bool HostApply(uint32_t eid, const coop::net::HitIntentPayload& judged) {
    void* prop = HostTarget(eid);
    if (!prop) return false;
    const FVector hitLoc{judged.hitLoc[0], judged.hitLoc[1], judged.hitLoc[2]};
    const FVector impact{judged.impact[0], judged.impact[1], judged.impact[2]};
    if (!ue_wrap::int_objects::InvokeAddDamage(prop, nullptr, judged.damage, hitLoc, impact)) return false;
    if (g_appliedSaid < kMaxSaid) {
        ++g_appliedSaid;
        UE_LOGI("hit_target_prop: applied eid=%u", static_cast<unsigned>(eid));
    }
    return true;
}

void OnDisconnect() {
    g_sentSaid = 0;
    g_appliedSaid = 0;
    g_unboundSaid = false;
}

}  // namespace coop::hit_target_prop
