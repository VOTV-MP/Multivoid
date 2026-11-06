// coop/creatures/walker_brain.cpp -- see coop/creatures/walker_brain.h.

#include "coop/creatures/walker_brain.h"

#include "coop/net/session.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/facts/function_row.h"

#include <chrono>
#include <cstdint>
#include <cstring>

namespace coop::walker_brain {
namespace {

namespace AC = ue_wrap::ambient_creature;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;
namespace UH = ue_wrap::ufunction_hook;

// The walker's event graph, the one body every entry of its Blueprint runs through, and the native its BeginPlay spawns the
// nav pawn with: the post hook sees the pawn after the spawn and before the finish.
UE_FACT_FN(kRowGraph, "walker_brain", L"prop_wMannequin_C", L"ExecuteUbergraph_prop_wMannequin", Declared);
UE_FACT_FN(kRowBeginDeferred, "walker_brain", L"GameplayStatics", L"BeginDeferredActorSpawnFromClass", Declared);

constexpr int kTagGraph = 0x574B4230;  // 'WKB0'

// The entries a copy runs: the BeginPlay and the destroy. The graph has thirteen; the other eleven are refused.
constexpr int32_t kRuns[] = {4411, 5249};

constexpr uint64_t kRetryMs = 1000;
constexpr int kMaxTries = 30;

R::ParamOffset g_entryParam{L"EntryPoint"};

bool g_graphAsked = false;
bool g_hooked = false;
bool g_armed = false;  // the hook sits on a native every Blueprint spawn calls: it calls back only while a client session runs
void* g_hookFn = nullptr;
bool g_gaveUp = false;
int g_tries = 0;
uint64_t g_nextTryMs = 0;

// What the gate refused, each entry counted and said at its first, and the pawns made inert.
struct Refusal {
    int32_t  entry;
    uint32_t count;
};
constexpr size_t kMaxRefusals = 16;  // the graph has thirteen entries
Refusal  g_refusals[kMaxRefusals];
size_t   g_refusalCount = 0;
uint32_t g_refusalsUnlisted = 0;
uint32_t g_inert = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void NoteRefused(int32_t entry) {
    for (size_t i = 0; i < g_refusalCount; ++i) {
        if (g_refusals[i].entry == entry) {
            ++g_refusals[i].count;
            return;
        }
    }
    if (g_refusalCount < kMaxRefusals) {
        g_refusals[g_refusalCount++] = Refusal{entry, 1};
        UE_LOGI("walker_brain: a walker copy's graph entry %d refused", entry);
        return;
    }
    ++g_refusalsUnlisted;
}

// Before the walker's graph runs, on a client: an entry a copy runs passes, every other is refused. The role is the whole
// test (the header says why), and an entry that cannot be read is refused with the rest.
SG::Verdict OnGraphPre(const SG::Call& call) {
    if (!coop::spawn_authority::IsActiveClientSession()) return SG::Verdict::Run;
    int32_t entry = -1;
    const int32_t off = g_entryParam.Of(call.function);
    if (off >= 0 && call.locals) std::memcpy(&entry, call.locals + off, sizeof(entry));
    for (const int32_t run : kRuns)
        if (entry == run) return SG::Verdict::Run;
    NoteRefused(entry);
    return SG::Verdict::Cancel;
}

// After any deferred spawn the game thread runs, on a client: one whose source is a walker is its nav pawn, made inert before
// the finish runs the pawn's BeginPlay, so the BeginPlay takes no controller and the walker's move toward a camera fails.
// MakePawnInert leaves a spawn that is not a pawn alone, so the walker's weapon spawns pass untouched.
void OnBeginDeferredPost(void* /*context*/, void* source, void* spawned) {
    if (!spawned || !source || !coop::spawn_authority::IsActiveClientSession()) return;
    if (!AC::IsWalker(source)) return;
    if (E::MakePawnInert(spawned) != E::PawnInertResult::Made) return;
    if (++g_inert == 1) UE_LOGI("walker_brain: a walker copy's nav pawn is made inert at its spawn (no controller)");
}

void InstallSpawnHook() {
    if (g_gaveUp) return;
    if (g_hooked) {
        if (!g_armed) g_armed = UH::SetArmed(g_hookFn, &OnBeginDeferredPost, true);
        return;
    }
    const uint64_t now = NowMs();
    if (now < g_nextTryMs) return;
    g_nextTryMs = now + kRetryMs;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowBeginDeferred);
    if (fn && UH::InstallPostHook(fn, &OnBeginDeferredPost, /*armed=*/false)) {
        g_hooked = true;
        g_hookFn = fn.Fn();
        g_armed = UH::SetArmed(g_hookFn, &OnBeginDeferredPost, true);
        UE_LOGI("walker_brain: watching deferred spawns for a walker's nav pawn (made inert on a client)");
    } else if (++g_tries >= kMaxTries) {
        g_gaveUp = true;
        UE_LOGE("walker_brain: BeginDeferredActorSpawnFromClass did not resolve or would not take a post hook in %d tries -- a "
                "walker copy's nav pawn keeps its AI controller", kMaxTries);
    }
}

}  // namespace

void Install(coop::net::Session& session) {
    if (session.role() != coop::net::Role::Client) return;
    if (!g_graphAsked) {
        g_graphAsked = true;
        if (SG::WatchName(kRowGraph, kTagGraph, &OnGraphPre, nullptr))
            UE_LOGI("walker_brain: the walker's graph watch is registered (a client's copy runs entries 4411 and 5249 only)");
        else
            UE_LOGE("walker_brain: the watch on the walker's graph was refused -- a client's walker copy thinks");
    }
    InstallSpawnHook();
}

void OnDisconnect() {
    for (size_t i = 0; i < g_refusalCount; ++i)
        UE_LOGI("walker_brain: graph entry %d: %u calls refused over the session", g_refusals[i].entry, g_refusals[i].count);
    if (g_refusalsUnlisted > 0) UE_LOGI("walker_brain: %u calls of entries past the list refused over the session", g_refusalsUnlisted);
    if (g_inert > 0) UE_LOGI("walker_brain: %u nav pawns made inert over the session", g_inert);
    g_refusalCount = 0;
    g_refusalsUnlisted = 0;
    g_inert = 0;
    if (g_armed && UH::SetArmed(g_hookFn, &OnBeginDeferredPost, false)) g_armed = false;
}

}  // namespace coop::walker_brain
