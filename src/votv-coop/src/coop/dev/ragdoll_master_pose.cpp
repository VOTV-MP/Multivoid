// coop/dev/ragdoll_master_pose.cpp -- see coop/dev/ragdoll_master_pose.h.

#include "coop/dev/ragdoll_master_pose.h"

#include "coop/dev/dev_gate.h"
#include "coop/session/ini_config.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <array>
#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::dev::ragdoll_master_pose {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;

namespace {

std::atomic<bool> g_enabled{false};

// One live coupling per flopping puppet (keyed by puppet actor; <= peer count entries).
// Game-thread only: Drive/Stop run inside RagdollDisplay, which runs in ApplyToEngine.
struct Coupling {
    void*   meshA = nullptr;  int32_t idxA = -1;  // ACharacter::Mesh @0x0280 (parent slot)
    void*   meshB = nullptr;  int32_t idxB = -1;  // mesh_playerVisible @0x04F8 (child slot)
    ue_wrap::FVector  relLocA{}, relLocB{};       // saved attach-relative transforms,
    ue_wrap::FRotator relRotA{}, relRotB{};       // restored on recover
};
std::unordered_map<void*, Coupling> g_live;

// Un-slave + restore one mesh. Liveness-guarded: the recover edge can race component
// death (level travel / puppet destroy) -- a dead comp just skips, the engine is
// tearing it down anyway.
void RestoreMesh_(void* mesh, int32_t idx,
                  const ue_wrap::FVector& relLoc, const ue_wrap::FRotator& relRot) {
    if (!mesh || !R::IsLiveByIndex(mesh, idx)) return;
    E::SetSkinnedMeshMasterPose(mesh, nullptr);
    E::SetComponentRelativeLocationAndRotation(mesh, relLoc, relRot);
}

void Restore_(void* puppetActor, const Coupling& c, const char* why) {
    RestoreMesh_(c.meshA, c.idxA, c.relLocA, c.relRotA);
    RestoreMesh_(c.meshB, c.idxB, c.relLocB, c.relRotB);
    UE_LOGI("[MASTER-POSE] restored puppet=%p (%s) -- kel meshes back on their own AnimBP",
            puppetActor, why);
}

// Coverage diagnostic (once per successful apply): enumerate the MASTER rig's bones and
// probe each name against the slave. THE decisive line -- 6/6 means every bone couples;
// a miss names the bone that will ride its ref-pose local instead.
void LogCoverage_(void* master, void* slave) {
    std::vector<std::array<uint8_t, 8>> fnames;
    const int n = E::CollectSkeletonBoneFNames(master, fnames);
    if (n <= 0) { UE_LOGW("[MASTER-POSE] coverage: master rig enumerated 0 bones"); return; }
    std::string acc;
    int mapped = 0;
    for (int i = 0; i < n; ++i) {
        const bool hit = E::GetBoneIndexByFName(slave, fnames[static_cast<size_t>(i)].data()) >= 0;
        mapped += hit ? 1 : 0;
        const std::wstring w =
            R::ToString(*reinterpret_cast<const R::FName*>(fnames[static_cast<size_t>(i)].data()));
        std::string asc; asc.reserve(w.size());
        for (wchar_t ch : w) asc.push_back(static_cast<char>(ch < 0x80 ? ch : '?'));
        acc += asc; acc += hit ? "=Y " : "=MISS ";
    }
    UE_LOGI("[MASTER-POSE] coverage %d/%d master-rig bones exist on the kel slave: %s",
            mapped, n, acc.c_str());
}

bool Apply_(void* puppetActor, void* body) {
    void* master = E::GetRagdollBodyMesh(body);
    if (!master) return false;
    void* meshA = ue_wrap::puppet::GetNativeBodyMeshComponent(puppetActor);
    void* meshB = ue_wrap::puppet::GetMeshPlayerVisibleComponent(puppetActor);
    if (!meshA || !R::IsLive(meshA) || !meshB || !R::IsLive(meshB)) return false;

    // Eviction sweep: a puppet destroyed MID-flop (peer left) never reaches Stop with
    // its key -- drop any entry whose parent-slot mesh is gone before inserting a new
    // one, so the map stays <= live-peer-sized (the g_boneGraphs valve precedent).
    for (auto it = g_live.begin(); it != g_live.end();) {
        if (!R::IsLiveByIndex(it->second.meshA, it->second.idxA)) it = g_live.erase(it);
        else ++it;
    }

    Coupling c;
    c.meshA = meshA; c.idxA = R::InternalIndexOf(meshA);
    c.meshB = meshB; c.idxB = R::InternalIndexOf(meshB);
    c.relLocA = E::GetComponentRelativeLocation(meshA);
    c.relRotA = E::GetComponentRelativeRotation(meshA);
    c.relLocB = E::GetComponentRelativeLocation(meshB);
    c.relRotB = E::GetComponentRelativeRotation(meshB);

    // Slave BOTH kel slots (both render -- [[lesson-attachparent-visibility-two-body]]).
    // All-or-nothing: a half-coupled pair would render one animated + one posed body
    // diverging from each other.
    if (!E::SetSkinnedMeshMasterPose(meshA, master)) {
        UE_LOGW("[MASTER-POSE] apply failed on the native slot -- probe inert this tick");
        return false;
    }
    if (!E::SetSkinnedMeshMasterPose(meshB, master)) {
        E::SetSkinnedMeshMasterPose(meshA, nullptr);
        UE_LOGW("[MASTER-POSE] apply failed on mesh_playerVisible -- rolled back");
        return false;
    }
    g_live[puppetActor] = c;
    UE_LOGI("[MASTER-POSE] applied puppet=%p master=%p (both kel slots slaved; world-pin active)",
            puppetActor, master);
    LogCoverage_(master, meshB);
    return true;
}

// Pin both slave components' WORLD transform onto the master component. Master-pose
// copies bones in COMPONENT space -- without this the pose renders rigidly offset by
// the slave-vs-master component delta. Parent slot FIRST (the child rides its move),
// then the child exactly. ~4 UFunction calls per attached tick: a ragdoll-scoped dev
// budget (runs only while a peer is flopping AND the probe is ON).
void Snap_(const Coupling& c, void* master) {
    const ue_wrap::FVector  loc = E::GetComponentLocation(master);
    const ue_wrap::FRotator rot = E::GetComponentWorldRotation(master);
    if (c.meshA && R::IsLiveByIndex(c.meshA, c.idxA))
        E::SetComponentWorldLocationAndRotation(c.meshA, loc, rot);
    if (c.meshB && R::IsLiveByIndex(c.meshB, c.idxB))
        E::SetComponentWorldLocationAndRotation(c.meshB, loc, rot);
}

}  // namespace

void InitFromIni() {
    if (::coop::ini_config::MasterEnabled() &&
        ::coop::ini_config::IsIniKeyTrue("ragdoll_master_pose")) {
        g_enabled.store(true, std::memory_order_release);
        UE_LOGI("ragdoll_master_pose: force-enabled via ini");
    }
}

bool IsEnabled() {
    return g_enabled.load(std::memory_order_acquire) && coop::dev_gate::Allowed();
}

void SetEnabled(bool on) {
    g_enabled.store(on, std::memory_order_release);
    UE_LOGI("ragdoll_master_pose: %s", on ? "ON" : "OFF");
}

void Drive(void* puppetActor, int32_t puppetIdx, void* body, int32_t bodyIdx) {
    auto it = g_live.find(puppetActor);
    // Stale entry from a RECYCLED puppet address (old puppet died mid-flop, new one got
    // the same pointer): the old comps fail liveness -- drop it so the new flop applies.
    if (it != g_live.end() && !R::IsLiveByIndex(it->second.meshA, it->second.idxA)) {
        g_live.erase(it);
        it = g_live.end();
    }
    const bool on = g_enabled.load(std::memory_order_acquire) && coop::dev_gate::Allowed();
    if (!on) {
        if (it != g_live.end()) { Restore_(puppetActor, it->second, "toggled off mid-flop"); g_live.erase(it); }
        return;
    }
    if (!puppetActor || !R::IsLiveByIndex(puppetActor, puppetIdx)) {
        if (it != g_live.end()) g_live.erase(it);  // puppet died; its comps die with it
        return;
    }
    void* master = (body && R::IsLiveByIndex(body, bodyIdx)) ? E::GetRagdollBodyMesh(body) : nullptr;
    if (!master) {
        // Body/mesh died under the coupling (RagdollDisplay self-heals its attach the
        // same tick) -- restore NOW so the kel isn't left slaved to a dead master.
        if (it != g_live.end()) { Restore_(puppetActor, it->second, "master died"); g_live.erase(it); }
        return;
    }
    if (it == g_live.end()) {
        if (!Apply_(puppetActor, body)) return;  // retried next tick; failure is loud
        it = g_live.find(puppetActor);
        if (it == g_live.end()) return;
    }
    Snap_(it->second, master);
}

void Stop(void* puppetActor) {
    auto it = g_live.find(puppetActor);
    if (it == g_live.end()) return;
    Restore_(puppetActor, it->second, "recover");
    g_live.erase(it);
}

}  // namespace coop::dev::ragdoll_master_pose
