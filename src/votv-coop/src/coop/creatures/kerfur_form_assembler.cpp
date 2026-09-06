// coop/creatures/kerfur_form_assembler.cpp -- see kerfur_form_assembler.h. Registers the two
// conversion verbs (dropKerfurProp, spawnKerfuro) with the EX_Local dispatch substrate and,
// with its own post-hooks on FinishSpawningActor and K2_DestroyActor, measures whether a
// kerfur-form spawn or self-destroy lands inside the verb's bracket. The counters accrue
// whenever the session is active and both summary lines are dumped at disconnect, so a run
// never ends having measured nothing; the per-catch verbose lines sit behind the
// vm_dispatch_log row with a cap, and every line carries the role. Attribution is
// deterministic: a spawn is the successor when its class is a kerfur form (the floppy is told
// apart by class), and a destroy is the verb's own victim when the dying actor is the
// bracket's context. On top of the counters, an in-bracket or in-request-scope successor
// spawn stores the finished actor in a one-shot thread-local slot that kerfur_convert consumes
// at the paired destroy edge, so it no longer guesses the successor by proximity. This module
// suppresses and converges nothing; the verb bodies run unchanged.

#include "coop/creatures/kerfur_form_assembler.h"

#include "coop/config/config.h"
#include "coop/creatures/kerfur_convert_host.h"  // ActiveRequestVerbEid, the request-route scope
#include "coop/element/element.h"    // ElementId, kInvalidId (gate 1 per-eid read)
#include "coop/element/registry.h"   // Registry::EidForActor (gate 1 per-eid read)
#include "coop/net/session.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/core/vm_dispatch.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>

namespace coop::kerfur_form_assembler {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace vm = ue_wrap::vm_dispatch;
namespace P  = ue_wrap::profile;
namespace E  = coop::element;

// The verb ids echoed back in the substrate's bracket.
constexpr int kVerbTurnOff = 1;  // dropKerfurProp -- NPC -> prop (destroys the NPC self)
constexpr int kVerbTurnOn  = 2;  // spawnKerfuro   -- prop -> NPC (destroys the prop self)

// The two blueprint function names registered. The literals are the identity the ambient
// window publishes (gate on the verb name, never on the id), so the same pointers are what
// InKerfurVerb compares.
constexpr const wchar_t* kVerbNameTurnOff = L"dropKerfurProp";
constexpr const wchar_t* kVerbNameTurnOn  = L"spawnKerfuro";

// Is the calling thread inside one of our own two verbs? The gates below must not test the
// active flag alone: it is true for any registered verb on this thread, and other modules
// publish verb ids 1 and 2 for their own verbs (the container's addObject, the meadow's mark,
// the drive's put-in and pulled-out), so the id would not help either. Under a foreign verb's
// bracket an unrelated spawn would count as ours and the request-scope test would go false.
// Reachable: the coin gun destroying a kerfur prop nests our destroy inside its bracket.
bool InKerfurVerb(const vm::ActiveVerb& av) {
    if (!av.active || !av.verbName) return false;
    return av.verbName == kVerbNameTurnOff || av.verbName == kVerbNameTurnOn ||
           std::wcscmp(av.verbName, kVerbNameTurnOff) == 0 ||
           std::wcscmp(av.verbName, kVerbNameTurnOn) == 0;
}

coop::net::Session* g_session = nullptr;

// The form and floppy class pointers, resolved lazily on the game thread for the spawn filter;
// null until resolved, so a spawn never mis-attributes.
std::atomic<void*> g_npcClass{nullptr};     // kerfurOmega_C     (turn-ON successor + turn-OFF victim)
std::atomic<void*> g_propClass{nullptr};    // prop_kerfurOmega_C (turn-OFF successor + turn-ON victim)
std::atomic<void*> g_floppyClass{nullptr};  // prop_floppyDisc_C  (the incidental floppy spawn)
std::atomic<bool>  g_classesResolved{false};

// The two native seams, patched once for the process. Own hooks, idempotent and multi-patch, so
// they coexist with host_spawn_watcher's FinishSpawningActor patch.
void* g_finishSpawnFn = nullptr;
void* g_destroyFn     = nullptr;
std::atomic<bool> g_seamsInstalled{false};

// The containment counters, accruing whenever the session is enabled.
std::atomic<std::uint64_t> g_spawnFormInWindow{0};   // kerfur-form successor spawned INSIDE a bracket
std::atomic<std::uint64_t> g_spawnFormOutWindow{0};  // kerfur-form successor spawned OUTSIDE any bracket
std::atomic<std::uint64_t> g_spawnFloppyInWindow{0}; // the floppy spawned inside a bracket (class-distinguished)
std::atomic<std::uint64_t> g_spawnOtherInWindow{0};  // an in-window spawn that is neither form nor floppy; the filter must reject it
std::atomic<std::uint64_t> g_destroySelfInWindow{0}; // dying actor == bracket Context (the verb's self-destroy)
std::atomic<std::uint64_t> g_destroyOtherInWindow{0};// a kerfur-class actor != Context destroyed inside a bracket (anomaly)
std::atomic<std::uint64_t> g_destroyKerfurOutWindow{0}; // a kerfur-class actor destroyed OUTSIDE any bracket
std::atomic<std::uint64_t> g_catchTurnOff{0};        // 0x45 dropKerfurProp entries caught
std::atomic<std::uint64_t> g_catchTurnOn{0};         // 0x45 spawnKerfuro entries caught
// The CallFunction route (the host executing a client's convert request) is invisible to the
// EX_Local bracket, so kerfur_convert publishes the request eid it is executing as a second
// capture scope; these count the form spawn and the self-destroy that fire while a request
// executes.
std::atomic<std::uint64_t> g_spawnFormInReqScope{0};   // kerfur-form successor spawned during a CallFunction request
std::atomic<std::uint64_t> g_destroySelfInReqScope{0}; // a kerfur-class actor destroyed during a CallFunction request

// The observe gates, measuring what the repoint-at-birth design rests on. Gate 1: does the
// verb's context actor carry an eid at entry? The repoint has no source otherwise. Also the
// re-entry tripwire: a same-eid bracket opened while one is open on this thread is the
// double-capture hazard.
std::atomic<std::uint64_t> g_entryEidBound{0};       // A had a live eid at verb entry (repoint has a source)
std::atomic<std::uint64_t> g_entryEidUnbound{0};     // A had NO eid at verb entry (nothing to repoint -- HALT-relevant)
std::atomic<std::uint64_t> g_entrySameEidReentry{0}; // nested bracket on the SAME eid (contested/re-entrant -- HALT)
// Gate 3b, the per-bracket seam order: within one bracket, did the successor's spawn fire
// before the self-destroy? The repoint migrates identity at the successor's birth, so a
// self-destroy with no preceding in-bracket spawn would strand the eid on the dying actor.
std::atomic<std::uint64_t> g_orderSpawnBeforeDestroy{0}; // GOOD: form-spawn preceded self-destroy in-bracket
std::atomic<std::uint64_t> g_orderDestroyNoSpawn{0};     // VIOLATION: self-destroy, no in-bracket form-spawn (HALT)
// Gate 3c: is the finished successor a real, index-assigned, live object at
// FinishSpawningActor? The repoint target must be valid there.
std::atomic<std::uint64_t> g_spawnBIndexLive{0};     // B had a live internal index at spawn (repoint target valid)
std::atomic<std::uint64_t> g_spawnBIndexDead{0};     // B's index not live at spawn (HALT -- repoint would dangle)

// The per-bracket order record, thread-local: the verb runs synchronously on the game thread,
// so one slot tracks the outermost bracket, and nested depth is counted as an anomaly through
// the substrate's depth. Reset at each outermost verb entry.
thread_local bool     tls_spawnFormFiredThisBracket = false;
thread_local void*    tls_bracketCtx = nullptr;       // the Context whose bracket owns the TLS record
thread_local E::ElementId tls_bracketEntryEid = E::kInvalidId; // A's eid captured at entry (for re-entry check)

// The deterministic successor, captured in-bracket and consumed once by kerfur_convert at the
// paired destroy edge. Thread-local: the verb and both seams run synchronously on the game
// thread. Cleared at each outermost verb entry and at the request entry (ClearCapturedForm),
// plus a freshness backstop, so a capture never outlives its bracket.
thread_local void*   tls_capturedForm    = nullptr;   // B's actor pointer (nullptr = slot empty)
thread_local int32_t tls_capturedFormIdx = -1;        // B's GUObjectArray internal index (for liveness)
thread_local bool    tls_capturedIsNpc   = false;     // true = kerfurOmega_C (turn-ON B); false = prop (turn-OFF B)
thread_local std::chrono::steady_clock::time_point tls_capturedAt{};  // capture instant (freshness backstop)

// The verbose per-catch log cap; the counters are uncapped and always on.
constexpr int kLogCap = 128;
std::atomic<int> g_logged{0};

// Latched: the row is read once, not on every check of a hot path.
bool LogVerbose() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::vm_dispatch_log);
    return s;
}
bool IsHostRole() { return g_session && g_session->role() == coop::net::Role::Host; }
const char* RoleTag() { return IsHostRole() ? "HOST" : "CLIENT"; }

// Is `cls` a kerfur form, the NPC or the prop family?
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
// Is `cls` the NPC form rather than the prop form? Called only after the form test, to tag the
// captured successor's direction.
bool IsKerfurNpcClass(void* cls) {
    void* nb = g_npcClass.load(std::memory_order_relaxed);
    return nb && R::IsDescendantOfAny(cls, &nb, 1);
}
// Record the freshly finished successor in the one-shot slot, from both the bracket and the
// request-scope branch, only when it has a live index. Overwrites any prior slot.
void StoreCapturedForm(void* b, int32_t idx, void* cls) {
    tls_capturedForm    = b;
    tls_capturedFormIdx = idx;
    tls_capturedIsNpc   = IsKerfurNpcClass(cls);
    tls_capturedAt      = std::chrono::steady_clock::now();
}
// Is the host executing a client's convert request through CallFunction?
bool InReqScope() {
    return coop::kerfur_convert_host::ActiveRequestVerbEid() != E::kInvalidId;
}

// The substrate entry callback, observe-only.
void OnVerbEntry(const vm::Bracket& b) {
    if (b.verbId == kVerbTurnOff) g_catchTurnOff.fetch_add(1, std::memory_order_relaxed);
    else                          g_catchTurnOn.fetch_add(1, std::memory_order_relaxed);

    // Gate 1: the context's eid at entry, a registry map read, not an engine call.
    const E::ElementId entryEid = E::Registry::Get().EidForActor(b.ctx);
    const bool bound = (entryEid != E::kInvalidId);
    if (bound) g_entryEidBound.fetch_add(1, std::memory_order_relaxed);
    else       g_entryEidUnbound.fetch_add(1, std::memory_order_relaxed);

    // Gate 3b: the per-bracket order record resets on the outermost entry. A nested bracket on the
    // same eid is the double-capture hazard: counted, and the outer record is kept.
    if (b.depth <= 1) {
        tls_spawnFormFiredThisBracket = false;
        tls_bracketCtx = b.ctx;
        tls_bracketEntryEid = entryEid;
        tls_capturedForm = nullptr;      // 2a-capture: fresh slot per 0x45 bracket
        tls_capturedFormIdx = -1;
    } else if (bound && entryEid == tls_bracketEntryEid) {
        g_entrySameEidReentry.fetch_add(1, std::memory_order_relaxed);
    }

    if (!LogVerbose()) return;
    if (g_logged.fetch_add(1, std::memory_order_relaxed) >= kLogCap) return;
    const wchar_t* verb = (b.verbId == kVerbTurnOff) ? kVerbNameTurnOff : kVerbNameTurnOn;
    std::wstring cls = R::ClassNameOf(b.ctx);
    UE_LOGI("[kerfur_asm][%s] VERB %ls id=%d Context=%p class=%ls depth=%d eid=%s%u (observe-only)",
            RoleTag(), verb, b.verbId, b.ctx, cls.c_str(), b.depth,
            bound ? "" : "UNBOUND:", bound ? entryEid : 0u);
}

// The spawn seam, a FinishSpawningActor post-hook; the result is the finished actor.
void OnFinishSpawn(void* /*context*/, void* /*sourceObject*/, void* spawnedResult) {
    if (!vm::IsEnabled() || !spawnedResult) return;  // no cost in solo SP
    void* cls = R::ClassOf(spawnedResult);
    if (!cls) return;
    const bool isForm   = IsKerfurFormClass(cls);
    const bool isFloppy = !isForm && IsFloppyClass(cls);
    if (!isForm && !isFloppy) {
        // Gate 2's reject side: an in-window spawn that is neither a form successor nor the floppy
        // (loot, an explosion, anything the conversion graph spawns inside the bracket) must be
        // rejected as a repoint target. Counting it proves the filter rejects a non-successor, a
        // different claim from catching every true successor. Out of window, neither is an ordinary
        // world spawn and is ignored.
        const vm::ActiveVerb av = vm::CurrentThreadVerb();
        if (InKerfurVerb(av)) {
            g_spawnOtherInWindow.fetch_add(1, std::memory_order_relaxed);
            if (LogVerbose() && g_logged.fetch_add(1, std::memory_order_relaxed) < kLogCap) {
                std::wstring cn = R::ClassNameOf(spawnedResult);
                UE_LOGI("[kerfur_asm][%s] SPAWN OTHER IN-WINDOW actor=%p class=%ls verb=%ls depth=%d "
                        "-- filter REJECTS (not form, not floppy; NOT a repoint target)",
                        RoleTag(), spawnedResult, cn.c_str(), av.verbName ? av.verbName : L"<none>", av.depth);
            }
        }
        return;
    }

    const vm::ActiveVerb av = vm::CurrentThreadVerb();
    const bool inVerb  = InKerfurVerb(av);
    const bool reqScope = !inVerb && InReqScope();  // G1: CallFunction route (0x45-blind)
    bool bIndexLive = false;
    int32_t bIdx = -1;
    if (isForm) {
        if (inVerb) {
            g_spawnFormInWindow.fetch_add(1, std::memory_order_relaxed);
            // Gate 3b: the form spawn fired in this bracket, so the later self-destroy can prove
            // the order.
            tls_spawnFormFiredThisBracket = true;
            // Gate 3c: a live index at FinishSpawningActor?
            bIdx = R::InternalIndexOf(spawnedResult);
            bIndexLive = R::IsLiveByIndex(spawnedResult, bIdx);
            if (bIndexLive) { g_spawnBIndexLive.fetch_add(1, std::memory_order_relaxed);
                              StoreCapturedForm(spawnedResult, bIdx, cls); }  // 2a-capture (0x45 route)
            else            g_spawnBIndexDead.fetch_add(1, std::memory_order_relaxed);
        } else if (reqScope) {
            // The request route: this form spawn is the conversion's successor, capturable through
            // the request eid instead of a bracket.
            g_spawnFormInReqScope.fetch_add(1, std::memory_order_relaxed);
            bIdx = R::InternalIndexOf(spawnedResult);
            bIndexLive = R::IsLiveByIndex(spawnedResult, bIdx);
            if (bIndexLive) StoreCapturedForm(spawnedResult, bIdx, cls);      // 2a-capture (CallFunction route)
        } else {
            g_spawnFormOutWindow.fetch_add(1, std::memory_order_relaxed);
        }
    } else {  // floppy
        if (inVerb) g_spawnFloppyInWindow.fetch_add(1, std::memory_order_relaxed);
    }
    if (LogVerbose() && g_logged.fetch_add(1, std::memory_order_relaxed) < kLogCap) {
        std::wstring cn = R::ClassNameOf(spawnedResult);
        const char* scope = inVerb ? "IN-WINDOW" : (reqScope ? "IN-WINDOW(req-scope)" : "out-of-window");
        UE_LOGI("[kerfur_asm][%s] SPAWN %ls actor=%p class=%ls %s verb=%ls depth=%d bIdx=%d bLive=%d reqEid=%d",
                RoleTag(), isForm ? L"FORM" : L"floppy", spawnedResult, cn.c_str(),
                scope, av.verbName ? av.verbName : L"<none>", av.depth, bIdx, bIndexLive ? 1 : 0,
                reqScope ? static_cast<int>(coop::kerfur_convert_host::ActiveRequestVerbEid()) : -1);
    }
}

// The destroy seam, a K2_DestroyActor post-hook; the context is the dying actor.
void OnDestroy(void* context, void* /*sourceObject*/, void* /*result*/) {
    if (!vm::IsEnabled() || !context) return;  // no cost in solo SP
    void* cls = R::ClassOf(context);
    if (!cls || !IsKerfurFormClass(cls)) return;  // only kerfur-class destroys interest us

    const vm::ActiveVerb av = vm::CurrentThreadVerb();
    const char* kind;
    bool orderGood = false;
    if (!InKerfurVerb(av)) {
        if (InReqScope()) {
            // The request route: a kerfur destroy while a request executes is the conversion's own
            // self-destroy.
            g_destroySelfInReqScope.fetch_add(1, std::memory_order_relaxed);
            kind = "IN-WINDOW(req-scope) self";
        } else {
            g_destroyKerfurOutWindow.fetch_add(1, std::memory_order_relaxed);
            kind = "out-of-window";
        }
    } else if (context == av.ctx) {  // IDENTITY invariant: the verb's own self-destroy
        g_destroySelfInWindow.fetch_add(1, std::memory_order_relaxed);
        // Gate 3b: did the form spawn already fire in this bracket? A self-destroy with none
        // strands the eid.
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
        UE_LOGI("[kerfur_asm][%s] DESTROY actor=%p class=%ls %s verb=%ls ctx=%p depth=%d",
                RoleTag(), context, cn.c_str(), kind, av.verbName ? av.verbName : L"<none>", av.ctx, av.depth);
    }
}

// Resolve the classes and patch the two seams; game thread only, retried until everything
// binds, latched once installed.
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
        // The two form bases must resolve before the filters mean anything; the floppy is
        // non-fatal, since a null class just leaves floppy spawns uncounted.
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
    // The observe gates are green only when every halt signal is zero: unbound, same-eid
    // re-entry, destroy without spawn, dead index.
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
    // The substrate underneath, which counts what the assembler above cannot see. offGtMatch is
    // the one that must stay zero: a watched verb matching off the game thread means a capture ran
    // where the engine calls are illegal. gtDispatch at zero with the hook installed says the
    // 0x45 path never carried a dispatch, which reads the same as a quiet session and is not.
    const vm::Stats vs = vm::GetStats();
    UE_LOGI("[kerfur_asm][%s] VM SUBSTRATE (%s): installed=%d enabled=%d verbs{registered=%d resolved=%d} "
            "dispatch{gt=%llu worker=%llu} nameMatch=%llu callbackFired=%llu OFF_GT_MATCH=%llu "
            "-- GREEN iff OFF_GT_MATCH=0 and, with verbs registered, gt>0",
            RoleTag(), when, vs.installed ? 1 : 0, vs.enabled ? 1 : 0,
            vs.registeredVerbs, vs.resolvedVerbs,
            vs.gtDispatch, vs.workerDispatch, vs.nameMatch, vs.callbackFired, vs.offGtMatch);
}

}  // namespace

// The capture accessors, consumed by kerfur_convert.
CapturedForm ConsumeCapturedForm(bool wantNpc) {
    CapturedForm out{nullptr, -1};
    if (!tls_capturedForm) return out;                       // slot empty
    // The freshness backstop: the store is one-shot and cleared at each bracket entry, but a
    // conversion that spawned no matching successor and then consumes could otherwise pull a
    // prior bracket's; a capture is valid only within its own sub-second verb window.
    if (std::chrono::steady_clock::now() - tls_capturedAt > std::chrono::seconds(2)) {
        tls_capturedForm = nullptr; tls_capturedFormIdx = -1;
        return out;
    }
    if (tls_capturedIsNpc != wantNpc) return out;            // wrong direction: leave for the right consumer
    if (!R::IsLiveByIndex(tls_capturedForm, tls_capturedFormIdx)) {  // died since capture
        tls_capturedForm = nullptr; tls_capturedFormIdx = -1;
        return out;
    }
    out.actor = tls_capturedForm;
    out.idx   = tls_capturedFormIdx;
    tls_capturedForm = nullptr; tls_capturedFormIdx = -1;    // one-shot
    return out;
}

void ClearCapturedForm() {
    tls_capturedForm = nullptr;
    tls_capturedFormIdx = -1;
}

bool IsCapturedForm(void* actor) {
    // A non-consuming peek: is `actor` the successor in the capture slot? The keyed-express
    // suppressor in prop_lifecycle reads it to decide that this prop is the conversion's
    // successor, whose express KerfurConvert owns, and skips the generic spawn. It must not
    // consume: the deferred converge still needs the slot.
    if (!actor || actor != tls_capturedForm) return false;
    if (std::chrono::steady_clock::now() - tls_capturedAt > std::chrono::seconds(2)) return false;
    return R::IsLiveByIndex(tls_capturedForm, tls_capturedFormIdx);
}

void Install(coop::net::Session* session) {
    g_session = session;
    vm::RegisterVirtualVerb(kVerbNameTurnOff, kVerbTurnOff, &OnVerbEntry);
    vm::RegisterVirtualVerb(kVerbNameTurnOn,  kVerbTurnOn,  &OnVerbEntry);
    vm::SetEnabled(true);  // both roles: the client needs the bracket to observe its own conversion
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
