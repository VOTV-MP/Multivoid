// harness/session_second_player.cpp -- the static second player the solo visual aid spawns (see harness/session_runtime.h).

#include "harness/session_runtime.h"

#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/session/shutdown.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <memory>

namespace harness::session_runtime {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

}  // namespace

// Spawn the second player the moment the local mainPlayer_C exists: a ~100 ms poll on the game
// thread (engine state is read only there). The flag is a shared_ptr so it outlives the loop if
// a posted check is still queued.
void SpawnSecondPlayerWhenReady() {
    UE_LOGI("play: waiting for STORY gameplay, spawn 2nd player the instant it's ready");
    for (int i = 0; i < 1200; ++i) {  // ~120 s safety cap
        if (coop::shutdown::IsShuttingDown()) {
            UE_LOGI("play: SpawnSecondPlayerWhenReady aborting -- shutdown signaled");
            return;
        }
        auto state = std::make_shared<std::atomic<int>>(0);  // 0 pending,1 not-ready,2 ok,3 failed
        GT::Post([state, i] {
            if (coop::puppet_drive::Puppet(1).valid()) { state->store(2); return; }
            void* local = coop::players::Registry::Get().Local();
            const bool diag = (i % 20 == 0);  // ~every 2 s
            if (!local) {
                if (diag) {
                    void* w = R::FindObjectByClass(P::name::WorldClass);
                    UE_LOGI("play[wait %d]: no mainPlayer_C; world=%ls objs=%d", i,
                            w ? R::ToString(R::NameOf(w)).c_str() : L"(none)", R::NumObjects());
                }
                state->store(1); return;
            }
            // A mainPlayer_C also exists at the menu (the preLoad world), at the origin; a puppet
            // spawned against it lands in the menu world, which the level load then destroys. Gate
            // on a non-origin location.
            ue_wrap::FVector p{};
            const bool pRead = ue_wrap::engine::TryGetActorLocation(local, p);   // unread proves no gameplay either
            if (!pRead || std::abs(p.X) + std::abs(p.Y) + std::abs(p.Z) < 100.f) {
                if (diag) UE_LOGI("play[wait %d]: mainPlayer_C @ORIGIN (%.0f,%.0f,%.0f)%s -- waiting for real gameplay",
                                  i, p.X, p.Y, p.Z, pRead ? "" : " (unread)");
                state->store(1);  // still the origin menu player; wait for gameplay
                return;
            }
            UE_LOGI("play: mainPlayer_C ready @ (%.0f,%.0f,%.0f) -- spawning puppet", p.X, p.Y, p.Z);
            state->store(coop::puppet_drive::Puppet(1).Spawn() ? 2 : 3);
        });
        // Shutdown-aware like its two siblings: a posted task the game thread never drains holds
        // this thread for as long as that lasts.
        while (state->load() == 0 && !coop::shutdown::IsShuttingDown()) ::Sleep(5);
        const int s = state->load();
        if (s == 2) {
            UE_LOGI("play: 2nd player spawned the moment the local player was ready");
            return;
        }
        if (s == 3) UE_LOGW("play: spawn attempt failed; retrying");
        // No sandbox fallback: coop targets story mode through the save system.
        ::Sleep(100);  // local player not in world yet -> poll again
    }
    UE_LOGW("play: gave up waiting for local mainPlayer_C");
}

}  // namespace harness::session_runtime
