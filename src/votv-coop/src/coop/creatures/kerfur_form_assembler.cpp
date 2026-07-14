// coop/creatures/kerfur_form_assembler.cpp -- see kerfur_form_assembler.h.
//
// Increment 1 (observe-only) of docs/COOP_VM_DISPATCH_PLAN.md S3. Registers the two
// conversion verbs (dropKerfurProp / spawnKerfuro) with the EX_Local* substrate and,
// on each caught 0x45 dispatch, LOGS the Context class -- proving the PERMANENT
// substrate delivers the catch end-to-end through the registration API and turning
// the inferred kerfur variant family into a MEASURED set.
//
// It ALSO carries the CONTAINMENT COUNTER the /qf pass (2026-07-13, user's 8 Qs + 3
// escalation rounds) forced into this increment: the 2a capture design rests on
// "FinishSpawningActor + K2_DestroyActor fire SYNCHRONOUSLY inside the 0x45 bracket"
// -- currently INFERRED from the 2026-06-12 kismet disasm, NOT measured through the
// live bracket. Rather than build 2a on that inference, this increment MEASURES it
// for free: the substrate publishes the active-verb window (ue_wrap::vm_dispatch::
// CurrentThreadVerb), and this module registers its OWN post-hooks on the two natives
// and counts each kerfur-relevant spawn/destroy as IN-window vs OUT-of-window. 2a is
// HALT-gated on the verdict (all form spawns + self-destroys land in-window).
//
// Attribution is deterministic, per the /qf convergence:
//   - SPAWN: class-filter the finished *Result -- a kerfur-form successor
//     (kerfurOmega_C / prop_kerfurOmega_C family) is B; the floppy (prop_floppyDisc_C)
//     is distinguished by class, NOT conflated. "in-window AND form-successor", not
//     "in-window".
//   - DESTROY: the invariant is IDENTITY, not class -- a conversion verb destroys
//     ITSELF, so the K2_DestroyActor whose dying actor == the bracket's Context is the
//     verb's own victim (the seam 2b suppresses), told apart from unrelated kerfur
//     churn by pointer identity.
// Both seams are pinned INDEPENDENTLY (spawn does not imply destroy).
//
// On top of the containment counter, this increment carries the 2a-OBSERVE GATE
// instrumentation the /qf convergence (2026-07-13 nite) named as the HALT-gate the
// repoint-at-birth capture design must clear on ONE deliberate hands-on run, counted
// deliberately -- all measured by READS only (registry lookup / index check), NO
// capture, NO repoint, NO converge:
//   - GATE 1 (one-capture-per-A-eid): resolve A's eid at verb ENTRY (Registry::
//     EidForActor -- a map read the mid-verb lesson explicitly permits). eidUnbound>0
//     means the repoint would have no source; reentrySameEid>0 is the contested/
//     re-entrant double-capture hazard.
//   - GATE 2 (class separation): the floppy is class-distinguished from the form (the
//     existing floppyIn counter). A RUN CONDITION, not code: the 18/18 floppyIn=0 was
//     a NULL result -- a floppy-carrying toggle is needed to turn it into a pass.
//   - GATE 3b (per-bracket seam ORDER -- LOAD-BEARING): a thread-local per-bracket flag
//     proves B's form-spawn fires BEFORE A's self-destroy WITHIN one bracket (not just
//     as global totals). destroyNoSpawn>0 = the eid would strand on the dying A = HALT.
//   - GATE 3c (B-index live at capture): B is a real, index-assigned, live object at
//     FinishSpawningActor (the repoint target is valid). bIndexDead>0 = HALT.
// The client LOCAL forced-prediction verb-death husk falls out of the SAME counters
// under the [CLIENT] role tag (SetEnabled is both roles); the pending-eid pose-tick
// misroute is NOT observable without the pending flag and is deferred to 2a-capture's
// own verification (documented in docs/COOP_VM_DISPATCH_PLAN.md, not faked here).
//
// The COUNTERS accrue whenever the session is active, independent of the verbose log
// -- and BOTH summary lines (containment + 2a-OBSERVE GATES) are ALWAYS dumped at
// OnDisconnect -- so a run can never end having measured nothing. Only the per-catch
// VERBOSE lines are gated behind [dev] vm_dispatch_log (+ a cap). Every line is
// role-tagged [HOST]/[CLIENT] so the gate set is not authority-blind. NO capture /
// suppress / converge yet (2a-2c); the verb bodies run entirely unchanged.

#include "coop/creatures/kerfur_form_assembler.h"

#include "coop/config/config.h"
#include "coop/creatures/kerfur_convert.h"  // OBSERVE (2026-07-14 G1): ActiveRequestVerbEid -- the CallFunction route scope
#include "coop/element/element.h"    // ElementId, kInvalidId (gate 1 per-eid read)
#include "coop/element/registry.h"   // Registry::EidForActor (gate 1 per-eid read)
#include "coop/net/session.h"

#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile_names.h"
#include "ue_wrap/ufunction_hook.h"
#include "ue_wrap/vm_dispatch.h"

#include <atomic>
#include <cstdint>

namespace coop::kerfur_form_assembler {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace vm = ue_wrap::vm_dispatch;
namespace P  = ue_wrap::profile;
namespace E  = coop::element;

// Verb ids echoed back in the substrate Bracket (also the window's verbId).
constexpr int kVerbTurnOff = 1;  // dropKerfurProp -- NPC -> prop (destroys the NPC self)
constexpr int kVerbTurnOn  = 2;  // spawnKerfuro   -- prop -> NPC (destroys the prop self)

coop::net::Session* g_session = nullptr;

// Kerfur-form + floppy class pointers (resolved lazily on the GT for the spawn
// class-filter). Null until resolved -> a spawn never mis-attributes.
std::atomic<void*> g_npcClass{nullptr};     // kerfurOmega_C     (turn-ON successor + turn-OFF victim)
std::atomic<void*> g_propClass{nullptr};    // prop_kerfurOmega_C (turn-OFF successor + turn-ON victim)
std::atomic<void*> g_floppyClass{nullptr};  // prop_floppyDisc_C  (the incidental floppy spawn)
std::atomic<bool>  g_classesResolved{false};

// The two native seams (Func-patched once, process-lifetime). Own hooks -- idempotent
// + multi-patch, so no collision with host_spawn_watcher's FinishSpawningActor patch.
void* g_finishSpawnFn = nullptr;
void* g_destroyFn     = nullptr;
std::atomic<bool> g_seamsInstalled{false};

// ---- containment counters (accrue whenever the session is enabled) ----
std::atomic<std::uint64_t> g_spawnFormInWindow{0};   // kerfur-form successor spawned INSIDE a bracket
std::atomic<std::uint64_t> g_spawnFormOutWindow{0};  // kerfur-form successor spawned OUTSIDE any bracket
std::atomic<std::uint64_t> g_spawnFloppyInWindow{0}; // the floppy spawned inside a bracket (class-distinguished)
std::atomic<std::uint64_t> g_spawnOtherInWindow{0};  // GATE 2 reject side: an in-window spawn that is NEITHER form NOR floppy (loot/explosion_C/etc.) -- the filter MUST reject it as a repoint target
std::atomic<std::uint64_t> g_destroySelfInWindow{0}; // dying actor == bracket Context (the verb's self-destroy)
std::atomic<std::uint64_t> g_destroyOtherInWindow{0};// a kerfur-class actor != Context destroyed inside a bracket (anomaly)
std::atomic<std::uint64_t> g_destroyKerfurOutWindow{0}; // a kerfur-class actor destroyed OUTSIDE any bracket
std::atomic<std::uint64_t> g_catchTurnOff{0};        // 0x45 dropKerfurProp entries caught
std::atomic<std::uint64_t> g_catchTurnOn{0};         // 0x45 spawnKerfuro entries caught
// G1 (2026-07-14): the CallFunction route (OnConvertRequest host-exec-client-request) is 0x45-BLIND
// (CallFunction != EX_Local, so no OnVerbEntry / CurrentThreadVerb window). kerfur_convert publishes the
// request eid it is executing (ActiveRequestVerbEid) as a SECOND capture scope. These count the form-spawn
// + kerfur self-destroy that fire while a request is executing -- proving the CallFunction-route B/destroy
// are capturable via the request eid (currently they land in formOut/kerfurOut, mixed with world spawns).
std::atomic<std::uint64_t> g_spawnFormInReqScope{0};   // kerfur-form successor spawned during a CallFunction request
std::atomic<std::uint64_t> g_destroySelfInReqScope{0}; // a kerfur-class actor destroyed during a CallFunction request

// ---- 2a-observe gate instrumentation (measures what the repoint-at-birth design rests on) ----
// GATE 1 (one-capture-per-A-eid): does the verb's Context actor A carry an eid at ENTRY?
// The repoint has NO source unless A is registry-bound here. Also the re-entry tripwire:
// a same-eid bracket opened while one is already open on this thread = double-capture hazard.
std::atomic<std::uint64_t> g_entryEidBound{0};       // A had a live eid at verb entry (repoint has a source)
std::atomic<std::uint64_t> g_entryEidUnbound{0};     // A had NO eid at verb entry (nothing to repoint -- HALT-relevant)
std::atomic<std::uint64_t> g_entrySameEidReentry{0}; // nested bracket on the SAME eid (contested/re-entrant -- HALT)
// GATE 3b (per-bracket seam ORDER -- LOAD-BEARING): within ONE bracket, did B's form-spawn
// fire BEFORE A's self-destroy? The repoint migrates identity at B's birth, so a self-destroy
// with no preceding in-bracket form-spawn would leave the eid stranded on the dying A.
std::atomic<std::uint64_t> g_orderSpawnBeforeDestroy{0}; // GOOD: form-spawn preceded self-destroy in-bracket
std::atomic<std::uint64_t> g_orderDestroyNoSpawn{0};     // VIOLATION: self-destroy, no in-bracket form-spawn (HALT)
// GATE 3c (B-index live at capture): is the finished successor B a real, index-assigned, live
// GUObjectArray object at FinishSpawningActor time? The repoint target must be valid here.
std::atomic<std::uint64_t> g_spawnBIndexLive{0};     // B had a live internal index at spawn (repoint target valid)
std::atomic<std::uint64_t> g_spawnBIndexDead{0};     // B's index not live at spawn (HALT -- repoint would dangle)

// Per-bracket seam-order record (thread-local: the verb runs synchronously on the GT, so a
// single-slot TLS tracks the innermost depth-1 bracket; nested depth is counted as an anomaly
// via the substrate's Bracket.depth, not tracked here). Reset at each depth-1 verb ENTRY.
thread_local bool     tls_spawnFormFiredThisBracket = false;
thread_local void*    tls_bracketCtx = nullptr;       // the Context whose bracket owns the TLS record
thread_local E::ElementId tls_bracketEntryEid = E::kInvalidId; // A's eid captured at entry (for re-entry check)

// Verbose per-catch log cap (the measurement counters above are UNCAPPED + always on).
constexpr int kLogCap = 128;
std::atomic<int> g_logged{0};

bool LogVerbose() { return coop::config::IsIniKeyTrue("vm_dispatch_log"); }
bool IsHostRole() { return g_session && g_session->role() == coop::net::Role::Host; }
const char* RoleTag() { return IsHostRole() ? "HOST" : "CLIENT"; }

// Is `cls` a kerfur-form class (either the NPC or prop family)?
bool IsKerfurFormClass(void* cls) {
    void* bases[2] = {g_npcClass.load(std::memory_order_relaxed),
                      g_propClass.load(std::memory_order_relaxed)};
    if (!bases[0] || !bases[1]) return false;
    return R::IsDescendantOfAny(cls, bases, 2);
}
bool IsFloppyClass(void* cls) {
    void* fb = g_floppyClass.load(std::memory_order_relaxed);
    return fb && R::IsDescendantOfAny(cls, &fb, 1);
}
// G1: is the host currently executing a client's convert-request via CallFunction (the 0x45-blind route)?
bool InReqScope() {
    return coop::kerfur_convert::ActiveRequestVerbEid() != E::kInvalidId;
}

// ---- the substrate ENTRY callback (observe-only) -------------------------------
void OnVerbEntry(const vm::Bracket& b) {
    if (b.verbId == kVerbTurnOff) g_catchTurnOff.fetch_add(1, std::memory_order_relaxed);
    else                          g_catchTurnOn.fetch_add(1, std::memory_order_relaxed);

    // GATE 1 -- resolve A's eid at entry (a pure Registry map READ, not an engine call; the
    // mid-verb lesson explicitly permits "resolve an eid off a still-live actor by lookup").
    const E::ElementId entryEid = E::Registry::Get().EidForActor(b.ctx);
    const bool bound = (entryEid != E::kInvalidId);
    if (bound) g_entryEidBound.fetch_add(1, std::memory_order_relaxed);
    else       g_entryEidUnbound.fetch_add(1, std::memory_order_relaxed);

    // GATE 3b -- reset the per-bracket seam-order record on the OUTERMOST (depth-1) entry.
    // A nested (depth>1) bracket on the SAME eid is the double-capture hazard; count it and
    // do NOT clobber the outer record.
    if (b.depth <= 1) {
        tls_spawnFormFiredThisBracket = false;
        tls_bracketCtx = b.ctx;
        tls_bracketEntryEid = entryEid;
    } else if (bound && entryEid == tls_bracketEntryEid) {
        g_entrySameEidReentry.fetch_add(1, std::memory_order_relaxed);
    }

    if (!LogVerbose()) return;
    if (g_logged.fetch_add(1, std::memory_order_relaxed) >= kLogCap) return;
    const wchar_t* verb = (b.verbId == kVerbTurnOff) ? L"dropKerfurProp" : L"spawnKerfuro";
    std::wstring cls = R::ClassNameOf(b.ctx);
    UE_LOGI("[kerfur_asm][%s] VERB %ls id=%d Context=%p class=%ls depth=%d eid=%s%u (observe-only)",
            RoleTag(), verb, b.verbId, b.ctx, cls.c_str(), b.depth,
            bound ? "" : "UNBOUND:", bound ? entryEid : 0u);
}

// ---- the SPAWN seam (FinishSpawningActor post-hook) ----------------------------
// spawnedResult = *Result = the finished actor (ufunction_hook contract).
void OnFinishSpawn(void* /*context*/, void* /*sourceObject*/, void* spawnedResult) {
    if (!vm::IsEnabled() || !spawnedResult) return;  // no cost in solo SP
    void* cls = R::ClassOf(spawnedResult);
    if (!cls) return;
    const bool isForm   = IsKerfurFormClass(cls);
    const bool isFloppy = !isForm && IsFloppyClass(cls);
    if (!isForm && !isFloppy) {
        // GATE 2, REJECT SIDE: an in-window spawn that is NEITHER a kerfur-form successor NOR the
        // floppy = loot / explosion_C / anything else the conversion ubergraph can BeginDeferred inside
        // the bracket. The class filter (IsKerfurFormClass) MUST reject it as a repoint target. Counting
        // it in-window proves the filter REJECTS non-B -- a DIFFERENT claim than the 45/45 descent census
        // (which only proved it CATCHES every true B). Without this the loot-carrying run would be another
        // floppyIn=0-style NULL result. Out-of-window neither = an ordinary world spawn (the vast
        // majority) -> ignored (CurrentThreadVerb is the cheap gate).
        const vm::ActiveVerb av = vm::CurrentThreadVerb();
        if (av.active) {
            g_spawnOtherInWindow.fetch_add(1, std::memory_order_relaxed);
            if (LogVerbose() && g_logged.fetch_add(1, std::memory_order_relaxed) < kLogCap) {
                std::wstring cn = R::ClassNameOf(spawnedResult);
                UE_LOGI("[kerfur_asm][%s] SPAWN OTHER IN-WINDOW actor=%p class=%ls verbId=%d depth=%d "
                        "-- filter REJECTS (not form, not floppy; NOT a repoint target)",
                        RoleTag(), spawnedResult, cn.c_str(), av.verbId, av.depth);
            }
        }
        return;
    }

    const vm::ActiveVerb av = vm::CurrentThreadVerb();
    const bool reqScope = !av.active && InReqScope();  // G1: CallFunction route (0x45-blind)
    bool bIndexLive = false;
    int32_t bIdx = -1;
    if (isForm) {
        if (av.active) {
            g_spawnFormInWindow.fetch_add(1, std::memory_order_relaxed);
            // GATE 3b -- mark that the form-spawn fired in THIS bracket, so the later
            // self-destroy can prove spawn-before-destroy ordering.
            tls_spawnFormFiredThisBracket = true;
            // GATE 3c -- is B a real, index-assigned, live object at FinishSpawningActor?
            bIdx = R::InternalIndexOf(spawnedResult);
            bIndexLive = R::IsLiveByIndex(spawnedResult, bIdx);
            if (bIndexLive) g_spawnBIndexLive.fetch_add(1, std::memory_order_relaxed);
            else            g_spawnBIndexDead.fetch_add(1, std::memory_order_relaxed);
        } else if (reqScope) {
            // G1: the host is executing a client's convert-request via CallFunction (0x45-blind route) --
            // this form-spawn IS the conversion successor B, capturable via the request eid instead of a
            // 0x45 bracket. Proving formInReqScope>0 closes the CallFunction-route capture gap.
            g_spawnFormInReqScope.fetch_add(1, std::memory_order_relaxed);
            bIdx = R::InternalIndexOf(spawnedResult);
            bIndexLive = R::IsLiveByIndex(spawnedResult, bIdx);
        } else {
            g_spawnFormOutWindow.fetch_add(1, std::memory_order_relaxed);
        }
    } else {  // floppy
        if (av.active) g_spawnFloppyInWindow.fetch_add(1, std::memory_order_relaxed);
    }
    if (LogVerbose() && g_logged.fetch_add(1, std::memory_order_relaxed) < kLogCap) {
        std::wstring cn = R::ClassNameOf(spawnedResult);
        const char* scope = av.active ? "IN-WINDOW" : (reqScope ? "IN-WINDOW(req-scope)" : "out-of-window");
        UE_LOGI("[kerfur_asm][%s] SPAWN %ls actor=%p class=%ls %s verbId=%d depth=%d bIdx=%d bLive=%d reqEid=%d",
                RoleTag(), isForm ? L"FORM" : L"floppy", spawnedResult, cn.c_str(),
                scope, av.verbId, av.depth, bIdx, bIndexLive ? 1 : 0,
                reqScope ? static_cast<int>(coop::kerfur_convert::ActiveRequestVerbEid()) : -1);
    }
}

// ---- the DESTROY seam (K2_DestroyActor post-hook) ------------------------------
// context = the dying actor (ufunction_hook contract).
void OnDestroy(void* context, void* /*sourceObject*/, void* /*result*/) {
    if (!vm::IsEnabled() || !context) return;  // no cost in solo SP
    void* cls = R::ClassOf(context);
    if (!cls || !IsKerfurFormClass(cls)) return;  // only kerfur-class destroys interest us

    const vm::ActiveVerb av = vm::CurrentThreadVerb();
    const char* kind;
    bool orderGood = false;
    if (!av.active) {
        if (InReqScope()) {
            // G1: the host is executing a client's convert-request via CallFunction (0x45-blind route);
            // a kerfur destroy here is the conversion's own self-destroy. Captures the CallFunction-route
            // destroy the prop_destroy_seam relay (bug1) fires on -- see prop_destroy_seam:118 provenance.
            g_destroySelfInReqScope.fetch_add(1, std::memory_order_relaxed);
            kind = "IN-WINDOW(req-scope) self";
        } else {
            g_destroyKerfurOutWindow.fetch_add(1, std::memory_order_relaxed);
            kind = "out-of-window";
        }
    } else if (context == av.ctx) {  // IDENTITY invariant: the verb's own self-destroy
        g_destroySelfInWindow.fetch_add(1, std::memory_order_relaxed);
        // GATE 3b -- did B's form-spawn already fire in THIS bracket (spawn-before-destroy)?
        // A self-destroy with no preceding in-bracket form-spawn strands the eid (2a-HALT).
        if (tls_spawnFormFiredThisBracket && context == tls_bracketCtx) {
            g_orderSpawnBeforeDestroy.fetch_add(1, std::memory_order_relaxed);
            orderGood = true;
            kind = "IN-WINDOW self (order OK: spawn<destroy)";
        } else {
            g_orderDestroyNoSpawn.fetch_add(1, std::memory_order_relaxed);
            kind = "IN-WINDOW self (ORDER VIOLATION: destroy w/o in-bracket spawn)";
        }
    } else {
        g_destroyOtherInWindow.fetch_add(1, std::memory_order_relaxed);
        kind = "IN-WINDOW non-self(anomaly)";
    }
    (void)orderGood;
    if (LogVerbose() && g_logged.fetch_add(1, std::memory_order_relaxed) < kLogCap) {
        std::wstring cn = R::ClassNameOf(context);
        UE_LOGI("[kerfur_asm][%s] DESTROY actor=%p class=%ls %s verbId=%d ctx=%p depth=%d",
                RoleTag(), context, cn.c_str(), kind, av.verbId, av.ctx, av.depth);
    }
}

// Resolve the classes + Func-patch the two seams. GT-only (FindClass/FindFunction +
// InstallPostHook). Retries until everything binds; latches once installed.
void EnsureSeamsInstalled() {
    if (g_seamsInstalled.load(std::memory_order_acquire)) return;
    if (!GT::IsGameThread()) return;

    if (!g_classesResolved.load(std::memory_order_relaxed)) {
        if (!g_npcClass.load(std::memory_order_relaxed))
            g_npcClass.store(R::FindClass(L"kerfurOmega_C"), std::memory_order_relaxed);
        if (!g_propClass.load(std::memory_order_relaxed))
            g_propClass.store(R::FindClass(L"prop_kerfurOmega_C"), std::memory_order_relaxed);
        if (!g_floppyClass.load(std::memory_order_relaxed))
            g_floppyClass.store(R::FindClass(L"prop_floppyDisc_C"), std::memory_order_relaxed);
        // Need the two form bases before the spawn/destroy filters are meaningful; the
        // floppy is non-fatal (a null floppy class just leaves floppy spawns uncounted).
        if (!g_npcClass.load(std::memory_order_relaxed) ||
            !g_propClass.load(std::memory_order_relaxed))
            return;  // retry next tick
        g_classesResolved.store(true, std::memory_order_relaxed);
    }

    if (!g_finishSpawnFn) {
        if (void* gsCls = R::FindClass(P::name::GameplayStaticsClass))
            g_finishSpawnFn = R::FindFunction(gsCls, P::name::FinishSpawningActorFn);
    }
    if (!g_destroyFn) {
        if (void* actorCls = R::FindClass(P::name::ActorClassName))
            g_destroyFn = R::FindFunction(actorCls, P::name::DestroyActorFn);
    }
    if (!g_finishSpawnFn || !g_destroyFn) return;  // retry next tick

    const bool a = ue_wrap::ufunction_hook::InstallPostHook(g_finishSpawnFn, &OnFinishSpawn);
    const bool b = ue_wrap::ufunction_hook::InstallPostHook(g_destroyFn, &OnDestroy);
    g_seamsInstalled.store(true, std::memory_order_release);  // latch regardless (idempotent hooks)
    UE_LOGI("[kerfur_asm] containment seams installed: FinishSpawningActor=%d K2_DestroyActor=%d "
            "(npc=%p prop=%p floppy=%p)", a ? 1 : 0, b ? 1 : 0,
            g_npcClass.load(std::memory_order_relaxed), g_propClass.load(std::memory_order_relaxed),
            g_floppyClass.load(std::memory_order_relaxed));
}

void DumpSummary(const char* when) {
    UE_LOGI("[kerfur_asm][%s] CONTAINMENT SUMMARY (%s): catch{off=%llu on=%llu} "
            "spawn{formIn=%llu formInReqScope=%llu formOut=%llu floppyIn=%llu otherIn=%llu} "
            "destroy{selfIn=%llu selfInReqScope=%llu otherIn=%llu kerfurOut=%llu} -- IN-window good, "
            "OUT/anomaly = 2a-HALT; reqScope = G1 CallFunction route (host-exec-client-request, 0x45-blind: "
            "formInReqScope>0 closes the CallFunction capture gap); "
            "spawn.otherIn = loot/explosion the filter REJECTED (GATE 2 reject side -- must NOT be formIn)",
            RoleTag(), when,
            (unsigned long long)g_catchTurnOff.load(std::memory_order_relaxed),
            (unsigned long long)g_catchTurnOn.load(std::memory_order_relaxed),
            (unsigned long long)g_spawnFormInWindow.load(std::memory_order_relaxed),
            (unsigned long long)g_spawnFormInReqScope.load(std::memory_order_relaxed),
            (unsigned long long)g_spawnFormOutWindow.load(std::memory_order_relaxed),
            (unsigned long long)g_spawnFloppyInWindow.load(std::memory_order_relaxed),
            (unsigned long long)g_spawnOtherInWindow.load(std::memory_order_relaxed),
            (unsigned long long)g_destroySelfInWindow.load(std::memory_order_relaxed),
            (unsigned long long)g_destroySelfInReqScope.load(std::memory_order_relaxed),
            (unsigned long long)g_destroyOtherInWindow.load(std::memory_order_relaxed),
            (unsigned long long)g_destroyKerfurOutWindow.load(std::memory_order_relaxed));
    // 2a-observe GATES: the repoint-at-birth design is GREEN only if eidUnbound=0,
    // sameEidReentry=0, orderNoSpawn=0, and bIndexDead=0 (all HALT signals at zero).
    UE_LOGI("[kerfur_asm][%s] 2a-OBSERVE GATES (%s): g1_entry{bound=%llu UNBOUND=%llu reentrySameEid=%llu} "
            "g3b_order{spawn<destroy=%llu DESTROY_NO_SPAWN=%llu} g3c_Bindex{live=%llu DEAD=%llu} "
            "-- GREEN iff UNBOUND=0 & reentrySameEid=0 & DESTROY_NO_SPAWN=0 & DEAD=0",
            RoleTag(), when,
            (unsigned long long)g_entryEidBound.load(std::memory_order_relaxed),
            (unsigned long long)g_entryEidUnbound.load(std::memory_order_relaxed),
            (unsigned long long)g_entrySameEidReentry.load(std::memory_order_relaxed),
            (unsigned long long)g_orderSpawnBeforeDestroy.load(std::memory_order_relaxed),
            (unsigned long long)g_orderDestroyNoSpawn.load(std::memory_order_relaxed),
            (unsigned long long)g_spawnBIndexLive.load(std::memory_order_relaxed),
            (unsigned long long)g_spawnBIndexDead.load(std::memory_order_relaxed));
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session = session;
    vm::RegisterVirtualVerb(L"dropKerfurProp", kVerbTurnOff, &OnVerbEntry);
    vm::RegisterVirtualVerb(L"spawnKerfuro",   kVerbTurnOn,  &OnVerbEntry);
    vm::SetEnabled(true);  // gate ACTIVITY on session-active (both roles -- the client
                           // needs the bracket to observe its OWN conversion; plan R9).
}

void Tick() {
    vm::TickResolvePending();  // GT FName resolve for the two verbs (no-op once armed)
    EnsureSeamsInstalled();    // GT class + Func-seam bind (retries until latched)
}

void OnDisconnect() {
    DumpSummary("session-end");  // ALWAYS -- the measurement is never behind the log gate
    vm::SetEnabled(false);
    g_logged.store(0, std::memory_order_relaxed);  // fresh verbose budget next session
}

}  // namespace coop::kerfur_form_assembler
