// coop/save_identity_bind.cpp -- see header. Phase 1 step 2b: CLIENT eid-range BIND (first mutating step).

#include "coop/props/save_identity_bind.h"

#include "coop/element/element.h"          // Element, ElementId, kInvalidId
#include "coop/element/element_deleter.h"  // reseed_orphan_selftest: Enqueue/Flush (reproduce the Take-but-not-Flushed race)
#include "coop/element/mirror_manager.h"   // MirrorManager<Prop>::Take (force_save_churn probe unbind)
#include "coop/element/prop.h"             // coop::element::Prop (MirrorManager<Prop>)
#include "coop/element/registry.h"         // Registry::Get().Get(eid)
#include "coop/session/ini_config.h"               // IsIniKeyTrue
#include "coop/creatures/kerfur_entity.h"            // IsKerfurPropClass (variant-1 position re-bind: kerfur family)
#include "coop/props/prop_element_tracker.h"     // UnmarkKnownKeyedProp, GetPropElementIdForActor, MarkBoundMirrorNative
#include "coop/props/remote_prop.h"              // RegisterPropMirror, ConsumeLocalActor
#include "coop/props/save_time_retire_util.h"    // FindExactMatch (variant-1 position re-bind, shared 1cm kernel)
#include "coop/props/trash_proxy.h"              // (X) item 4: IsProxy / RetireProxy (proxy-before-bind race)
#include "coop/props/trash_channel.h"            // b1 (#2): CtxForEid -- was E converted in-window? (proxy-wins discriminator)
#include "ue_wrap/engine.h"                // GetActorLocation (variant-1 position re-bind)
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"                  // GetInteractableKeyString, IsChipPile
#include "ue_wrap/reflection.h"            // ClassNameOf, IsLive, ClassOf

#include <mutex>
#include <string>
#include <vector>

namespace coop::save_identity_bind {
namespace {

namespace R   = ue_wrap::reflection;
namespace PT  = coop::prop_element_tracker;
namespace MAP = coop::save_identity_map;

std::mutex g_mu;  // SetReceivedMap (net thread) vs OnSaveLoadSpawn (game thread)
MAP::IdMap g_map;          // the full received map (kept for the total count + the per-family split)

// Build 3 (per-family ordinal, 2026-06-26): the bind keys on the saveSlot ARRAY INDEX, not a global spawn
// ordinal. RE (loadObjects/Load Primitives bytecode) proved: each replay loop is a synchronous in-loop BP
// for-loop (BeginDeferred->FinishSpawning same iteration, no async), so the k-th client chipPile spawn IS
// primitivesData[k] and the k-th off-kerfur spawn IS objectsData[k] -- the within-array spawn order == the
// array index order BY CONSTRUCTION. The ONLY disorder was CROSS-array: the client spawns the two arrays as
// two phases (Load Primitives vs the objectsData loop) whose RELATIVE order varies run-to-run, while the host
// map concatenates [objectsData(kerfur) -> primitivesData(chip)] in a fixed order. A GLOBAL ordinal therefore
// mis-pairs the moment the phases land out of the map's order (observed: k=0 chipPile vs map[0] kerfurOff ->
// disarm). Splitting the cursor by FAMILY isolates each array, so the per-family rank == the array index
// regardless of which phase spawns first. This is the same stable cross-peer anchor Path A uses host-side
// (same blob -> same array index on both peers), accessed via the proven-deterministic within-array order --
// NOT position, zero transport change, no co-located ambiguity. [[lesson-chippile-saved-in-primitivesData-not-objectsData]]
std::vector<MAP::IdEntry> g_chipEntries;    // map entries with family==ChipPile, in array index order
std::vector<MAP::IdEntry> g_kerfurEntries;  // map entries with family==KerfurOff, in array index order
bool       g_armed = false;
size_t     g_chipCursor   = 0;  // next chipPile spawn's index into g_chipEntries (keyless ordinal bind)
// (kerfurOff has NO cursor since sidecar v3: the keyed family binds by KEY, not by spawn order -- see
// FindKerfurEntryByKey_ / OnSaveLoadSpawn. The cursor floated under churn = the 15:55 retire regression root.)

// DEV PROBE state ([dev] force_kerfur_unmap, RULE-2-exempts-probes): deterministically inject the jUuC HOLE
// case (a kerfurOff untracked on the host -> NO map entry) so a hands-on can verify HOLE-TOLERANCE, not just
// the happy path. At arm time we drop ONE kerfur entry; that kerfur's native must then stay UNBOUND while every
// OTHER kerfur still key-binds to its correct eid (no rank/cursor shift). EmitBindSummary asserts the dropped
// eid was never bound. The 15:55 regression was ON a hole -- a happy-path test would miss a hole-shift regression.
bool        g_holeInjected = false;
std::wstring g_holeKey;            // the key of the dropped kerfurOff entry (must stay unbound)
uint32_t    g_holeEid = 0;        // its host eid (no other native may bind it)

// per-join bind tallies (for the quiescence summary)
int g_boundChip = 0, g_boundKerfur = 0;
int g_caseI = 0, g_caseII = 0;
int g_overflowChip = 0;  // chipPile spawns beyond the mapped count (unexpected -- logged). No kerfur overflow:
                         // the keyed family binds by key (no cursor to overflow).

const char* FamName(MAP::Family f) { return f == MAP::Family::ChipPile ? "chipPile" : "kerfurOff"; }

// sidecar v3 (2026-06-29): find the kerfurOff map entry whose PORTABLE save key matches `nativeKey`. The off-
// kerfur key is intrinsic + cross-peer-stable (same blob both peers) -> pairing native<->eid by key makes the
// bound eid cross-peer-stable, the root fix for the 2026-06-29 15:55 retire regression (a load-order cursor
// floated -> the same physical kerfur bound different eids across peers -> retire-by-eid killed the wrong one;
// [[lesson-eid-not-cross-peer-stable-loadorder-bind]]). HOLE-TOLERANT: an off-kerfur untracked on the host (no
// entry, e.g. jUuC) returns nullptr WITHOUT shifting any other kerfur's pairing -- the exact failure a rank/
// cursor scheme cannot survive. Caller holds g_mu. Returns nullptr for an empty/None key or no match.
const MAP::IdEntry* FindKerfurEntryByKey_(const std::wstring& nativeKey) {
    if (nativeKey.empty() || nativeKey == L"None") return nullptr;
    for (const MAP::IdEntry& e : g_kerfurEntries)
        if (!e.key.empty() && e.key == nativeKey) return &e;
    return nullptr;
}

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

    // b1 (#2, 2026-06-26): PROXY-WINS when a convert already touched E. If a trash proxy beat the bind AND a
    // PropConvert was adopted for E (CtxForEid>0), the host grabbed+MOVED this pile IN the join window: the
    // proxy absorbed the ToClump/ToPile and IS the authoritative current rendering (at the moved position/
    // form), while the save-loaded native loaded at the now-STALE save position -> it is the redundant dup.
    // Keep E bound to the proxy; retire the native (drain its local element + echo-suppressed destroy). This
    // is the morph-hand-off shape (#3) inverted for the converted save-loaded case. Distinct from the X item-4
    // race BELOW (a FRESH host-PropSpawn proxy, no convert -> CtxForEid==0 -> the native at its untouched save
    // position wins, RetireProxy + bind native). Isolation: clean-join save-loaded piles get no in-window
    // convert -> CtxForEid==0 -> unchanged; only a pile converted in-window before its bind reaches here.
    if (caseII && family == MAP::Family::ChipPile && coop::trash_proxy::IsProxy(E) &&
        coop::trash_channel::CtxForEid(E) > 0) {
        const unsigned ctx = coop::trash_channel::CtxForEid(E);
        PT::UnmarkKnownKeyedProp(native);                  // drain the native's peer-range LOCAL element (if any)
        coop::remote_prop::ConsumeLocalActor(native);      // echo-suppressed destroy of the redundant stale-pos native
        ++g_boundChip;                                     // E IS satisfied (bound to the proxy) -> count for the 874/874 summary
        ++g_caseII;
        UE_LOGI("save_identity_bind: PROXY-WINS k=%zu chipPile native=%p -> host eid=%u [case(ii)-converted: "
                "pile grabbed/moved in-window (ctx=%u) -> proxy authoritative, redundant save-loaded native retired]",
                k, native, static_cast<unsigned>(E), ctx);
        return;
    }

    // Step 3.1: free the native's peer-range LOCAL element (no-op if the post-load seed hasn't minted one yet).
    // UnmarkKnownKeyedProp drains the local Prop Element + erases g_actorToPropElementId + the key index,
    // leaving the actor ALIVE (it becomes the mirror's rendering). Identical to the position-adopt retire.
    PT::UnmarkKnownKeyedProp(native);
    // (ii) proxy-before-bind race (X item 4): if a host PropSpawn beat the bind and spawned a trash PROXY at
    // E (a chipPile mirror), retire it PROPERLY *before* re-binding. RetireProxy does Destroy -> RemoveFromRoot
    // -> Take(E)/unbind, so it MUST precede RegisterPropMirror (otherwise Take(E) would destroy the element we
    // just bound to the native), and it REPLACES ConsumeLocalActor (which only destroys the actor -> would leak
    // the rooted proxy + its g_proxies entry). After RetireProxy, E is free in both the proxy map and the
    // Registry, so the RegisterPropMirror below fresh-binds the native cleanly.
    bool retiredProxy = false;
    if (caseII && family == MAP::Family::ChipPile && coop::trash_proxy::IsProxy(E)) {
        coop::trash_proxy::RetireProxy(E);
        retiredProxy = true;
    }
    // Step 3.2: install the host-range MIRROR at E onto the native. rebindInPlace=true handles the (ii) race
    // (re-points an already-bound E onto the native instead of HEAD-rejecting). senderSlot=0 (host owns it).
    coop::remote_prop::RegisterPropMirror(E, native, key, cls, /*senderSlot*/ 0, /*rebindInPlace*/ true);
    // (D1 enabler, sync-refactor 2026-06-27): flag the Element at E as a save-loaded native. Now that E is
    // bound to the native (RegisterPropMirror above wrote the unified reverse), the flag lives WITH the
    // identity -- IsBoundMirrorNative reads it via the Element, so it can never desync from the binding the
    // way the retired g_boundMirrorNatives set could (the 15:01:49 morphBoundNative=false root). Set
    // unconditionally on the bound Element (case(i) fresh + case(ii) rebind both land here with E->native).
    if (coop::element::Element* el = coop::element::Registry::Get().Get(E)) el->SetSaveNative(true);
    // (ii) non-proxy race (kerfur, or any non-proxy actor at E): a host PropSpawn already spawned a SEPARATE
    // fresh actor at E -> after the rebind that actor is orphaned (element-less); echo-destroy it so no dup
    // survives (mini-design S4(ii)). Skipped when the proxy path above already tore the proxy down.
    if (caseII && !retiredProxy) coop::remote_prop::ConsumeLocalActor(oldActor);

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
    // Build 3: split the map into the two per-family lists, PRESERVING map order (== saveSlot array index
    // order, since BuildHostMap walks objectsData then primitivesData in array index order). The k-th entry of
    // each list is that family's array index k.
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
    // [dev] force_kerfur_unmap: deterministically inject the jUuC HOLE -- drop the LAST kerfurOff entry so its
    // native finds no key-match (== untracked-on-host). Hole-tolerance verify: that kerfur stays unbound, every
    // OTHER kerfur binds correctly, no other native steals its eid (checked in EmitBindSummary).
    g_holeInjected = false; g_holeKey.clear(); g_holeEid = 0;
    if (coop::ini_config::IsIniKeyTrue("force_kerfur_unmap") && !g_kerfurEntries.empty()) {
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
    // Ignore a double-fire of the thunk for a native we already bound (1A saw the thunk can re-fire at a recycled
    // address). Already-bound -> no double-bind.
    if (PT::IsBoundMirrorNative(newActor)) return;

    if (family == MAP::Family::KerfurOff) {
        // KEYED family (sidecar v3): pair native<->eid BY KEY, NEVER by cursor. The cursor floated under async-
        // load/GC churn -> the same physical kerfur bound different eids across peers -> the 15:55 retire killed
        // the wrong one. The key is intrinsic + portable (same blob) -> the bound eid is cross-peer-stable.
        const std::wstring nkey = ue_wrap::prop::GetInteractableKeyString(newActor);
        const MAP::IdEntry* e = FindKerfurEntryByKey_(nkey);
        if (!e) {
            // No host entry for this key. Either the key isn't readable yet at this PRE-FinishSpawning seam, or
            // the off-kerfur is untracked on the host (e.g. jUuC). Do NOT cursor-bind (that was the bug) -- the
            // quiescence sweep (BindUnboundReCreates) re-tries the key-match once the key is guaranteed readable.
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

    // KEYLESS family (chipPile): per-family ordinal cursor. Build 3: the k-th chip spawn binds to chipEntries[k]
    // == saveSlot primitivesData[k] (RE: within-array spawn order == array index by construction). A genuinely
    // keyless native -- no intrinsic key to match; position is (0,0,0) at this PRE-FinishSpawning seam, so order
    // is the only observable signal here (the position re-bind at quiescence is the GC-churn backstop below).
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
    // [dev] force_kerfur_unmap hole-tolerance verdict: the dropped eid must be UNBOUND (no native -- not the
    // hole kerfur itself, and crucially not some OTHER kerfur that a rank/cursor shift would have mis-bound here).
    if (g_holeInjected) {
        coop::element::Element* el =
            coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(g_holeEid));
        void* boundActor = el ? el->GetActor() : nullptr;
        const bool free = !(boundActor && R::IsLive(boundActor));
        UE_LOGW("save_identity_bind: [force_kerfur_unmap] HOLE VERDICT=%s -- dropped kerfurOff key='%ls' eid=%u "
                "boundActor=%p. %s",
                free ? "PASS" : "FAIL", g_holeKey.c_str(), g_holeEid, boundActor,
                free ? "the unmapped kerfur stayed unbound + no other kerfur stole its eid (key-match is hole-"
                       "tolerant -- a rank/cursor scheme would have shifted a wrong native onto this eid)"
                     : "an actor bound the dropped eid -- HOLE-TOLERANCE BROKEN (a shift mis-bound a native; this "
                       "is the regression class the key-match was meant to kill)");
    }
}

// Quiescence re-bind sweep (2026-06-27 variant-1, extended to key-match 2026-06-29). Catches save natives left
// UNBOUND after the seam: UE's incremental GC sporadically destroys + re-instantiates a SPARSE handful mid-join
// (12:29: 2 of 870), which re-create UNBOUND (the 09:54/11:32 ghost); kerfurs whose key wasn't readable at the
// PRE-FinishSpawning seam also land here. At quiescence the natives are fully spawned, so BOTH intrinsic keys
// are now observable -- and each family binds by ITS intrinsic identity:
//   - chipPile  -> POSITION (genuinely keyless): an exact (1cm) match against the authoritative host-wire save
//     position (sidecar v2). ORDER/COUNT/TIMING-independent. (Pile master-fix = promote this to PRIMARY; today
//     the cursor is primary at the seam and this is the sparse-churn backstop.) Reuses FindExactMatch.
//   - kerfurOff -> KEY (sidecar v3): the portable save key, GUARANTEED readable post-FinishSpawning. This is the
//     keyed family's PRIMARY+GUARANTEED bind -- no fuzzy radius, no cursor float, hole-tolerant. NEVER position.
// Game thread (the sweep).
int BindUnboundReCreates() {
    if (!IsEnabled()) return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_armed) return 0;  // not a coop join with a received map -> nothing to re-bind

    // FRESH GC-robust walk of live UNBOUND save natives (both families). Excludes bound mirrors (the bulk
    // survivors + anything already bound) so only sparse re-creates / seam-deferred kerfurs remain. Pointer-
    // compare class filter before any read; capture InternalIndex for FindExactMatch's GC-robust liveness check.
    struct LiveNative { void* actor; int32_t idx; float x, y, z; };
    std::vector<LiveNative> chipN, kerfurN;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o)) continue;
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;       // CDO
        if (PT::IsBoundMirrorNative(o)) continue;                          // already bound (survivor) -> skip
        const bool isChip = ue_wrap::prop::IsChipPile(o);
        bool isKerfur = false;
        if (!isChip) { void* c = R::ClassOf(o); isKerfur = c && coop::kerfur_entity::IsKerfurPropClass(c); }
        if (!isChip && !isKerfur) continue;
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(o);
        (isChip ? chipN : kerfurN).push_back({o, R::InternalIndexOf(o), loc.X, loc.Y, loc.Z});
    }
    if (chipN.empty() && kerfurN.empty()) return 0;  // no unbound natives -> nothing to do (the common case)

    int rebound = 0;
    // Helper: an eid is FREE to (re)bind iff no live actor currently occupies it (never double-bind a survivor).
    auto eidFree = [](uint32_t eid) {
        coop::element::Element* el = coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(eid));
        void* boundActor = el ? el->GetActor() : nullptr;
        return !(boundActor && R::IsLive(boundActor));
    };

    // chipPile arm -- POSITION match (keyless).
    {
        std::vector<bool> used(chipN.size(), false);
        for (const MAP::IdEntry& e : g_chipEntries) {
            if (e.eid == 0u || e.eid == coop::element::kInvalidId || !eidFree(e.eid)) continue;
            const ue_wrap::FVector key{e.savePosX, e.savePosY, e.savePosZ};
            const int idx = coop::save_time_retire_util::FindExactMatch(
                chipN, used, key, [](const LiveNative&) { return true; });  // position-only (1cm exact)
            if (idx < 0) continue;  // 0 (no re-create -- normal) or >1 (ambiguous co-located -> skip)
            used[idx] = true;
            BindLocalNativeToHostEid_(chipN[idx].actor, static_cast<coop::element::ElementId>(e.eid),
                                      MAP::Family::ChipPile, e.index);
            ++rebound;
            UE_LOGI("save_identity_bind: RE-BIND chipPile by position -- native=%p -> host eid=%u @save-pos=(%.1f,"
                    "%.1f,%.1f) (GC-churned re-create; authoritative host-wire position match)",
                    chipN[idx].actor, e.eid, e.savePosX, e.savePosY, e.savePosZ);
        }
    }

    // kerfurOff arm -- KEY match (sidecar v3): the GUARANTEED kerfur bind. Each unbound kerfur native's portable
    // key resolves its host eid directly -- no fuzzy position, no cursor. The eidFree guard makes same-key
    // duplicates (shouldn't occur) idempotent: the first binds, the rest see the eid occupied and skip.
    for (LiveNative& kn : kerfurN) {
        const std::wstring nkey = ue_wrap::prop::GetInteractableKeyString(kn.actor);
        const MAP::IdEntry* e = FindKerfurEntryByKey_(nkey);
        if (!e || e->eid == 0u || e->eid == coop::element::kInvalidId || !eidFree(e->eid)) continue;
        BindLocalNativeToHostEid_(kn.actor, static_cast<coop::element::ElementId>(e->eid),
                                  MAP::Family::KerfurOff, e->index);
        ++rebound;
        UE_LOGI("save_identity_bind: RE-BIND kerfurOff by KEY -- native=%p key='%ls' -> host eid=%u "
                "(deterministic intrinsic-key bind; no position, no cursor)", kn.actor, nkey.c_str(), e->eid);
    }

    if (rebound > 0 || !chipN.empty() || !kerfurN.empty())
        UE_LOGI("save_identity_bind: quiescence re-bind pass -- %d native(s) re-bound (walked %zu unbound chip "
                "[by position] + %zu unbound kerfur [by key])", rebound, chipN.size(), kerfurN.size());
    return rebound;
}

void ForceSaveChurnForTest() {
    static const bool s_on = coop::ini_config::IsIniKeyTrue("force_save_churn");
    static bool s_done = false;
    if (s_done) return;  // one-shot: churn once, just before the quiescence sweep
    s_done = true;
    std::lock_guard<std::mutex> lk(g_mu);
    // DIAGNOSTIC (unconditional): the 15:12 run produced ZERO churn -- log every gate so the next run pins
    // which one failed (flag read / armed / how many chip eids are bound+live at this instant).
    int boundLive = 0;
    for (const MAP::IdEntry& e : g_chipEntries) {
        if (e.eid == 0u || e.eid == coop::element::kInvalidId) continue;
        coop::element::Element* el =
            coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(e.eid));
        void* actor = el ? el->GetActor() : nullptr;
        if (actor && R::IsLive(actor)) ++boundLive;
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
        void* actor = el ? el->GetActor() : nullptr;
        if (!actor || !R::IsLive(actor)) continue;  // only churn a currently-bound, live save-native
        // Unbind: Take the mirror Element (the actor stays alive at its save position). ~Element clears the
        // registry slot + the unified actor->eid reverse, so variant-1 sees an UNBOUND native at savePos.
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
    static const bool s_on = coop::ini_config::IsIniKeyTrue("reseed_orphan_selftest");
    static bool s_done = false;
    if (!s_on || s_done) return false;
    if (!IsEnabled()) {
        UE_LOGW("save_identity_bind: [reseed_orphan_selftest] requires save_identity_bind=1 (variant-1 gates on it) -- skipping");
        s_done = true;
        return false;
    }
    namespace EL = coop::element;

    // 1. Find a real live UNBOUND chipPile native (a host's own local pile) as the subject. Retry next tick if
    //    the world's piles haven't loaded yet (do NOT latch s_done until we actually have a subject).
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

    // 2. Pick a free host-range eid for the synthetic bind.
    EL::ElementId hostEid = EL::kInvalidId;
    for (EL::ElementId cand = 32000; cand > 1; --cand) {
        if (EL::Registry::Get().Get(cand) == nullptr) { hostEid = cand; break; }
    }
    if (hostEid == EL::kInvalidId) {
        UE_LOGW("save_identity_bind: [reseed_orphan_selftest] no free host-range eid -- skipping");
        return false;
    }

    // 3. Self-arm a 1-entry map (the native's CURRENT position -> hostEid) + BIND the native (host-range mirror).
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

    // 4. CHURN: Take the mirror Element + enqueue it DEFERRED -- exactly the reaper's Take-but-not-Flushed window
    //    (the Element is gone from the manager, but ~Element/reverse-clear is pending until Flush).
    auto taken = EL::MirrorManager<EL::Prop>::Instance().Take(hostEid);
    const bool tookIt = (taken != nullptr);
    if (tookIt) EL::ElementDeleter::Get().Enqueue(std::move(taken));
    const EL::ElementId eidBeforeFlush = EL::Registry::Get().EidForActor(native);  // STALE: still maps -> the race

    // 5. THE FIX SEQUENCE (mirrors net_pump's purge-drain re-seed edge): (a) Flush settles the reverse, then the
    //    re-seed, then (b) variant-1 re-binds the now-unbound native by its save-time position.
    EL::ElementDeleter::Get().Flush();
    const EL::ElementId eidAfterFlush = EL::Registry::Get().EidForActor(native);   // expect kInvalid (settled)
    PT::ReSeedKnownKeyedProps();                  // re-seeds the unbound native as a fresh local (the GC-recreate analog)
    const int rebound = BindUnboundReCreates();  // (b): re-bind (chip by position; this self-test is a chipPile)

    // 6. VERDICT: the churned native must be RE-BOUND to hostEid as a host-range mirror = no orphan.
    const EL::ElementId finalEid = EL::Registry::Get().EidForActor(native);
    const bool reboundOK = (finalEid == hostEid) && PT::IsBoundMirrorNative(native);
    UE_LOGW("save_identity_bind: [reseed_orphan_selftest] native=%p idx=%d @pos=(%.1f,%.1f,%.1f) hostEid=%u | "
            "bound=%d took=%d eidBeforeFlush=%u(stale) eidAfterFlush=%u(settled) rebound=%d finalEid=%u | VERDICT=%s",
            native, nativeIdx, pos.X, pos.Y, pos.Z, static_cast<unsigned>(hostEid),
            boundOK ? 1 : 0, tookIt ? 1 : 0, static_cast<unsigned>(eidBeforeFlush),
            static_cast<unsigned>(eidAfterFlush), rebound, static_cast<unsigned>(finalEid),
            reboundOK ? "PASS (churned save-native re-bound by position AT the re-seed -- 09:54 orphan closed)"
                      : "FAIL (native orphaned -- re-seed-orphan NOT closed)");

    // 7. RESTORE: drop the synthetic test binding so the host world returns to normal (native re-seeds as a local).
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
    // (sync-refactor 2026-06-27) No bound-mirror SET to clear: is-save-native is an Element flag that dies when
    // the save-native mirror Element is drained on disconnect (DrainMirrorsOnly).
}

}  // namespace coop::save_identity_bind
