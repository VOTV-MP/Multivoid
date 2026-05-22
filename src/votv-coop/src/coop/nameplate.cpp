#include "coop/nameplate.h"

#include "coop/remote_player.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace coop::nameplate {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

namespace {

// VOTV does not run the stock HUD canvas (ReceiveDrawHUD never fires), so a
// screen-space nametag is impossible here. Instead each player gets a 3D
// ATextRenderActor floating above its head, billboarded to face the local player.
struct Entry {
    RemotePlayer* player = nullptr;
    void* textActor = nullptr;
};
std::vector<Entry> g_entries;
void* g_viewer = nullptr;  // cached local player (avoid a GUObjectArray walk every Update)

constexpr float kHeadTextZ = 195.f;  // just above the head (actor origin = feet)

}  // namespace

void Register(RemotePlayer* player) {
    if (!player) return;
    if (std::any_of(g_entries.begin(), g_entries.end(),
                    [&](const Entry& e) { return e.player == player; }))
        return;

    ue_wrap::FVector at = player->GetLocation();
    at.Z += kHeadTextZ;
    // NOTE: opaque TextRender (UnlitText is Masked -> no partial alpha). The
    // translucent UWidgetComponent path (engine::SpawnNameplateWidget) builds the
    // widget but its world-space quad won't render yet -- WIP; likely pivoting to a
    // screen-projected viewport widget (what VOTV's toasts use).
    const ue_wrap::FColor color{255, 255, 255, 255};
    void* label = E::SpawnTextActor(at, player->GetNickname().c_str(), 11.f, color);
    g_entries.push_back({player, label});
    UE_LOGI("nameplate: label '%ls' actor=%p for player %p (now %zu)",
            player->GetNickname().c_str(), label, player, g_entries.size());
    Update();  // place + face immediately
}

void Unregister(RemotePlayer* player) {
    for (auto it = g_entries.begin(); it != g_entries.end(); ++it) {
        if (it->player == player) {
            // Only destroy if still live: now that DestroyActor actually fires
            // K2_DestroyActor, calling it on a label the engine already killed on a
            // level change would be a use-after-free.
            if (it->textActor && R::IsLive(it->textActor)) E::DestroyActor(it->textActor);
            g_entries.erase(it);
            return;
        }
    }
}

void Update() {
    if (g_entries.empty()) return;
    // Billboard target = the local player. Cache the pointer (FindObjectByClass
    // walks the whole GUObjectArray, too costly per frame), but drop the cache if
    // the actor was destroyed (level change) so we never read freed memory.
    if (g_viewer && !R::IsLive(g_viewer)) g_viewer = nullptr;
    if (!g_viewer) g_viewer = R::FindObjectByClass(P::name::MainPlayerClass);
    const ue_wrap::FVector viewer =
        g_viewer ? E::GetActorLocation(g_viewer) : ue_wrap::FVector{};

    for (const auto& e : g_entries) {
        if (!e.player || !e.player->valid() || !e.textActor) continue;
        if (!R::IsLive(e.textActor)) continue;  // label destroyed (level change)
        ue_wrap::FVector at = e.player->GetLocation();
        at.Z += kHeadTextZ;
        E::SetActorLocation(e.textActor, at);

        // Face the viewer. TextRenderComponent draws text readable from the -X
        // side, so point the actor's +X TOWARD the viewer (yaw of label->viewer)
        // -- otherwise the glyphs render mirrored (seen from behind).
        const float dx = viewer.X - at.X;
        const float dy = viewer.Y - at.Y;
        const float yaw = std::atan2(dy, dx) * 57.29578f;
        E::SetActorRotation(e.textActor, ue_wrap::FRotator{0.f, yaw, 0.f});
    }
}

}  // namespace coop::nameplate
