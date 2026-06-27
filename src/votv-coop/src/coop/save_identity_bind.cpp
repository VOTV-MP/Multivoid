// coop/save_identity_bind.cpp -- see header. Phase 1 step 2b: CLIENT eid-range BIND (first mutating step).

#include "coop/save_identity_bind.h"

#include "coop/element/element.h"          // Element, ElementId, kInvalidId
#include "coop/element/registry.h"         // Registry::Get().Get(eid)
#include "coop/ini_config.h"               // IsIniKeyTrue
#include "coop/prop_element_tracker.h"     // UnmarkKnownKeyedProp, GetPropElementIdForActor, MarkBoundMirrorNative
#include "coop/remote_prop.h"              // RegisterPropMirror, ConsumeLocalActor
#include "coop/trash_proxy.h"              // (X) item 4: IsProxy / RetireProxy (proxy-before-bind race)
#include "coop/trash_channel.h"            // b1 (#2): CtxForEid -- was E converted in-window? (proxy-wins discriminator)
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"                  // GetInteractableKeyString
#include "ue_wrap/reflection.h"            // ClassNameOf, IsLive

#include <chrono>
#include <mutex>
#include <string>

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

// Option-3 purge-race fix (2026-06-27): the reseed-rebind window. ResetForReseed (mass-purge episode-start)
// resets the per-family cursors + clears the bound-mirror flags so the same-world Load-Primitives re-creates
// re-bind via the proven ordinal seam; the divergence sweep then defers (IsReseedRebindSettled) until the
// re-create trickle has re-bound, so it (and b3 + the kerfur retire that gate on its quiescence) adjudicate the
// RE-BOUND world. The dwell + hard-cap fallbacks ensure a re-create count that never reaches the mapped count
// can't hang the gate forever.
bool g_reseedRebindActive = false;
std::chrono::steady_clock::time_point g_reseedResetAt{};
std::chrono::steady_clock::time_point g_reseedLastAdvance{};
constexpr int kReseedTrickleDwellMs   = 10000;  // no new re-bind for this long => trickle settled (count-never-reaches-mapped fallback)
constexpr int kReseedTrickleHardCapMs = 60000;  // absolute ceiling from reset (< the sweep's 120s hard cap so the rebind settles first)

// [BIND-PROBE] (read-only diagnostic, 2026-06-27, purge-race option-3 viability -- RULE-2-exempt, no mutation).
// Counts bind-seam fires that land DURING a purge episode (the same-world Load-Primitives re-create). Decides
// option 3 (cursor-reset) vs option 1 (position wire-extension): if the ~870 re-created chipPiles fire the seam
// here in bulk (high count) a cursor-reset (+ clearing the bound-mirror flags so recycled-address re-creates
// aren't mistaken for survivors) would re-bind them; if ~0 fire, the re-create bypasses this seam -> option 1.
// The 09:54 log's anomaly (overflow only 2 vs ~870 expected if they fired fresh) is exactly what this resolves:
// it logs, per fire, whether the native is a survivor (IsBoundMirrorNative -> recycled-address false-positive)
// vs a fresh exhausted-cursor fire. Throttled to bound log volume. Counters are cumulative across episodes.
int g_probeChipFires = 0, g_probeKerfurFires = 0;

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
    // Guard: mark the native as a bound mirror so the client's post-load SeedWalk_ does NOT re-mint a peer-range
    // LOCAL element on it (MarkPropElement's idempotency only knows g_actorToPropElementId, which mirrors are
    // not in -> without this it would double-element). The root fix for binding-as-mirror at load time.
    PT::MarkBoundMirrorNative(native);
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
    g_reseedRebindActive = false;  // a fresh arm = a new join; no reseed-rebind in flight
    g_boundChip = g_boundKerfur = g_caseI = g_caseII = 0;
    g_overflowChip = g_overflowKerfur = 0;
    UE_LOGI("save_identity_bind: ARMED with %zu-entry host eid map (%zu chipPile + %zu kerfurOff) -- per-family "
            "ordinal bind (Build 3: key on saveSlot array index, immune to cross-array spawn order). [dev] "
            "save_identity_bind=1", g_map.size(), g_chipEntries.size(), g_kerfurEntries.size());
}

void OnKeylessLoadSpawn(void* newActor, MAP::Family family) {
    if (!newActor || !IsEnabled()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    // [BIND-PROBE] read-only diagnostic (2026-06-27, repurposed for the option-3 FIX verification). Originally
    // gated on InPurgeEpisode to decide option 3 vs 1; lever (a) collapsed that window to <1s while the
    // re-creates fire over ~18s, so it logged zero (the overflow lines answered it instead). Now gated on the
    // reseed-rebind WINDOW (ResetForReseed..settled): logs each re-create re-binding from the reset cursor =
    // direct evidence the fix works (cursor climbs 0 -> mapped, survivor=0, exhausted=0). Throttled: first 8 +
    // every 100th. Logging only (no mutation).
    if (g_reseedRebindActive) {
        const size_t curp = (family == MAP::Family::ChipPile) ? g_chipCursor : g_kerfurCursor;
        const size_t szp  = (family == MAP::Family::ChipPile) ? g_chipEntries.size() : g_kerfurEntries.size();
        const bool   surv = PT::IsBoundMirrorNative(newActor);
        int& seen = (family == MAP::Family::ChipPile) ? g_probeChipFires : g_probeKerfurFires;
        ++seen;
        if (seen <= 8 || (seen % 100) == 0)
            UE_LOGW("[BIND-PROBE] %s re-create #%d in reseed-rebind window: cursor=%zu/%zu armed=%d survivor=%d "
                    "exhausted=%d -- will-rebind=%d (cursor climbing 0->mapped = fix working; survivor/exhausted "
                    "should be 0)",
                    FamName(family), seen, curp, szp, (int)g_armed, (int)surv, (int)(curp >= szp),
                    (int)(g_armed && !surv && curp < szp));
    }
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
    if (g_reseedRebindActive) g_reseedLastAdvance = std::chrono::steady_clock::now();  // re-bind progress -> reset the dwell
}

// Purge-race option-3 part 1 (2026-06-27): mass-purge episode-start -- reset the per-family cursors so the
// same-world Load-Primitives re-creates re-bind via the proven ordinal seam, and clear the bound-mirror guard
// set so a re-create at a recycled address of a just-destroyed bound native isn't taken for a survivor (the
// silent half the 11:32 log couldn't see; safe -- at a mass purge ALL bound natives are in the purge, so this
// orphans nothing). Arms the reseed-rebind window the divergence sweep defers on (part 2).
void ResetForReseed() {
    if (!IsEnabled()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_armed) return;  // never armed -> nothing to re-bind
    g_chipCursor = g_kerfurCursor = 0;
    PT::ClearBoundMirrorNatives();
    g_probeChipFires = g_probeKerfurFires = 0;  // fresh [BIND-PROBE] count for this re-bind
    g_reseedRebindActive = true;
    const auto now = std::chrono::steady_clock::now();
    g_reseedResetAt = now;
    g_reseedLastAdvance = now;
    UE_LOGI("save_identity_bind: RESET-FOR-RESEED -- per-family cursors->0 + bound-mirror flags cleared; "
            "reseed-rebind window armed. The same-world Load-Primitives re-create now re-binds the %zu chipPile + "
            "%zu kerfurOff natives via the proven ordinal seam; the divergence sweep defers (IsReseedRebindSettled) "
            "until the re-create trickle settles.", g_chipEntries.size(), g_kerfurEntries.size());
}

// Purge-race option-3 part 2 (2026-06-27): the sweep-fire gate. True when no reseed is in flight, the re-create
// trickle has fully re-bound (cursors back to mapped), or a no-progress dwell / hard-cap fallback expired (so a
// re-create count that never reaches mapped can't hang the sweep). Latches once settled.
bool IsReseedRebindSettled() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_reseedRebindActive) return true;  // no reseed in flight -> never blocks a normal join
    const auto now = std::chrono::steady_clock::now();
    const bool allRebound = (g_chipCursor >= g_chipEntries.size()) && (g_kerfurCursor >= g_kerfurEntries.size());
    const auto sinceAdvance = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_reseedLastAdvance).count();
    const auto sinceReset   = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_reseedResetAt).count();
    if (allRebound || sinceAdvance >= kReseedTrickleDwellMs || sinceReset >= kReseedTrickleHardCapMs) {
        g_reseedRebindActive = false;  // latch settled
        UE_LOGI("save_identity_bind: reseed-rebind SETTLED (chip %zu/%zu kerfur %zu/%zu; reason=%s) -- divergence "
                "sweep + b3 + kerfur-retire may now adjudicate the re-bound world",
                g_chipCursor, g_chipEntries.size(), g_kerfurCursor, g_kerfurEntries.size(),
                allRebound ? "all-rebound" : (sinceAdvance >= kReseedTrickleDwellMs ? "no-progress-dwell" : "hard-cap"));
        return true;
    }
    return false;
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

void OnDisconnect() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_map.clear();
    g_chipEntries.clear();
    g_kerfurEntries.clear();
    g_armed = false;
    g_chipCursor = g_kerfurCursor = 0;
    g_reseedRebindActive = false;
    g_boundChip = g_boundKerfur = g_caseI = g_caseII = 0;
    g_overflowChip = g_overflowKerfur = 0;
    g_probeChipFires = g_probeKerfurFires = 0;
    PT::ClearBoundMirrorNatives();
}

}  // namespace coop::save_identity_bind
