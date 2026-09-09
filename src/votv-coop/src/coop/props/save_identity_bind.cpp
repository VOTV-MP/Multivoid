// coop/props/save_identity_bind.cpp -- the client binds its save-loaded natives to the host's eids
// from the identity map the host sent: chip piles by per-family spawn order at the spawn seam and
// by position at quiescence, off-form kerfurs by their portable save key. See the header.

#include "coop/props/save_identity_bind.h"

#include "coop/element/element.h"          // Element, ElementId, kInvalidId
#include "coop/element/element_deleter.h"  // Enqueue and Flush, for the self-test
#include "coop/element/mirror_manager.h"   // Take, for the churn probe and the self-test
#include "coop/element/prop.h"             // coop::element::Prop (MirrorManager<Prop>)
#include "coop/element/quiescence_drain.h"  // the pending twins and position corrections
#include "coop/element/registry.h"         // Registry::Get().Get(eid)
#include "coop/config/config.h"               // ResolveFlag
#include "coop/creatures/kerfur_entity.h"            // IsKerfurPropClass
#include "coop/props/join_membership_sweep.h"    // HasLoadTailQuiesced
#include "coop/props/prop_element_tracker.h"     // UnmarkKnownKeyedProp, GetPropElementIdForActor, MarkBoundMirrorNative
#include "coop/props/remote_prop.h"              // RegisterPropMirror, ConsumeLocalActor
#include "coop/props/save_time_retire_util.h"    // FindExactMatch, the shared 1 cm kernel
#include "coop/props/trash_proxy.h"              // IsProxy, RetireProxy
#include "coop/props/trash_channel.h"            // CtxForEid: was the eid converted in-window
#include "ue_wrap/engine/engine.h"                // GetActorLocation
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"                  // GetInteractableKeyString, IsChipPile
#include "ue_wrap/core/reflection.h"            // ClassNameOf, IsLive, ClassOf

#include <mutex>
#include <string>
#include <vector>

namespace coop::save_identity_bind {
namespace {

namespace R   = ue_wrap::reflection;
namespace PT  = coop::prop_element_tracker;
namespace MAP = coop::save_identity_map;

std::mutex g_mu;  // SetReceivedMap (net thread) vs OnSaveLoadSpawn (game thread)
MAP::IdMap g_map;          // the full received map (the total count and the per-family split)

// The bind keys on the save array index, per family. Each replay loop spawns and finishes its
// actor in the same iteration, so the k-th client chipPile spawn is primitivesData[k] and the
// k-th off-kerfur spawn is objectsData[k]; only the order between the two arrays varies run to
// run, which a global ordinal mis-paired. A per-family cursor isolates each array, the same
// cross-peer anchor the host uses (the same blob gives the same index on both peers).
std::vector<MAP::IdEntry> g_chipEntries;    // map entries with family==ChipPile, in array index order
std::vector<MAP::IdEntry> g_kerfurEntries;  // map entries with family==KerfurOff, in array index order
bool       g_armed = false;
size_t     g_chipCursor   = 0;  // next chipPile spawn's index into g_chipEntries (keyless ordinal bind)
// The keyed family has no cursor: it binds by key, never by spawn order (a cursor floated under
// churn and bound the same kerfur to different eids on the two peers).

// The hole probe ([dev] force_kerfur_unmap): one kerfur entry is dropped at arm time, so a run
// verifies hole tolerance rather than the happy path. That kerfur must stay unbound while every
// other kerfur still binds to its own eid; EmitBindSummary asserts the dropped eid was never
// bound.
bool        g_holeInjected = false;
std::wstring g_holeKey;            // the key of the dropped kerfurOff entry (must stay unbound)
uint32_t    g_holeEid = 0;        // its host eid (no other native may bind it)

// The per-join bind tallies, for the quiescence summary.
int g_boundChip = 0, g_boundKerfur = 0;
int g_caseI = 0, g_caseII = 0;
int g_overflowChip = 0;  // chipPile spawns beyond the mapped count; the keyed family has no cursor to overflow

const char* FamName(MAP::Family f) { return f == MAP::Family::ChipPile ? "chipPile" : "kerfurOff"; }

// The kerfurOff map entry whose portable save key matches `nativeKey`. The off-kerfur's key is
// intrinsic and identical on both peers, so a key-paired eid is cross-peer stable; an untracked
// kerfur (no entry) returns null without shifting any other pairing. The caller holds g_mu.
const MAP::IdEntry* FindKerfurEntryByKey_(const std::wstring& nativeKey) {
    if (nativeKey.empty() || nativeKey == L"None") return nullptr;
    for (const MAP::IdEntry& e : g_kerfurEntries)
        if (!e.key.empty() && e.key == nativeKey) return &e;
    return nullptr;
}

// The bind: retire the native's peer-range local element, then install a host-range mirror at E
// onto the native. The caller holds g_mu. `consumeDisplaced` false means the actor displaced
// from E belongs to a different identity (a recycle smear): it stays alive and unbound, for its
// own identity to re-claim.
void BindLocalNativeToHostEid_(void* native, coop::element::ElementId E, MAP::Family family, size_t k,
                               bool consumeDisplaced = true) {
    // The class and key for the mirror element: a chipPile is keyless, an off-kerfur carries its
    // real key so a later keyed resolution still finds it.
    const std::wstring cls = R::ClassNameOf(native);
    std::wstring key;
    if (family == MAP::Family::KerfurOff) {
        const std::wstring k2 = ue_wrap::prop::GetInteractableKeyString(native);
        if (k2 != L"None") key = k2;
    }

    // The collision case, classified before anything mutates.
    coop::element::Element* preE = coop::element::Registry::Get().Get(E);
    if (preE && !preE->IsMirror()) {
        // A host-range eid resolving to a local element is impossible by the range split; refused
        // rather than rebinding a local onto the native, which would break the host-range-mirror
        // invariant the bound guard assumes.
        UE_LOGE("save_identity_bind: host eid=%u unexpectedly resolves to a LOCAL element on the client -- "
                "REFUSING to bind native=%p (range-split invariant broken?). k=%zu", static_cast<unsigned>(E),
                native, k);
        return;
    }
    void* oldActor = preE ? preE->GetActor() : nullptr;
    const bool sameActor = (oldActor == native);
    // IsLiveByIndex, never IsLive: a purge frees the row-held actor's memory while the row lingers,
    // and IsLive on freed memory can read true.
    const bool caseII = (preE && oldActor && !sameActor &&
                         R::IsLiveByIndex(oldActor, preE->GetInternalIdx()));  // host PropSpawn beat the bind

    // Convert wins when a convert already touched E: the host grabbed and moved this pile in the
    // join window, so E's current rendering (a trash proxy, or the native a landing materialised)
    // is the authoritative form at the moved position, and this save-loaded native at the stale
    // save position is the redundant one. E stays bound; the fresh native is retired. A clean-join
    // pile has no in-window convert and takes the plain bind; a fresh host spawn with no convert is
    // the race below, where the native at its untouched position wins.
    if (caseII && family == MAP::Family::ChipPile &&
        coop::trash_channel::CtxForEid(E) > 0 &&
        (coop::trash_proxy::IsProxy(E) || PT::IsBoundMirrorNative(oldActor))) {
        const unsigned ctx = coop::trash_channel::CtxForEid(E);
        const bool viaProxy = coop::trash_proxy::IsProxy(E);
        PT::UnmarkKnownKeyedProp(native);                  // drop the fresh native's local element
        coop::remote_prop::ConsumeLocalActor(native);      // the redundant native at the save position
        ++g_boundChip;                                     // E is satisfied
        ++g_caseII;
        UE_LOGI("save_identity_bind: CONVERT-WINS k=%zu chipPile freshNative=%p -> host eid=%u [case(ii)-converted: "
                "pile grabbed/moved in-window (ctx=%u) -> %s authoritative @new, redundant save-loaded native@old retired]",
                k, native, static_cast<unsigned>(E), ctx, viaProxy ? "proxy" : "landed native");
        return;
    }

    // Free the native's local element (a no-op before the post-load seed minted one): the local
    // Prop Element, the reverse map and the key index go, and the actor stays alive as the mirror's
    // rendering.
    PT::UnmarkKnownKeyedProp(native);
    // The proxy-before-bind race: a host PropSpawn beat the bind and spawned a trash proxy at E, so
    // the proxy is retired properly first (destroy, un-root, unbind), before the mirror install
    // below, which then binds the native to a free E.
    bool retiredProxy = false;
    if (caseII && family == MAP::Family::ChipPile && coop::trash_proxy::IsProxy(E)) {
        coop::trash_proxy::RetireProxy(E);
        retiredProxy = true;
    }
    // The host-range mirror at E onto the native; rebindInPlace re-points an already-bound E rather
    // than rejecting.
    coop::remote_prop::RegisterPropMirror(E, native, key, cls, /*senderSlot*/ 0, /*rebindInPlace*/ true);
    // The Element at E is flagged as a save-loaded native; the flag lives with the identity, so it
    // cannot desync from the binding.
    if (coop::element::Element* el = coop::element::Registry::Get().Get(E)) el->SetSaveNative(true);
    // The non-proxy race: a host PropSpawn already spawned a separate actor at E, orphaned by the
    // rebind, so it is destroyed echo-suppressed. Skipped when the proxy path tore it down, and
    // when the caller declared it a foreign identity.
    if (caseII && !retiredProxy && consumeDisplaced) coop::remote_prop::ConsumeLocalActor(oldActor);

    if (family == MAP::Family::ChipPile) ++g_boundChip; else ++g_boundKerfur;
    if (caseII) ++g_caseII; else ++g_caseI;

    // Logged: every kerfur, every race, and the first five piles. No position here: the seam runs
    // before FinishSpawning, when position and chip type are not initialised.
    if (caseII || family == MAP::Family::KerfurOff || k < 5) {
        UE_LOGI("save_identity_bind: BOUND k=%zu %s native=%p -> host eid=%u [%s]%s", k, FamName(family),
                native, static_cast<unsigned>(E),
                caseII ? (consumeDisplaced
                              ? "case(ii) host-PropSpawn-beat-bind: rebindInPlace + echo-destroyed redundant actor"
                              : "case(ii) wrong-occupant heal: rebindInPlace, displaced FOREIGN actor kept alive")
                       : (sameActor ? "case re-entrant (E already this native)" : "case(i) E free: fresh mirror"),
                family == MAP::Family::KerfurOff ? "  <- jUuC off-kerfur bound by eid (K-5-clean: host-range mirror, no client mint)" : "");
    }
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::save_identity_bind);
    return s;
}

void SetReceivedMap(const MAP::IdMap& map) {
    if (!IsEnabled()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_map = map;            // copy (the source is the client's transient receive buffer)
    // The map split into the two per-family lists in map order, which is the save array order.
    g_chipEntries.clear();
    g_kerfurEntries.clear();
    g_chipEntries.reserve(g_map.size());
    g_kerfurEntries.reserve(8);
    for (const MAP::IdEntry& e : g_map) {
        if (e.family == static_cast<uint8_t>(MAP::Family::ChipPile)) g_chipEntries.push_back(e);
        else                                                          g_kerfurEntries.push_back(e);
    }
    g_chipCursor = 0;
    g_armed = true;
    g_boundChip = g_boundKerfur = g_caseI = g_caseII = 0;
    g_overflowChip = 0;
    // The hole probe: the last kerfur entry is dropped, so its native finds no key match.
    g_holeInjected = false; g_holeKey.clear(); g_holeEid = 0;
    static const bool s_forceUnmap =
        coop::config::ResolveFlag(::coop::config_registry::rows::force_kerfur_unmap);
    if (s_forceUnmap && !g_kerfurEntries.empty()) {
        const MAP::IdEntry dropped = g_kerfurEntries.back();
        g_kerfurEntries.pop_back();
        g_holeInjected = true; g_holeKey = dropped.key; g_holeEid = dropped.eid;
        UE_LOGW("save_identity_bind: [force_kerfur_unmap] HOLE INJECTED -- dropped kerfurOff key='%ls' eid=%u from "
                "the map. That kerfur MUST stay UNBOUND; no other kerfur may bind eid=%u (hole-tolerance test).",
                g_holeKey.c_str(), g_holeEid, g_holeEid);
    }
    UE_LOGI("save_identity_bind: ARMED with %zu-entry host eid map (%zu chipPile [ordinal bind] + %zu kerfurOff "
            "[KEY bind, sidecar v3]). chipPile keyless->cursor; kerfurOff keyed->key-match (cross-peer-stable "
            "eid). [dev] save_identity_bind=1", g_map.size(), g_chipEntries.size(), g_kerfurEntries.size());
}

void OnSaveLoadSpawn(void* newActor, MAP::Family family) {
    if (!newActor || !IsEnabled()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_armed) return;
    // A double fire of the thunk for a native already bound is ignored.
    if (PT::IsBoundMirrorNative(newActor)) return;

    if (family == MAP::Family::KerfurOff) {
        // The keyed family: native to eid by key, never by cursor.
        const std::wstring nkey = ue_wrap::prop::GetInteractableKeyString(newActor);
        const MAP::IdEntry* e = FindKerfurEntryByKey_(nkey);
        if (!e) {
            // No host entry for this key: either the key is not readable yet at this
            // pre-FinishSpawning seam, or the kerfur is untracked on the host. The quiescence sweep
            // retries the key match once the key is guaranteed readable.
            UE_LOGI("save_identity_bind: kerfurOff native=%p key='%ls' -- no map entry at seam; deferring to the "
                    "quiescence key-match (key not ready pre-FinishSpawning, or untracked-on-host)",
                    newActor, nkey.c_str());
            return;
        }
        if (e->eid == 0u || e->eid == coop::element::kInvalidId) {
            UE_LOGW("save_identity_bind: kerfurOff key='%ls' map entry has invalid eid=%u -- skipping (no bind)",
                    nkey.c_str(), e->eid);
            return;
        }
        BindLocalNativeToHostEid_(newActor, static_cast<coop::element::ElementId>(e->eid),
                                  MAP::Family::KerfurOff, e->index);
        return;
    }

    // The keyless family: the k-th chip spawn binds to the k-th chip entry. Position is not yet set
    // at this seam, so order is the only signal here; the position re-bind at quiescence is the
    // backstop for GC churn.
    if (g_chipCursor >= g_chipEntries.size()) {
        if (g_overflowChip == 0)
            UE_LOGW("save_identity_bind: chipPile keyless spawn beyond the mapped %zu chipPile entries -- NOT "
                    "binding this or further chipPile spawns (per-family count mismatch)", g_chipEntries.size());
        ++g_overflowChip;
        return;
    }
    const MAP::IdEntry& e = g_chipEntries[g_chipCursor];
    if (e.eid == 0u || e.eid == coop::element::kInvalidId) {
        UE_LOGW("save_identity_bind: chipPile map entry k=%zu (array index=%u) has invalid eid=%u -- skipping",
                g_chipCursor, e.index, e.eid);
        ++g_chipCursor;
        return;
    }
    BindLocalNativeToHostEid_(newActor, static_cast<coop::element::ElementId>(e.eid), MAP::Family::ChipPile,
                              g_chipCursor);
    ++g_chipCursor;
}

void EmitBindSummary() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_armed) return;
    UE_LOGW("save_identity_bind: BIND SUMMARY -- bound %d/%zu map entries (%d/%zu chipPile [ordinal] + %d/%zu "
            "kerfurOff [KEY, sidecar v3]); case(i) E-free=%d, case(ii) race-rebind=%d; chip cursor=%zu. (bound "
            "natives are host-range MIRRORs -> excluded from the divergence sweep doom set, "
            "remote_prop_spawn.cpp:1080 -- free win #1. kerfurOff eid is now cross-peer-stable via key-match.)",
            g_boundChip + g_boundKerfur, g_map.size(), g_boundChip, g_chipEntries.size(), g_boundKerfur,
            g_kerfurEntries.size(), g_caseI, g_caseII, g_chipCursor);
    if (g_overflowChip > 0)
        UE_LOGW("save_identity_bind: OVERFLOW -- %d chipPile spawn(s) exceeded the mapped count (per-family count "
                "mismatch -- investigate the save vs the map)", g_overflowChip);
    // The hole verdict: the dropped eid must be unbound, by the hole kerfur or by any other.
    if (g_holeInjected) {
        coop::element::Element* el =
            coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(g_holeEid));
        void* boundActor = el ? el->LiveActor() : nullptr;  // slot-validated
        const bool free = (boundActor == nullptr);
        UE_LOGW("save_identity_bind: [force_kerfur_unmap] HOLE VERDICT=%s -- dropped kerfurOff key='%ls' eid=%u "
                "boundActor=%p. %s",
                free ? "PASS" : "FAIL", g_holeKey.c_str(), g_holeEid, boundActor,
                free ? "the unmapped kerfur stayed unbound + no other kerfur stole its eid (key-match is hole-"
                       "tolerant -- a rank/cursor scheme would have shifted a wrong native onto this eid)"
                     : "an actor bound the dropped eid -- HOLE-TOLERANCE BROKEN (a shift mis-bound a native; this "
                       "is the regression class the key-match was meant to kill)");
    }
}

// The quiescence re-bind sweep, for natives left unbound after the seam: the engine's
// incremental GC sporadically destroys and re-creates a few mid-join, and a kerfur's key may
// not have been readable at the seam. At quiescence both intrinsic identities are observable: a
// chipPile by an exact 1 cm position match against the host's save position, a kerfur by its
// key. Game thread.
int BindUnboundReCreates(bool* ghostRetireDrained) {
    if (ghostRetireDrained) *ghostRetireDrained = true;  // default: the tail left nothing standing
    if (!IsEnabled()) return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_armed) return 0;  // not a coop join with a received map -> nothing to re-bind

    // A fresh walk of the live unbound natives of both families, excluding bound mirrors, with the
    // internal index captured for the match's liveness check.
    struct LiveNative { void* actor; int32_t idx; float x, y, z; };
    std::vector<LiveNative> chipN, kerfurN;
    size_t chipBound = 0;  // live BOUND chip natives (the GHOST-RETIRE >50% valve's denominator)
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o)) continue;
        // The allocation-free class filter first: a name read per object over the whole array,
        // every reconcile pass, was the pump's dominant spike. Only the few hundred that pass pay
        // for a name.
        const bool isChip = ue_wrap::prop::IsChipPile(o);
        bool isKerfur = false;
        if (!isChip) { void* c = R::ClassOf(o); isKerfur = c && coop::kerfur_entity::IsKerfurPropClass(c); }
        if (!isChip && !isKerfur) continue;                                // the cheap class filter
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;       // CDO
        if (PT::IsBoundMirrorNative(o)) { if (isChip) ++chipBound; continue; }  // bound: the ghost valve's denominator
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(o);
        (isChip ? chipN : kerfurN).push_back({o, R::InternalIndexOf(o), loc.X, loc.Y, loc.Z});
    }
    if (chipN.empty() && kerfurN.empty()) return 0;  // no unbound natives -> nothing to do (the common case)

    int rebound = 0;
    // An eid is free to re-bind iff no live actor occupies it. IsLiveByIndex, never IsLive: after a
    // purge the row holds a freed pointer, and IsLive on it can read true, so the eid would read
    // occupied for good and the re-bind would never happen.
    auto eidFree = [](uint32_t eid) {
        coop::element::Element* el = coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(eid));
        void* boundActor = el ? el->GetActor() : nullptr;
        return !(el && boundActor && R::IsLiveByIndex(boundActor, el->GetInternalIdx()));
    };

    // The chipPile arm, by position. Each entry carries an immutable save position (where the game
    // re-creates) and a host-position overlay (where the host says E is). (a) E free: phase 1
    // matches at the host position (a churned mirror's surviving actor), excluding any native
    // within 1 cm of a free entry's save position, which is that entry's own re-create; phase 2
    // matches at the save position (a purge re-create always spawns there), and if the host
    // position is far away the pending vacate twin is cancelled and a position correction is
    // queued. (b) E bound far from its save position: an unbound native still there is the leftover
    // twin of a join-window move, so a save-time twin is armed and the sweep retires it per eid.
    {
        constexpr float kMovedCm2 = 50.f * 50.f;  // >50cm from save-pos = E genuinely moved @new (matches the sweep)
        std::vector<bool> used(chipN.size(), false);
        int dupArmed = 0;
        // The pre-pass: every free entry's save position, the phase-1 exclusion set. Small, so a
        // linear check.
        std::vector<ue_wrap::FVector> freeSavePos;
        for (const MAP::IdEntry& e : g_chipEntries) {
            if (e.eid == 0u || e.eid == coop::element::kInvalidId) continue;
            if (eidFree(e.eid)) freeSavePos.push_back(ue_wrap::FVector{e.savePosX, e.savePosY, e.savePosZ});
        }
        auto nearAnyFreeSavePos = [&freeSavePos](const LiveNative& nv) {
            for (const ue_wrap::FVector& s : freeSavePos) {
                const float dx = nv.x - s.X, dy = nv.y - s.Y, dz = nv.z - s.Z;
                if (dx * dx + dy * dy + dz * dz <= coop::save_time_retire_util::kExactMatchR2Cm) return true;
            }
            return false;
        };
        for (const MAP::IdEntry& e : g_chipEntries) {
            if (e.eid == 0u || e.eid == coop::element::kInvalidId) continue;
            const ue_wrap::FVector saveKey{e.savePosX, e.savePosY, e.savePosZ};
            coop::element::Element* el =
                coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(e.eid));
            void* bound = el ? el->GetActor() : nullptr;
            const bool boundLive = el && bound && R::IsLiveByIndex(bound, el->GetInternalIdx());
            if (!boundLive) {
                // (a) E free: phase 1, the host position (a churn survivor).
                if (e.hasHostPos) {
                    const ue_wrap::FVector hostKey{e.hostPosX, e.hostPosY, e.hostPosZ};
                    const int hIdx = coop::save_time_retire_util::FindExactMatch(
                        chipN, used, hostKey,
                        [&](const LiveNative& nv) { return !nearAnyFreeSavePos(nv); });
                    if (hIdx >= 0) {
                        used[hIdx] = true;
                        BindLocalNativeToHostEid_(chipN[hIdx].actor,
                                                  static_cast<coop::element::ElementId>(e.eid),
                                                  MAP::Family::ChipPile, e.index);
                        ++rebound;
                        UE_LOGI("save_identity_bind: RE-BIND chipPile by HOST pos -- native=%p -> host eid=%u "
                                "@host=(%.1f,%.1f,%.1f) (churned mirror's surviving actor; @save stays the "
                                "re-create key)", chipN[hIdx].actor, e.eid, e.hostPosX, e.hostPosY, e.hostPosZ);
                        continue;
                    }
                }
                // Phase 2, the save position (a purge re-create).
                const int idx = coop::save_time_retire_util::FindExactMatch(
                    chipN, used, saveKey, [](const LiveNative&) { return true; });  // position-only (1cm exact)
                if (idx < 0) continue;  // 0 (no re-create -- normal) or >1 (ambiguous co-located -> skip)
                used[idx] = true;
                BindLocalNativeToHostEid_(chipN[idx].actor, static_cast<coop::element::ElementId>(e.eid),
                                          MAP::Family::ChipPile, e.index);
                ++rebound;
                UE_LOGI("save_identity_bind: RE-BIND chipPile by position -- native=%p -> host eid=%u @save-pos="
                        "(%.1f,%.1f,%.1f) (GC-churned re-create; authoritative host-wire position match)",
                        chipN[idx].actor, e.eid, e.savePosX, e.savePosY, e.savePosZ);
                if (e.hasHostPos) {
                    const float hdx = e.hostPosX - e.savePosX, hdy = e.hostPosY - e.savePosY,
                                hdz = e.hostPosZ - e.savePosZ;
                    if (hdx * hdx + hdy * hdy + hdz * hdz > kMovedCm2) {
                        coop::element::quiescence_drain::CancelPendingSaveTimeTwin(
                            static_cast<coop::element::ElementId>(e.eid));
                        coop::element::quiescence_drain::EnsurePosCorrection(
                            static_cast<coop::element::ElementId>(e.eid),
                            ue_wrap::FVector{e.hostPosX, e.hostPosY, e.hostPosZ},
                            ue_wrap::engine::GetActorRotation(chipN[idx].actor));
                    }
                }
                continue;
            }
            // (b) E bound elsewhere: retire the stale twin at the save position if E moved.
            const ue_wrap::FVector bl = ue_wrap::engine::GetActorLocation(bound);
            const float ddx = bl.X - e.savePosX, ddy = bl.Y - e.savePosY, ddz = bl.Z - e.savePosZ;
            if (ddx * ddx + ddy * ddy + ddz * ddz <= kMovedCm2) continue;  // E still @save-pos -> not moved
            const int idx = coop::save_time_retire_util::FindExactMatch(
                chipN, used, saveKey, [](const LiveNative&) { return true; });  // unbound native still @old?
            if (idx < 0) continue;  // no stale orphan @old (E moved cleanly, or ambiguous co-located) -> nothing
            used[idx] = true;
            const uint8_t chipType = ue_wrap::prop::GetChipType(chipN[idx].actor);
            coop::element::quiescence_drain::ArmPendingSaveTimeTwin(
                static_cast<coop::element::ElementId>(e.eid), saveKey, chipType);
            ++dupArmed;
        }
        if (dupArmed)
            UE_LOGI("save_identity_bind: DUP-RETIRE -- armed %d save-time twin(s) from the identity map (eid bound "
                    "@new but a stale UNBOUND native lingers @save-pos = FLOOR-kept mass-move dup) "
                    "-> the sweep retires each per-eid (confirmed -> no cap)", dupArmed);

        // The ghost-retire tail, after quiescence (client-only by construction: the host never
        // fires the join sweep). A live unbound native chipPile that this pass could not claim,
        // sits at no entry's save position and at no free entry's host position is identity-less:
        // nothing on the wire can address it and no binder will claim it. All such are retired
        // here, the single owner.
        if (coop::join_membership_sweep::HasLoadTailQuiesced() && !chipN.empty()) {
            std::vector<size_t> ghostIdx;
            for (size_t i = 0; i < chipN.size(); ++i) {
                if (used[i]) continue;
                const LiveNative& nv = chipN[i];
                if (!R::IsLiveByIndex(nv.actor, nv.idx)) continue;
                if (coop::element::Registry::Get().EidForActor(nv.actor) != coop::element::kInvalidId)
                    continue;  // a bind above claimed it after all -> not a ghost
                bool nearKey = false;
                for (const MAP::IdEntry& e : g_chipEntries) {
                    if (e.eid == 0u || e.eid == coop::element::kInvalidId) continue;
                    const float sdx = nv.x - e.savePosX, sdy = nv.y - e.savePosY, sdz = nv.z - e.savePosZ;
                    if (sdx * sdx + sdy * sdy + sdz * sdz <= coop::save_time_retire_util::kExactMatchR2Cm) {
                        nearKey = true;
                        break;
                    }
                    if (e.hasHostPos && eidFree(e.eid)) {
                        const float hdx = nv.x - e.hostPosX, hdy = nv.y - e.hostPosY, hdz = nv.z - e.hostPosZ;
                        if (hdx * hdx + hdy * hdy + hdz * hdz <=
                            coop::save_time_retire_util::kExactMatchR2Cm) {
                            nearKey = true;
                            break;
                        }
                    }
                }
                if (!nearKey) ghostIdx.push_back(i);
            }
            // The over-50% valve, over all live chip natives: a mass ghost verdict is a racing
            // bracket or a bug, so nothing is retired this pass and the sweep stays armed for a
            // clean re-adjudication.
            const size_t totalChips = chipBound + chipN.size();
            if (!ghostIdx.empty() && ghostIdx.size() * 2 > totalChips) {
                UE_LOGE("save_identity_bind: GHOST-RETIRE VALVE -- %zu ghost verdict(s) out of %zu live "
                        "chip native(s) (>50%%): refusing the mass retire (racing bracket / bug); "
                        "nothing destroyed this pass", ghostIdx.size(), totalChips);
                if (ghostRetireDrained) *ghostRetireDrained = false;
            } else {
                // The per-pass cap: the valve still admits up to half the natives, hundreds of
                // destroys in one tick on a big save. The sweep stays armed until the set is empty,
                // so the drain completes over the reconcile cadence.
                constexpr size_t kGhostRetirePassCap = 40;
                const size_t retireN = ghostIdx.size() < kGhostRetirePassCap
                                           ? ghostIdx.size() : kGhostRetirePassCap;
                for (size_t k = 0; k < retireN; ++k) {
                    const size_t gi = ghostIdx[k];
                    used[gi] = true;
                    UE_LOGW("save_identity_bind: GHOST-RETIRE unbound native chipPile %p @(%.1f,%.1f,%.1f) "
                            "-- no eid, no @save/@host key match post-quiescence (bind-displaced leftover "
                            "/ native-chain product) -> destroyed (wholesale reconcile)",
                            chipN[gi].actor, chipN[gi].x, chipN[gi].y, chipN[gi].z);
                    coop::save_time_retire_util::UnmarkAndDestroy(chipN[gi].actor);
                }
                if (retireN)
                    UE_LOGI("save_identity_bind: GHOST-RETIRE pass -- %zu identity-less native pile(s) "
                            "retired (%zu found, cap %zu/pass; %zu bound + %zu unbound walked)",
                            retireN, ghostIdx.size(), kGhostRetirePassCap, chipBound, chipN.size());
                if (retireN < ghostIdx.size() && ghostRetireDrained) *ghostRetireDrained = false;
            }
        }
    }

    // The kerfurOff arm, by key: each unbound kerfur's portable key resolves its eid directly. An
    // occupied eid whose occupant carries the same key is a seam duplicate the dedup owns; an
    // occupant with a different key is a recycle smear, and the row is rebound onto the key-matched
    // native while the displaced actor is kept alive for its own identity to re-claim.
    int kerfurNoEntry = 0, kerfurDupSkip = 0;
    for (LiveNative& kn : kerfurN) {
        const std::wstring nkey = ue_wrap::prop::GetInteractableKeyString(kn.actor);
        const MAP::IdEntry* e = FindKerfurEntryByKey_(nkey);
        if (!e || e->eid == 0u || e->eid == coop::element::kInvalidId) { ++kerfurNoEntry; continue; }
        if (!eidFree(e->eid)) {
            coop::element::Element* el =
                coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(e->eid));
            void* occ = el ? el->GetActor() : nullptr;  // live by eidFree's IsLiveByIndex check
            const std::wstring occKey = occ ? ue_wrap::prop::GetInteractableKeyString(occ) : std::wstring();
            if (occKey == nkey) { ++kerfurDupSkip; continue; }  // true same-key duplicate -> seam dedup owns it
            BindLocalNativeToHostEid_(kn.actor, static_cast<coop::element::ElementId>(e->eid),
                                      MAP::Family::KerfurOff, e->index, /*consumeDisplaced=*/false);
            ++rebound;
            UE_LOGW("save_identity_bind: RE-BIND kerfurOff WRONG-OCCUPANT heal -- eid=%u row held live actor %p "
                    "key='%ls' (foreign identity; recycle smear) -> rebound to key-matched native=%p key='%ls'; "
                    "displaced actor kept alive (its own identity re-claims it)",
                    e->eid, occ, occKey.c_str(), kn.actor, nkey.c_str());
            continue;
        }
        BindLocalNativeToHostEid_(kn.actor, static_cast<coop::element::ElementId>(e->eid),
                                  MAP::Family::KerfurOff, e->index);
        ++rebound;
        UE_LOGI("save_identity_bind: RE-BIND kerfurOff by KEY -- native=%p key='%ls' -> host eid=%u "
                "(deterministic intrinsic-key bind; no position, no cursor)", kn.actor, nkey.c_str(), e->eid);
    }

    if (rebound > 0 || !chipN.empty() || !kerfurN.empty())
        UE_LOGI("save_identity_bind: quiescence re-bind pass -- %d native(s) re-bound (walked %zu unbound chip "
                "[by position] + %zu unbound kerfur [by key])", rebound, chipN.size(), kerfurN.size());
    if (kerfurNoEntry > 0 || kerfurDupSkip > 0)
        UE_LOGI("save_identity_bind: unbound-kerfur reasons -- %d no/invalid map entry (untracked-on-host "
                "hole; permanent, benign) + %d same-key duplicate (seam dedup owns)", kerfurNoEntry, kerfurDupSkip);
    return rebound;
}

bool UpdateChipHostPos(coop::element::ElementId eid, const ue_wrap::FVector& newPos,
                       ue_wrap::FVector& saveOut) {
    if (!IsEnabled()) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    constexpr float kMovedCm2 = 50.f * 50.f;  // >50cm = a real move (matches the sweep's twin threshold)
    for (MAP::IdEntry& e : g_chipEntries) {
        if (e.eid != static_cast<uint32_t>(eid)) continue;
        // The host's current position is recorded as the overlay; the save position stays
        // immutable. A purge re-create spawns at the game's save position, so a key rewritten to
        // the new position could never match the re-create again and E was unbindable for good. Two
        // positions, two roles: the save position is the re-create key and the vacate-twin key, the
        // host position the phase-1 key and the resurrect protection.
        e.hostPosX = newPos.X; e.hostPosY = newPos.Y; e.hostPosZ = newPos.Z;
        e.hasHostPos = true;
        saveOut = ue_wrap::FVector{e.savePosX, e.savePosY, e.savePosZ};
        const float dx = newPos.X - e.savePosX, dy = newPos.Y - e.savePosY, dz = newPos.Z - e.savePosZ;
        if (dx * dx + dy * dy + dz * dz <= kMovedCm2) return false;  // small nudge -> pos-correction alone
        UE_LOGI("save_identity_bind: host pos OVERLAY eid=%u @save=(%.1f,%.1f,%.1f) (immutable) @host="
                "(%.1f,%.1f,%.1f) (host PropSnapPos authoritative; re-bind = @host first, @save fallback)",
                static_cast<unsigned>(eid), saveOut.X, saveOut.Y, saveOut.Z, newPos.X, newPos.Y, newPos.Z);
        return true;
    }
    return false;  // eid not in the chip identity map (not a keyless save pile / not a map-bearing joiner)
}

void ForceSaveChurnForTest() {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::force_save_churn);
    static bool s_done = false;
    if (s_done) return;  // one-shot: churn once, just before the quiescence sweep
    s_done = true;
    std::lock_guard<std::mutex> lk(g_mu);
    // Every gate is logged, so a run that churns nothing names which gate failed.
    int boundLive = 0;
    for (const MAP::IdEntry& e : g_chipEntries) {
        if (e.eid == 0u || e.eid == coop::element::kInvalidId) continue;
        coop::element::Element* el =
            coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(e.eid));
        if (el && el->LiveActor()) ++boundLive;  // slot-validated
    }
    UE_LOGW("save_identity_bind: [force_save_churn] GATE -- flag=%d armed=%d chipEntries=%zu boundLive=%d",
            s_on ? 1 : 0, g_armed ? 1 : 0, g_chipEntries.size(), boundLive);
    if (!s_on || !g_armed) return;
    constexpr int kChurnN = 3;
    int churned = 0;
    for (const MAP::IdEntry& e : g_chipEntries) {
        if (churned >= kChurnN) break;
        if (e.eid == 0u || e.eid == coop::element::kInvalidId) continue;
        coop::element::Element* el =
            coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(e.eid));
        void* actor = el ? el->LiveActor() : nullptr;  // slot-validated
        if (!actor) continue;  // only churn a currently-bound, live save-native
        // Unbind: the mirror Element is taken (the actor stays alive at its save position), and its
        // destructor clears the registry slot and the reverse map, so the sweep sees an unbound
        // native.
        coop::element::MirrorManager<coop::element::Prop>::Instance().Take(
            static_cast<coop::element::ElementId>(e.eid));
        ++churned;
        UE_LOGW("save_identity_bind: [force_save_churn] UNBOUND chip eid=%u native=%p @save-pos=(%.1f,%.1f,%.1f) "
                "-- synthetic GC churn; variant-1 must re-bind it by position",
                static_cast<unsigned>(e.eid), actor, e.savePosX, e.savePosY, e.savePosZ);
    }
    if (churned)
        UE_LOGW("save_identity_bind: [force_save_churn] unbound %d save-native(s) before the sweep -- expect "
                "%d 'RE-BIND by position' line(s) next", churned, churned);
}

bool RunReseedOrphanSelfTest() {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::reseed_orphan_selftest);
    static bool s_done = false;
    if (!s_on || s_done) return false;
    if (!IsEnabled()) {
        UE_LOGW("save_identity_bind: [reseed_orphan_selftest] requires save_identity_bind=1 (variant-1 gates on it) -- skipping");
        s_done = true;
        return false;
    }
    namespace EL = coop::element;

    // 1. A live unbound chipPile native as the subject; retried next tick until the piles have
    // loaded.
    void* native = nullptr; int32_t nativeIdx = -1; ue_wrap::FVector pos{};
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o)) continue;
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
        if (!ue_wrap::prop::IsChipPile(o)) continue;
        if (PT::IsBoundMirrorNative(o)) continue;  // never disturb an already-bound native
        native = o; nativeIdx = R::InternalIndexOf(o);
        pos = ue_wrap::engine::GetActorLocation(o);
        break;
    }
    if (!native) return false;  // piles not loaded yet -> retry next tick
    s_done = true;              // we have a subject; this is the one real run

    // 2. A free host-range eid for the synthetic bind.
    EL::ElementId hostEid = EL::kInvalidId;
    for (EL::ElementId cand = 32000; cand > 1; --cand) {
        if (EL::Registry::Get().Get(cand) == nullptr) { hostEid = cand; break; }
    }
    if (hostEid == EL::kInvalidId) {
        UE_LOGW("save_identity_bind: [reseed_orphan_selftest] no free host-range eid -- skipping");
        return false;
    }

    // 3. A one-entry map (the native's current position to the eid) and the bind.
    {
        std::lock_guard<std::mutex> lk(g_mu);  // BindLocalNativeToHostEid_ contract: caller holds g_mu
        g_chipEntries.clear(); g_kerfurEntries.clear();
        MAP::IdEntry e{};
        e.eid      = static_cast<uint32_t>(hostEid);
        e.family   = static_cast<uint8_t>(MAP::Family::ChipPile);
        e.index    = 0;
        e.savePosX = pos.X; e.savePosY = pos.Y; e.savePosZ = pos.Z;
        g_chipEntries.push_back(e);
        g_armed = true; g_chipCursor = 0;
        BindLocalNativeToHostEid_(native, hostEid, MAP::Family::ChipPile, 0);
    }
    const bool boundOK = (EL::Registry::Get().Get(hostEid) != nullptr) && PT::IsBoundMirrorNative(native);

    // 4. The churn: the mirror Element taken and enqueued deferred, the reaper's taken-but-not-
    // flushed window.
    auto taken = EL::MirrorManager<EL::Prop>::Instance().Take(hostEid);
    const bool tookIt = (taken != nullptr);
    if (tookIt) EL::ElementDeleter::Get().Enqueue(std::move(taken));
    const EL::ElementId eidBeforeFlush = EL::Registry::Get().EidForActor(native);  // STALE: still maps -> the race

    // 5. The sequence the pump runs at its purge-drain re-seed edge: the flush settles the reverse
    // map, the re-seed, then the re-bind by position.
    EL::ElementDeleter::Get().Flush();
    const EL::ElementId eidAfterFlush = EL::Registry::Get().EidForActor(native);   // expect kInvalid (settled)
    PT::ReSeedKnownKeyedProps();                  // re-seeds the unbound native as a fresh local (the GC-recreate analog)
    const int rebound = BindUnboundReCreates();  // (b): re-bind (chip by position; this self-test is a chipPile)

    // 6. The verdict: the churned native must be re-bound to the eid as a host-range mirror.
    const EL::ElementId finalEid = EL::Registry::Get().EidForActor(native);
    const bool reboundOK = (finalEid == hostEid) && PT::IsBoundMirrorNative(native);
    UE_LOGW("save_identity_bind: [reseed_orphan_selftest] native=%p idx=%d @pos=(%.1f,%.1f,%.1f) hostEid=%u | "
            "bound=%d took=%d eidBeforeFlush=%u(stale) eidAfterFlush=%u(settled) rebound=%d finalEid=%u | VERDICT=%s",
            native, nativeIdx, pos.X, pos.Y, pos.Z, static_cast<unsigned>(hostEid),
            boundOK ? 1 : 0, tookIt ? 1 : 0, static_cast<unsigned>(eidBeforeFlush),
            static_cast<unsigned>(eidAfterFlush), rebound, static_cast<unsigned>(finalEid),
            reboundOK ? "PASS (churned save-native re-bound by position AT the re-seed -- 09:54 orphan closed)"
                      : "FAIL (native orphaned -- re-seed-orphan NOT closed)");

    // 7. Restore: the synthetic binding dropped, and the native re-seeds as a local.
    {
        auto cleanup = EL::MirrorManager<EL::Prop>::Instance().Take(hostEid);
        if (cleanup) EL::ElementDeleter::Get().Enqueue(std::move(cleanup));
        EL::ElementDeleter::Get().Flush();
        std::lock_guard<std::mutex> lk(g_mu);
        g_chipEntries.clear(); g_kerfurEntries.clear(); g_armed = false; g_chipCursor = 0;
    }
    PT::ReSeedKnownKeyedProps();  // re-mark the subject native as a normal host local
    return reboundOK;
}

void OnDisconnect() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_map.clear();
    g_chipEntries.clear();
    g_kerfurEntries.clear();
    g_armed = false;
    g_chipCursor = 0;
    g_boundChip = g_boundKerfur = g_caseI = g_caseII = 0;
    g_overflowChip = 0;
    g_holeInjected = false; g_holeKey.clear(); g_holeEid = 0;
    // No bound-mirror set to clear: the save-native flag lives on the Element and dies with it in
    // the disconnect drain.
}

}  // namespace coop::save_identity_bind
