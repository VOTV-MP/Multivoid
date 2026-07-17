// coop/dev/ragdoll_bone_overlay.cpp -- see coop/dev/ragdoll_bone_overlay.h.

#include "coop/dev/ragdoll_bone_overlay.h"

#include "coop/dev/dev_gate.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/config/config.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

namespace coop::dev::ragdoll_bone_overlay {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;

namespace {

std::atomic<bool> g_enabled{false};

int  g_tick = 0;
bool g_publishedAnything = false;

std::mutex g_mu;
Snapshot   g_snap;

void Publish_(const Snapshot& s) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_snap = s;
    }
    g_publishedAnything = (s.count != 0 || s.status[0] != '\0');
}

void PublishEmpty_(const char* status) {
    Snapshot s;
    if (status) std::snprintf(s.status, sizeof(s.status), "%s", status);
    Publish_(s);
}

// Project one mesh's skeleton into `snap` as bone->parent screen lines. The bone points
// come from the per-component graph cache (ue_wrap CollectSkeletonBonePoints); every bone
// is projected ONCE into a scratch table, then each parented pair emits a line -- so the
// per-bone UFunction cost is 1 GetSocketLocation + 1 ProjectWorldToScreen, not doubled.
void AppendMeshSkeleton_(void* pc, void* mesh, uint8_t kind, Snapshot& snap,
                         std::vector<E::BonePoint>& scratchBones,
                         std::vector<ue_wrap::FVector2D>& scratchScr,
                         std::vector<uint8_t>& scratchOk, int& bonesOut) {
    const int n = E::CollectSkeletonBonePoints(mesh, scratchBones);
    if (n <= 0) return;
    bonesOut += n;
    scratchScr.assign(static_cast<size_t>(n), {});
    scratchOk.assign(static_cast<size_t>(n), 0);
    for (int i = 0; i < n; ++i) {
        ue_wrap::FVector2D s{};
        if (E::ProjectWorldToScreen(pc, scratchBones[static_cast<size_t>(i)].world, s, false)) {
            scratchScr[static_cast<size_t>(i)] = s;
            scratchOk[static_cast<size_t>(i)] = 1;
        }
    }
    for (int i = 0; i < n && snap.count < kMaxLines; ++i) {
        const int32_t p = scratchBones[static_cast<size_t>(i)].parent;
        if (p < 0 || p >= n) continue;                                       // root
        if (!scratchOk[static_cast<size_t>(i)] || !scratchOk[static_cast<size_t>(p)]) continue;  // behind camera
        Line& L = snap.lines[snap.count++];
        L.x1 = scratchScr[static_cast<size_t>(i)].X;
        L.y1 = scratchScr[static_cast<size_t>(i)].Y;
        L.x2 = scratchScr[static_cast<size_t>(p)].X;
        L.y2 = scratchScr[static_cast<size_t>(p)].Y;
        L.kind = kind;
    }
}

}  // namespace

void InitFromIni() {
    if (::coop::config::MasterEnabled() &&
        ::coop::config::IsIniKeyTrue("ragdoll_bone_overlay")) {
        g_enabled.store(true, std::memory_order_release);
        UE_LOGI("ragdoll_bone_overlay: force-enabled via ini");
    }
}

void Update() {
    if (!g_enabled.load(std::memory_order_acquire) || !coop::dev_gate::Allowed()) {
        if (g_publishedAnything) PublishEmpty_(nullptr);  // self-clear once on toggle-off
        return;
    }

    // 30 Hz: every other ~60 Hz pump tick (the object_overlay cadence) -- halves the
    // per-bone UFunction load; visually indistinguishable for a debug skeleton.
    if (++g_tick & 1) return;

    void* lp = coop::players::Registry::Get().Local();
    if (!lp) {
        PublishEmpty_("ragdoll bones: waiting for player/world");
        return;
    }
    void* pc = E::GetController(lp);
    if (!pc) {
        PublishEmpty_("ragdoll bones: no controller");
        return;
    }

    // Collect the ACTIVE ragdoll meshes: the local native ragdoll (non-null exactly while
    // ragdolled -- the C key / faint / trip) + every remote peer's mirror body.
    struct Target { void* mesh; uint8_t kind; };
    Target targets[coop::players::kMaxPeers + 1];
    int nTargets = 0;
    if (void* localMesh = E::GetLocalRagdollBodyMesh(lp))
        targets[nTargets++] = {localMesh, 0};
    int remoteBodies = 0;
    for (int slot = 0; slot < static_cast<int>(coop::players::kMaxPeers); ++slot) {
        coop::RemotePlayer& rp = coop::net_pump::Puppet(slot);
        if (!rp.IsRagdollDisplayed()) continue;
        void* body = rp.RagdollBody();
        if (!body || !R::IsLiveByIndex(body, rp.RagdollBodyIdx())) continue;
        if (void* mesh = E::GetRagdollBodyMesh(body)) {
            targets[nTargets++] = {mesh, 1};
            ++remoteBodies;
        }
    }

    if (nTargets == 0) {
        PublishEmpty_("ragdoll bones: no active ragdoll (press C, or wait for a peer to flop)");
        return;
    }

    // Scratch buffers persist across updates (no per-tick allocation once warm).
    static std::vector<E::BonePoint> sBones;
    static std::vector<ue_wrap::FVector2D> sScr;
    static std::vector<uint8_t> sOk;

    Snapshot snap;
    int bones = 0;
    for (int t = 0; t < nTargets; ++t)
        AppendMeshSkeleton_(pc, targets[t].mesh, targets[t].kind, snap, sBones, sScr, sOk, bones);
    std::snprintf(snap.status, sizeof(snap.status),
                  "ragdoll bones: %d line(s), %d bone(s) | local=%s remote bodies=%d",
                  snap.count, bones, (nTargets > 0 && targets[0].kind == 0) ? "YES" : "no",
                  remoteBodies);
    Publish_(snap);
}

void GetSnapshot(Snapshot& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    out = g_snap;
}

bool IsEnabled() {
    return g_enabled.load(std::memory_order_acquire) && coop::dev_gate::Allowed();
}

void SetEnabled(bool on) {
    g_enabled.store(on, std::memory_order_release);
    UE_LOGI("ragdoll_bone_overlay: %s", on ? "ON" : "OFF");
}

}  // namespace coop::dev::ragdoll_bone_overlay
