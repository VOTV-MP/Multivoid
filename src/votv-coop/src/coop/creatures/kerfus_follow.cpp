// coop/creatures/kerfus_follow.cpp -- see coop/creatures/kerfus_follow.h.

#include "coop/creatures/kerfus_follow.h"

#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/actors/kerfus.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"

#include <atomic>
#include <cstdint>
#include <unordered_map>

namespace coop::kerfus_follow {
namespace {

namespace GT = ue_wrap::game_thread;
namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace UH = ue_wrap::ufunction_hook;
namespace UK = ue_wrap::kerfus;

constexpr int kTagTarget = 0x4B464601;  // 'KFF' 1

std::atomic<coop::net::Session*> g_session{nullptr};
bool  g_targetWatched = false;
bool  g_saidLive = false;
void* g_gpcFn = nullptr;       // GameplayStatics::GetPlayerCharacter
bool  g_hookInstalled = false;
bool  g_hookRefused = false;   // the seam cannot install in this process: not asked again
bool  g_armed = false;

// The Kerfuses a client pressed, by the actor, its slot index re-checked. `on` once the tick has seen
// the press turn it on: a record whose Kerfus is off afterwards -- pressed off, out of energy, a
// possess arrived -- goes.
struct Rec {
    int32_t idx;
    uint8_t slot;
    bool    on = false;
};
std::unordered_map<void*, Rec> g_activator;
unsigned long long g_targetsAnswered = 0, g_readsAnswered = 0;

bool OnHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Host;
}

// A slot's body on the host: its puppet, once it has taken a pose.
void* PawnOf(uint8_t slot) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    return (rp && rp->valid() && rp->HasPose()) ? rp->GetActor() : nullptr;
}

void OnPlayerCharacterPost(void* context, void* source, void* result);

void UpdateArm() {
    const bool want = !g_activator.empty();
    if (!g_hookInstalled || want == g_armed) return;
    if (UH::SetArmed(g_gpcFn, &OnPlayerCharacterPost, want)) g_armed = want;
}

// The activator's body for a Kerfus a client turned on, or null: the host's own, a leaver's whose
// puppet is gone, or a dead Kerfus's record.
void* ActivatorPawn(void* kerfus) {
    auto it = g_activator.find(kerfus);
    if (it == g_activator.end()) return nullptr;
    if (!R::IsLiveByIndex(kerfus, it->second.idx)) {
        g_activator.erase(it);
        UpdateArm();
        return nullptr;
    }
    return PawnOf(it->second.slot);
}

// targetActor's player-0 branch, answered with the activator's body. The Blueprint tests the Kerfus's
// own fields, never its out parameter: `moveTo` not valid (no task) and the member `possessLoc` zero
// (not possessed); it then returns GetPlayerPawn(0) and copies the member into the out `possessLoc`
// (bp_cfg targetActor, blocks @0 and @353). The task branch and the possessed one run.
sg::Verdict OnTargetPre(const sg::Call& c) {
    if (!OnHost() || !c.result || !c.object) return sg::Verdict::Run;
    void* pawn = ActivatorPawn(c.object);
    if (!pawn || UK::ValidTask(c.object)) return sg::Verdict::Run;
    ue_wrap::FVector possessed{};
    if (!UK::ReadPossessLoc(c.object, possessed)) return sg::Verdict::Run;
    if (possessed.X != 0.f || possessed.Y != 0.f || possessed.Z != 0.f) return sg::Verdict::Run;
    const int32_t outOff = UK::TargetActorPossessOut(c.function);
    auto* out = outOff >= 0 ? reinterpret_cast<ue_wrap::FVector*>(sg::OutParamPtr(c, outOff)) : nullptr;
    if (!out) return sg::Verdict::Run;
    *out = possessed;
    *static_cast<void**>(c.result) = pawn;
    ++g_targetsAnswered;
    return sg::Verdict::Cancel;
}

// GetPlayerCharacter, armed while a Kerfus has a client activator. The source is the object whose
// bytecode made the call; only a Kerfus a client turned on is answered, and it asks for player 0 alone.
void OnPlayerCharacterPost(void* /*context*/, void* source, void* /*result*/) {
    if (!source || !GT::IsGameThread()) return;
    void* pawn = ActivatorPawn(source);
    if (!pawn) return;
    if (void* r = UH::CurrentResult()) {
        *static_cast<void**>(r) = pawn;
        ++g_readsAnswered;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // A client never records a presser, so neither seam is ever asked there: both install on a host.
    if (!OnHost()) return;
    if (!g_targetWatched)
        g_targetWatched = sg::WatchClassName(UK::kClassName, L"targetActor", kTagTarget, &OnTargetPre, nullptr);
    if (g_hookInstalled || g_hookRefused) return;
    if (!g_gpcFn) {
        void* gs = R::FindClass(P::name::GameplayStaticsClass);
        if (!gs) return;  // the engine class resolves at boot; asked again next call
        g_gpcFn = R::FindFunction(gs, L"GetPlayerCharacter");
        if (!g_gpcFn) {
            g_hookRefused = true;
            UE_LOGE("kerfus_follow: GameplayStatics has no GetPlayerCharacter in this build -- a Kerfus a client "
                    "turns on judges its approach by the host's distance for the rest of this process");
            return;
        }
    }
    g_hookInstalled = UH::InstallPostHook(g_gpcFn, &OnPlayerCharacterPost, /*armed=*/false);
    if (!g_hookInstalled) {
        g_hookRefused = true;
        UE_LOGE("kerfus_follow: the GetPlayerCharacter seam did not install (ufunction_hook said why above) -- a "
                "Kerfus a client turns on judges its approach by the host's distance for the rest of this process");
    }
}

void Tick() {
    if (!g_saidLive && g_targetWatched) {
        // This lane drives its own watch to live, and says so once: until then a Kerfus a client turned on
        // would path to the host while its reads judged the client.
        sg::ResolvePendingNames();
        if (sg::ClassNameWatchLive(UK::kClassName, L"targetActor", kTagTarget)) {
            g_saidLive = true;
            UE_LOGI("kerfus_follow: the Kerfus's path goal is watched -- a Kerfus a client turns on follows it");
        }
    }
    for (auto it = g_activator.begin(); it != g_activator.end();) {
        void* k = it->first;
        bool on = false;
        if (!R::IsLiveByIndex(k, it->second.idx) || !UK::ReadActive(k, on)) {
            it = g_activator.erase(it);
            continue;
        }
        if (on && !it->second.on) {
            it->second.on = true;
            UE_LOGI("kerfus_follow: Kerfus %p is on by slot %u's press -- it follows that player", k,
                    static_cast<unsigned>(it->second.slot));
        } else if (!on) {
            if (it->second.on)
                UE_LOGI("kerfus_follow: Kerfus %p is off -- slot %u's press no longer leads it (so far %llu "
                        "targetActor and %llu GetPlayerCharacter reads answered)", k,
                        static_cast<unsigned>(it->second.slot), g_targetsAnswered, g_readsAnswered);
            it = g_activator.erase(it);
            continue;
        }
        ++it;
    }
    UpdateArm();
}

void OnToggle(void* kerfus, uint8_t slot) {
    if (!kerfus || !OnHost()) return;
    // A press on a Kerfus that is on turns it off: the tick sees that edge and drops its record.
    bool on = false;
    if (UK::ReadActive(kerfus, on) && on) return;
    if (slot == 0) {
        if (g_activator.erase(kerfus)) UpdateArm();  // the host's own press turns it on: it follows the host
        return;
    }
    g_activator[kerfus] = Rec{R::InternalIndexOf(kerfus), slot};
    UpdateArm();
}

void OnPeerLeft(uint8_t slot) {
    bool any = false;
    for (auto it = g_activator.begin(); it != g_activator.end();) {
        if (it->second.slot == slot) { it = g_activator.erase(it); any = true; }
        else ++it;
    }
    if (any) {
        UpdateArm();
        UE_LOGI("kerfus_follow: slot %u left -- its Kerfuses follow the host again", static_cast<unsigned>(slot));
    }
}

void OnDisconnect() {
    if (g_targetsAnswered || g_readsAnswered)
        UE_LOGI("kerfus_follow: session end -- %llu targetActor and %llu GetPlayerCharacter reads answered with an "
                "activator", g_targetsAnswered, g_readsAnswered);
    g_activator.clear();
    UpdateArm();
    g_targetsAnswered = g_readsAnswered = 0;
}

}  // namespace coop::kerfus_follow
