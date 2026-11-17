// coop/props/prop_wire_parity.cpp -- see coop/props/prop_wire_parity.h.
//
// The log prefixes read "remote_prop::OnSpawn" because that is where these bodies sat before;
// the wording is kept so older log captures stay searchable.

#include "coop/props/prop_wire_parity.h"

#include "coop/element/registry.h"         // EidForActor: the id a correction line names
#include "coop/net/protocol.h"
#include "coop/props/body_push.h"          // Rebase: a write of this peer's own is no push
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"         // TryGetActorLocation, IsComponentSimulatingPhysics
#include "ue_wrap/engine/engine_attach.h"  // IsActorRootBodyAtRest

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace coop::prop_wire_parity {

namespace R = ue_wrap::reflection;

// ==== RULE 1 RETIREMENT PLAN ====
// The collision restore is an INTERIM fix at the SYMPTOM (collision write at OnSpawn-time). The
// architecturally correct ROOT-CAUSE fix is to suppress AmushroomSpawner_C::Spawn on the client
// entirely, so the local actor never goes through spawnedNaturally() in the first place. Gating
// criteria for retirement:
//   (1) A single BeginDeferredActorSpawnFromClass observer ships, with a class allowlist that
//       includes AmushroomSpawner_C (covers all natural spawners at once).
//   (2) Hands-on test confirms mushrooms sync correctly with this restore call commented out.
//   (3) Then remove IsCollisionRestoreClass + RestoreCollisionIfNeeded + the
//       PropFoodMushroomClass constant. Per RULE 2 no parallel paths.
bool IsCollisionRestoreClass(const std::wstring& cls) {
    return cls == ue_wrap::profile::name::PropFoodMushroomClass;
}

void RestoreCollisionIfNeeded(const wchar_t* pathLabel,
                              const std::wstring& classW,
                              void* actor) {
    if (!IsCollisionRestoreClass(classW)) return;
    const bool ok = ue_wrap::prop::ForceRestoreDefaultCollision(actor);
    if (ok) {
        UE_LOGI("remote_prop::OnSpawn[%ls]: collision restored (QueryAndPhysics) on '%ls' actor=%p",
                pathLabel, classW.c_str(), actor);
    } else {
        UE_LOGW("remote_prop::OnSpawn[%ls]: collision restore FAILED on '%ls' actor=%p -- prop may fall through ground",
                pathLabel, classW.c_str(), actor);
    }
}

void ReconcileToHostPhysics(void* actor, uint8_t physFlags) {
    const bool hostSimulating =
        (physFlags & coop::net::propspawn_flags::kSimulatePhysics) != 0;
    if (hostSimulating) return;
    if (!actor || !R::IsLive(actor)) return;
    void* mesh = ue_wrap::prop::GetStaticMesh(actor);  // Aprop_C only; null for non-Aprop_C
    if (!mesh) return;
    ue_wrap::engine::SetComponentSimulatePhysics(mesh, /*simulate=*/false);
}

uint8_t PhysFlagsOf(void* actor) {
    namespace pf = coop::net::propspawn_flags;
    if (!actor || !ue_wrap::prop::IsDescendantOfProp(actor)) return 0;  // the offsets are the prop's
    uint8_t flags = pf::kLiveState;
    const bool isStatic = ue_wrap::prop::IsStatic(actor);
    const bool frozen   = ue_wrap::prop::IsFrozen(actor);
    const bool sleep    = ue_wrap::prop::IsSleeping(actor);
    if (!(isStatic || frozen || sleep) && !ue_wrap::engine::IsActorRootBodyAtRest(actor))
        flags |= pf::kSimulatePhysics;
    if (ue_wrap::prop::IsHeavy(actor)) flags |= pf::kIsHeavy;
    if (frozen)   flags |= pf::kFrozen;
    if (isStatic) flags |= pf::kStatic;
    if (sleep)    flags |= pf::kSleep;
    if (ue_wrap::prop::ReadRemoveWOrespawn(actor)) flags |= pf::kRemoveWOrespawn;
    return flags;
}

uint8_t FlagBitsOf(void* actor) {
    namespace pf = coop::net::propspawn_flags;
    if (!actor || !ue_wrap::prop::IsDescendantOfProp(actor)) return 0;
    return static_cast<uint8_t>((ue_wrap::prop::IsStatic(actor) ? pf::kStatic : 0) |
                                (ue_wrap::prop::IsFrozen(actor) ? pf::kFrozen : 0) |
                                (ue_wrap::prop::IsSleeping(actor) ? pf::kSleep : 0));
}

void ConvergeFrozenSleep(void* actor, uint8_t physFlags) {
    namespace pf = coop::net::propspawn_flags;
    if (!(physFlags & pf::kLiveState) || !actor || !ue_wrap::prop::IsDescendantOfProp(actor)) return;
    const bool wantFrozen = (physFlags & pf::kFrozen) != 0;
    const bool wantSleep  = (physFlags & pf::kSleep) != 0;
    const bool isFrozen = ue_wrap::prop::IsFrozen(actor);
    const bool isSleep  = ue_wrap::prop::IsSleeping(actor);
    if (isFrozen == wantFrozen && isSleep == wantSleep) return;
    const bool ok = (!wantFrozen && !wantSleep)
        ? ue_wrap::prop::CallAwakeUnfreeze(actor)
        : ue_wrap::prop::CallBaseSetPropProps(actor, ue_wrap::prop::IsStatic(actor), wantFrozen, wantSleep);
    if (!ok) {
        UE_LOGW("prop_wire_parity: %p frozen=%d sleep=%d -> want frozen=%d sleep=%d -- the game's verb did "
                "not dispatch; the copy keeps its flags", actor, isFrozen ? 1 : 0, isSleep ? 1 : 0,
                wantFrozen ? 1 : 0, wantSleep ? 1 : 0);
    }
}

void ConvergeStatic(void* actor, uint8_t physFlags) {
    namespace pf = coop::net::propspawn_flags;
    if (!(physFlags & pf::kLiveState) || !actor || !ue_wrap::prop::IsDescendantOfProp(actor)) return;
    const bool wantStatic = (physFlags & pf::kStatic) != 0;
    if (ue_wrap::prop::IsStatic(actor) == wantStatic) return;
    const bool wantFrozen = (physFlags & pf::kFrozen) != 0;
    const bool wantSleep  = (physFlags & pf::kSleep) != 0;
    if (!ue_wrap::prop::CallBaseSetPropProps(actor, wantStatic, wantFrozen, wantSleep)) {
        UE_LOGW("prop_wire_parity: %p static -> %d, frozen=%d sleep=%d -- the game's setter did not dispatch; the "
                "copy keeps its flags", actor, wantStatic ? 1 : 0, wantFrozen ? 1 : 0, wantSleep ? 1 : 0);
    }
}

bool SpParitySimulate(uint8_t physFlags) {
    namespace pf = coop::net::propspawn_flags;
    return (physFlags & (pf::kStatic | pf::kFrozen | pf::kSleep)) == 0;
}

void RestoreSpParityPhysicsAfterConverge(void* actor, uint8_t physFlags) {
    if (!actor || !R::IsLive(actor)) return;
    void* mesh = ue_wrap::prop::GetStaticMesh(actor);
    if (!mesh) return;
    ue_wrap::engine::SetComponentSimulatePhysics(mesh, SpParitySimulate(physFlags));
}

namespace {

constexpr float    kNoteMinCm      = 3.f;     // a smaller move is jitter, not a correction
constexpr float    kNoteFarCm      = 10.f;    // the summary counts these apart
constexpr uint64_t kNoteWindowMs   = 5000;
constexpr int      kNoteLinesPerWindow = 6;
constexpr uint64_t kSummaryEveryMs = 30000;
constexpr size_t   kMaxPaths       = 16;

struct PathTally {
    const char* path = nullptr;   // a literal: compared by pointer, then by text
    uint32_t    n = 0, far = 0;
    float       sumCm = 0.f, maxCm = 0.f;
};
PathTally g_tally[kMaxPaths];     // game thread only
uint64_t  g_windowStartMs = 0;
int       g_windowLines   = 0;
uint32_t  g_suppressed    = 0;    // counted since the last line that went out
uint64_t  g_nextSummaryMs = 0;
bool      g_anyTallied    = false;

uint64_t SteadyMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

PathTally* TallyFor(const char* path) {
    for (PathTally& t : g_tally) {
        if (t.path == path || (t.path && std::strcmp(t.path, path) == 0)) return &t;
        if (!t.path) { t.path = path; return &t; }
    }
    return nullptr;   // more paths than slots: still logged, not tallied
}

}  // namespace

void NoteCorrection(void* actor, const ue_wrap::FVector& to, const char* path) {
    UE_ASSERT_GAME_THREAD("prop_wire_parity::NoteCorrection");
    // Every path here writes the prop's place next: the push poll must not read that write as a body's push.
    if (actor) coop::props::body_push::Rebase(actor);
    ue_wrap::FVector from{};
    if (!actor || !path || !ue_wrap::engine::TryGetActorLocation(actor, from)) return;
    const float dx = to.X - from.X, dy = to.Y - from.Y, dz = to.Z - from.Z;
    const float cm = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(cm >= kNoteMinCm)) return;   // also drops a NaN
    if (PathTally* t = TallyFor(path)) {
        ++t->n;
        if (cm >= kNoteFarCm) ++t->far;
        t->sumCm += cm;
        if (cm > t->maxCm) t->maxCm = cm;
        g_anyTallied = true;
    }
    const uint64_t now = SteadyMs();
    if (g_nextSummaryMs == 0) g_nextSummaryMs = now + kSummaryEveryMs;
    if (now - g_windowStartMs >= kNoteWindowMs) { g_windowStartMs = now; g_windowLines = 0; }
    if (g_windowLines >= kNoteLinesPerWindow) { ++g_suppressed; return; }
    ++g_windowLines;
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(actor);
    const char* body = "kinematic";
    if (void* mesh = ue_wrap::prop::GetStaticMesh(actor);
        mesh && ue_wrap::engine::IsComponentSimulatingPhysics(mesh))
        body = ue_wrap::engine::IsActorRootBodyAtRest(actor) ? "asleep" : "awake";
    UE_LOGI("[PROP-CORRECT] eid=%u key='%ls' path=%s moved=%.1f cm (dz %.1f) body=%s%s",
            static_cast<unsigned>(eid == coop::element::kInvalidId ? 0u : eid),
            ue_wrap::prop::GetInteractableKeyString(actor).c_str(), path, cm, dz, body,
            g_suppressed ? " (more suppressed since the last line)" : "");
    g_suppressed = 0;
}

void OnDisconnect() {
    for (PathTally& t : g_tally) t = PathTally{};
    g_windowStartMs = 0;
    g_windowLines   = 0;
    g_suppressed    = 0;
    g_nextSummaryMs = 0;
    g_anyTallied    = false;
}

void FlushCorrectionSummary(uint64_t nowMs) {
    if (!g_anyTallied || nowMs < g_nextSummaryMs) return;
    g_nextSummaryMs = nowMs + kSummaryEveryMs;
    char line[512];
    size_t used = 0;
    for (PathTally& t : g_tally) {
        if (!t.path || t.n == 0) continue;
        const int w = std::snprintf(line + used, sizeof(line) - used, "%s%s n=%u far=%u mean=%.1f max=%.1f cm",
                                    used ? "; " : "", t.path, t.n, t.far, t.sumCm / static_cast<float>(t.n), t.maxCm);
        if (w < 0 || static_cast<size_t>(w) >= sizeof(line) - used) break;
        used += static_cast<size_t>(w);
        t.n = t.far = 0;
        t.sumCm = t.maxCm = 0.f;
    }
    g_anyTallied = false;
    if (used) UE_LOGI("[PROP-CORRECT] last 30 s: %s", line);
}

}  // namespace coop::prop_wire_parity
