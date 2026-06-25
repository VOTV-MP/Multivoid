// coop/save_identity_bind.cpp -- see header. Phase 1 step 2b: CLIENT eid-range BIND (first mutating step).

#include "coop/save_identity_bind.h"

#include "coop/element/element.h"          // Element, ElementId, kInvalidId
#include "coop/element/registry.h"         // Registry::Get().Get(eid)
#include "coop/ini_config.h"               // IsIniKeyTrue
#include "coop/prop_element_tracker.h"     // UnmarkKnownKeyedProp, GetPropElementIdForActor, MarkBoundMirrorNative
#include "coop/remote_prop.h"              // RegisterPropMirror, ConsumeLocalActor
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"                  // GetInteractableKeyString
#include "ue_wrap/reflection.h"            // ClassNameOf, IsLive

#include <mutex>
#include <string>

namespace coop::save_identity_bind {
namespace {

namespace R   = ue_wrap::reflection;
namespace PT  = coop::prop_element_tracker;
namespace MAP = coop::save_identity_map;

std::mutex g_mu;  // SetReceivedMap (net thread) vs OnKeylessLoadSpawn (game thread)
MAP::IdMap g_map;          // the received {index->eid} map (gather order == loadObjects spawn order)
bool       g_armed = false;
size_t     g_ordinal = 0;  // the next keyless spawn's index into g_map

// per-join bind tallies (for the quiescence summary)
int g_boundChip = 0, g_boundKerfur = 0;
int g_caseI = 0, g_caseII = 0;
bool g_desynced = false;

const char* FamName(MAP::Family f) { return f == MAP::Family::ChipPile ? "chipPile" : "kerfurOff"; }

// The bind (mini-design S3): retire the native's peer-range LOCAL element, then install a host-range MIRROR
// at E onto the native. Caller holds g_mu. `family` selects the key convention (keyless pile vs keyed kerfur).
void BindLocalNativeToHostEid_(void* native, coop::element::ElementId E, MAP::Family family, size_t k) {
    // Class + key for the mirror element. A chipPile is keyless (Key==None); an off-kerfur carries its real
    // Aprop key (so a later keyed resolution still finds it). Both get a class name for the element type tag.
    const std::wstring cls = R::ClassNameOf(native);
    std::wstring key;
    if (family == MAP::Family::KerfurOff) {
        const std::wstring k2 = ue_wrap::prop::GetInteractableKeyString(native);
        if (k2 != L"None") key = k2;
    }

    // Classify the collision case (mini-design S4) BEFORE mutating, for the acceptance log.
    coop::element::Element* preE = coop::element::Registry::Get().Get(E);
    if (preE && !preE->IsMirror()) {
        // A host-range eid resolving to a LOCAL element is impossible by the range split (client locals are
        // peer-range [32768,65536), the map eids are host-range [0,32768)). Refuse rather than rebind a LOCAL
        // onto the native via RegisterPropMirror's RebindLocalElementActor path -- that would leave the native
        // a non-mirror and silently break the "host-range MIRROR" invariant the bound-guard assumes (audit
        // LOW-1). The ordinal still advances (entry consumed); the native falls back to the position-adopt path.
        UE_LOGE("save_identity_bind: host eid=%u unexpectedly resolves to a LOCAL element on the client -- "
                "REFUSING to bind native=%p (range-split invariant broken?). k=%zu", static_cast<unsigned>(E),
                native, k);
        return;
    }
    void* oldActor = preE ? preE->GetActor() : nullptr;
    const bool sameActor = (oldActor == native);
    const bool caseII = (preE && oldActor && !sameActor && R::IsLive(oldActor));  // host PropSpawn beat the bind

    // Step 3.1: free the native's peer-range LOCAL element (no-op if the post-load seed hasn't minted one yet).
    // UnmarkKnownKeyedProp drains the local Prop Element + erases g_actorToPropElementId + the key index,
    // leaving the actor ALIVE (it becomes the mirror's rendering). Identical to the position-adopt retire.
    PT::UnmarkKnownKeyedProp(native);
    // Guard: mark the native as a bound mirror so the client's post-load SeedWalk_ does NOT re-mint a peer-range
    // LOCAL element on it (MarkPropElement's idempotency only knows g_actorToPropElementId, which mirrors are
    // not in -> without this it would double-element). The root fix for binding-as-mirror at load time.
    PT::MarkBoundMirrorNative(native);
    // Step 3.2: install the host-range MIRROR at E onto the native. rebindInPlace=true handles the (ii) race
    // (re-points an already-bound E onto the native instead of HEAD-rejecting). senderSlot=0 (host owns it).
    coop::remote_prop::RegisterPropMirror(E, native, key, cls, /*senderSlot*/ 0, /*rebindInPlace*/ true);
    // (ii): a host PropSpawn already spawned a SEPARATE fresh actor at E -> after the rebind that actor is
    // orphaned (element-less); echo-destroy it so no dup survives (mini-design S4(ii)).
    if (caseII) coop::remote_prop::ConsumeLocalActor(oldActor);

    if (family == MAP::Family::ChipPile) ++g_boundChip; else ++g_boundKerfur;
    if (caseII) ++g_caseII; else ++g_caseI;

    // Log: every kerfurOff (only 4 -- the jUuC target case), every (ii) race, and the first 5 piles.
    if (caseII || family == MAP::Family::KerfurOff || k < 5) {
        UE_LOGI("save_identity_bind: BOUND k=%zu %s native=%p -> host eid=%u [%s]%s", k, FamName(family),
                native, static_cast<unsigned>(E),
                caseII ? "case(ii) host-PropSpawn-beat-bind: rebindInPlace + echo-destroyed redundant actor"
                       : (sameActor ? "case re-entrant (E already this native)" : "case(i) E free: fresh mirror"),
                family == MAP::Family::KerfurOff ? "  <- jUuC off-kerfur bound by eid (K-5-clean: host-range mirror, no client mint)" : "");
    }
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::ini_config::IsIniKeyTrue("save_identity_bind");
    return s;
}

void SetReceivedMap(const MAP::IdMap& map) {
    if (!IsEnabled()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_map = map;            // copy (the source is the client's transient receive buffer)
    g_ordinal = 0;
    g_armed = true;
    g_desynced = false;
    g_boundChip = g_boundKerfur = g_caseI = g_caseII = 0;
    UE_LOGI("save_identity_bind: ARMED with %zu-entry host eid map -- will bind each keyless load-spawn by "
            "spawn order (first mutating step; mini-design S3). [dev] save_identity_bind=1", g_map.size());
}

void OnKeylessLoadSpawn(void* newActor, MAP::Family family) {
    if (!newActor || !IsEnabled()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_armed || g_desynced) return;
    // Ignore a double-fire of the thunk for a native we already bound (keeps the ordinal aligned to distinct
    // natives; 1A saw the thunk can re-fire at a recycled address). Already-bound -> no double-bind, no advance.
    if (PT::IsBoundMirrorNative(newActor)) return;
    if (g_ordinal >= g_map.size()) {
        UE_LOGW("save_identity_bind: keyless spawn #%zu but the map holds only %zu entries -- more client "
                "keyless spawns than the host mapped; NOT binding (disarming, order assumption broken)",
                g_ordinal, g_map.size());
        g_desynced = true;
        return;
    }
    const MAP::IdEntry& e = g_map[g_ordinal];
    if (e.family != static_cast<uint8_t>(family)) {
        // Order desync: the k-th client keyless spawn's family disagrees with the k-th map entry. 1A + S8.2
        // proved the order is deterministic, so this should never fire; if it does, DO NOT bind a wrong eid --
        // disarm and report (diagnose-before-fix). The smoke surfaces exactly where the order diverged.
        UE_LOGE("save_identity_bind: ORDER DESYNC at k=%zu -- spawn family=%s but map entry family=%s "
                "(index=%u eid=%u). Disarming; %d bound before the divergence. NO wrong bind.",
                g_ordinal, FamName(family), e.family == 0 ? "chipPile" : "kerfurOff", e.index, e.eid,
                g_boundChip + g_boundKerfur);
        g_desynced = true;
        return;
    }
    if (e.eid == 0u || e.eid == coop::element::kInvalidId) {
        UE_LOGW("save_identity_bind: map entry k=%zu has invalid eid=%u -- skipping (no bind)", g_ordinal, e.eid);
        ++g_ordinal;
        return;
    }
    BindLocalNativeToHostEid_(newActor, static_cast<coop::element::ElementId>(e.eid), family, g_ordinal);
    ++g_ordinal;
}

void EmitBindSummary() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_armed) return;
    UE_LOGW("save_identity_bind: BIND SUMMARY -- bound %d/%zu map entries (%d chipPile + %d kerfurOff); "
            "case(i) E-free=%d, case(ii) race-rebind=%d; ordinal reached=%zu%s. (bound natives are host-range "
            "MIRRORs -> excluded from the divergence sweep doom set, remote_prop_spawn.cpp:1080 -- free win #1.)",
            g_boundChip + g_boundKerfur, g_map.size(), g_boundChip, g_boundKerfur, g_caseI, g_caseII,
            g_ordinal, g_desynced ? " [DESYNCED -- bind stopped early, see the ORDER DESYNC line]" : "");
}

void OnDisconnect() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_map.clear();
    g_armed = false;
    g_ordinal = 0;
    g_desynced = false;
    g_boundChip = g_boundKerfur = g_caseI = g_caseII = 0;
    PT::ClearBoundMirrorNatives();
}

}  // namespace coop::save_identity_bind
