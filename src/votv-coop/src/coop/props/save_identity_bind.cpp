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

std::mutex g_mu;  // SetReceivedMap (net thread) vs OnKeylessLoadSpawn (game thread)
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
size_t     g_chipCursor   = 0;  // next chipPile spawn's index into g_chipEntries
size_t     g_kerfurCursor = 0;  // next off-kerfur spawn's index into g_kerfurEntries

// per-join bind tallies (for the quiescence summary)
int g_boundChip = 0, g_boundKerfur = 0;
int g_caseI = 0, g_caseII = 0;
int g_overflowChip = 0, g_overflowKerfur = 0;  // spawns beyond the per-family map count (unexpected -- logged)

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
    g_chipCursor = g_kerfurCursor = 0;
    g_armed = true;
    g_boundChip = g_boundKerfur = g_caseI = g_caseII = 0;
    g_overflowChip = g_overflowKerfur = 0;
    UE_LOGI("save_identity_bind: ARMED with %zu-entry host eid map (%zu chipPile + %zu kerfurOff) -- per-family "
            "ordinal bind (Build 3: key on saveSlot array index, immune to cross-array spawn order). [dev] "
            "save_identity_bind=1", g_map.size(), g_chipEntries.size(), g_kerfurEntries.size());
}

void OnKeylessLoadSpawn(void* newActor, MAP::Family family) {
    if (!newActor || !IsEnabled()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_armed) return;
    // Ignore a double-fire of the thunk for a native we already bound (keeps the per-family cursor aligned to
    // distinct natives; 1A saw the thunk can re-fire at a recycled address). Already-bound -> no double-bind.
    if (PT::IsBoundMirrorNative(newActor)) return;

    // Build 3: pick THIS family's list + cursor. The k-th spawn of a family binds to that family's k-th map
    // entry == that family's saveSlot array index k (RE Q1: within-array spawn order == array index order). No
    // cross-family tripwire is possible/needed now -- the family selects the list, so a mismatch cannot occur;
    // the cross-array phase ordering (the global-ordinal failure) is structurally bypassed.
    std::vector<MAP::IdEntry>& fam    = (family == MAP::Family::ChipPile) ? g_chipEntries : g_kerfurEntries;
    size_t&                    cursor = (family == MAP::Family::ChipPile) ? g_chipCursor  : g_kerfurCursor;
    if (cursor >= fam.size()) {
        // More client keyless spawns of this family than the host mapped. Unexpected (both peers load the same
        // blob -> same per-family count); stop binding THIS family (the cursor naturally blocks further binds)
        // but DO NOT touch the other family. Count + log once for the summary; never bind a non-existent entry.
        int& ov = (family == MAP::Family::ChipPile) ? g_overflowChip : g_overflowKerfur;
        if (ov == 0)
            UE_LOGW("save_identity_bind: %s keyless spawn beyond the mapped %zu %s entries -- NOT binding this "
                    "or further %s spawns (per-family count mismatch; other family unaffected)",
                    FamName(family), fam.size(), FamName(family), FamName(family));
        ++ov;
        return;
    }
    const MAP::IdEntry& e = fam[cursor];
    if (e.eid == 0u || e.eid == coop::element::kInvalidId) {
        UE_LOGW("save_identity_bind: %s map entry k=%zu (array index=%u) has invalid eid=%u -- skipping (no bind)",
                FamName(family), cursor, e.index, e.eid);
        ++cursor;
        return;
    }
    BindLocalNativeToHostEid_(newActor, static_cast<coop::element::ElementId>(e.eid), family, cursor);
    ++cursor;
}

void EmitBindSummary() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_armed) return;
    const bool overflow = (g_overflowChip + g_overflowKerfur) > 0;
    UE_LOGW("save_identity_bind: BIND SUMMARY -- bound %d/%zu map entries (%d/%zu chipPile + %d/%zu kerfurOff); "
            "case(i) E-free=%d, case(ii) race-rebind=%d; per-family cursors chip=%zu kerfur=%zu. (bound natives "
            "are host-range MIRRORs -> excluded from the divergence sweep doom set, remote_prop_spawn.cpp:1080 -- "
            "free win #1. Build 3: per-family ordinal == saveSlot array index, immune to cross-array spawn order.)",
            g_boundChip + g_boundKerfur, g_map.size(), g_boundChip, g_chipEntries.size(), g_boundKerfur,
            g_kerfurEntries.size(), g_caseI, g_caseII, g_chipCursor, g_kerfurCursor);
    if (overflow)
        UE_LOGW("save_identity_bind: OVERFLOW -- %d chipPile + %d kerfurOff spawn(s) exceeded the mapped per-family "
                "count (per-family count mismatch -- investigate the save vs the map)", g_overflowChip, g_overflowKerfur);
}

// Variant 1 (host-wire position re-bind, 2026-06-27). The cursor bind handles the bulk first load; UE's
// incremental GC sporadically destroys + re-instantiates a SPARSE handful of save natives mid-join (12:29: 2 of
// 870), which re-create at their save position UNBOUND (the cursor was already consumed -> they overflow-dropped
// = the 09:54/11:32 ghost). No client-side eid->save-pos source exists (the bind seam is pre-FinishSpawning ->
// (0,0,0); the churned eids have no survivor to capture from), so the host ships the authoritative save-time
// position per eid (sidecar v2). Here, at quiescence, we re-bind each churned native by an exact (1cm) position
// match -- ORDER/COUNT/TIMING-independent (the host stated the positions before any churn). Reuses the proven
// FindExactMatch kernel (claim-tracked, ambiguous->skip). Sibling of pile_reconcile's twin sweep
// (position-match -> DESTROY a dup); this is position-match -> BIND a sole re-create. Game thread (the sweep).
int BindUnboundReCreatesByPosition() {
    if (!IsEnabled()) return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_armed) return 0;  // not a coop join with a received map -> nothing to re-bind

    // FRESH GC-robust walk of live UNBOUND save natives (both families). Excludes bound mirrors (the bulk
    // survivors + anything already bound) so only sparse re-creates / orphans remain. Pointer-compare class
    // filter before any read; capture InternalIndex for FindExactMatch's GC-robust liveness check.
    struct LiveNative { void* actor; int32_t idx; float x, y, z; };
    std::vector<LiveNative> chipN, kerfurN;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o)) continue;
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;       // CDO
        if (PT::IsBoundMirrorNative(o)) continue;                          // already bound (survivor) -> not a re-create
        const bool isChip = ue_wrap::prop::IsChipPile(o);
        bool isKerfur = false;
        if (!isChip) { void* c = R::ClassOf(o); isKerfur = c && coop::kerfur_entity::IsKerfurPropClass(c); }
        if (!isChip && !isKerfur) continue;
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(o);
        (isChip ? chipN : kerfurN).push_back({o, R::InternalIndexOf(o), loc.X, loc.Y, loc.Z});
    }
    if (chipN.empty() && kerfurN.empty()) return 0;  // no unbound natives -> nothing to do (the common case)

    int rebound = 0;
    auto tryFamily = [&](const std::vector<MAP::IdEntry>& entries, std::vector<LiveNative>& nats,
                         MAP::Family fam) {
        std::vector<bool> used(nats.size(), false);
        for (const MAP::IdEntry& e : entries) {
            if (e.eid == 0u || e.eid == coop::element::kInvalidId) continue;
            // Skip a still-BOUND eid (a survivor occupies it) -- never double-bind. Only a churned (unbound) eid
            // gets a position re-bind.
            coop::element::Element* el =
                coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(e.eid));
            void* boundActor = el ? el->GetActor() : nullptr;
            if (boundActor && R::IsLive(boundActor)) continue;
            const ue_wrap::FVector key{e.savePosX, e.savePosY, e.savePosZ};
            const int idx = coop::save_time_retire_util::FindExactMatch(
                nats, used, key, [](const LiveNative&) { return true; });  // position-only (1cm exact)
            if (idx < 0) continue;  // 0 (no re-create for this eid -- normal) or >1 (ambiguous co-located -> skip)
            used[idx] = true;
            BindLocalNativeToHostEid_(nats[idx].actor, static_cast<coop::element::ElementId>(e.eid), fam, e.index);
            ++rebound;
            UE_LOGI("save_identity_bind: RE-BIND by position -- %s native=%p -> host eid=%u @save-pos=(%.1f,%.1f,"
                    "%.1f) (GC-churned save native re-created unbound; authoritative host-wire position match)",
                    FamName(fam), nats[idx].actor, e.eid, e.savePosX, e.savePosY, e.savePosZ);
        }
    };
    tryFamily(g_chipEntries, chipN, MAP::Family::ChipPile);
    tryFamily(g_kerfurEntries, kerfurN, MAP::Family::KerfurOff);
    if (rebound > 0 || !chipN.empty() || !kerfurN.empty())
        UE_LOGI("save_identity_bind: position re-bind pass -- %d sparse GC-churned native(s) re-bound "
                "(walked %zu unbound chip + %zu unbound kerfur native(s))", rebound, chipN.size(), kerfurN.size());
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
    const int rebound = BindUnboundReCreatesByPosition();  // (b): re-bind by position

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
    g_chipCursor = g_kerfurCursor = 0;
    g_boundChip = g_boundKerfur = g_caseI = g_caseII = 0;
    g_overflowChip = g_overflowKerfur = 0;
    // (sync-refactor 2026-06-27) No bound-mirror SET to clear: is-save-native is an Element flag that dies when
    // the save-native mirror Element is drained on disconnect (DrainMirrorsOnly).
}

}  // namespace coop::save_identity_bind
