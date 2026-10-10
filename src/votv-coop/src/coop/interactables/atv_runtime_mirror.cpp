// coop/interactables/atv_runtime_mirror.cpp -- a client's mirror of a runtime ATV (declared in
// coop/interactables/atv_sync.h). The host announces an ATV spawned mid-session under a synthetic key; this
// client fresh-spawns a native ATV for it, physics on so it is grabbable, and destroys it when the host's goes.
// A spawn landing while this world is not ready waits in the park (atv_spawn_park.h). The map stays
// atv_sync.cpp's: this half asks it to adopt or drop a row (atv_sync_internal.h).

#include "coop/interactables/atv_sync.h"
#include "atv_spawn_park.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/atv_sync_internal.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/session/world_load_episode.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/world_identity.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace coop::atv_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace A = ue_wrap::atv;
using ue_wrap::FVector;
using ue_wrap::FRotator;
using coop::net::StringFromWireKey;

std::wstring WireClassNameToString(const coop::net::WireClassName& in) {
    std::wstring s;
    const uint8_t n = in.len <= sizeof(in.data) ? in.len : static_cast<uint8_t>(sizeof(in.data));
    s.reserve(n);
    for (uint8_t i = 0; i < n; ++i) s.push_back(static_cast<wchar_t>(static_cast<unsigned char>(in.data[i])));
    return s;
}

}  // namespace

void OnAtvSpawn(const coop::net::AtvSpawnPayload& payload, uint8_t /*senderPeerSlot*/) {
    auto* s = LaneSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return;
    std::wstring synthKey = StringFromWireKey(payload.synthKey);
    if (synthKey.empty()) { UE_LOGW("atv: OnAtvSpawn empty synthKey -- dropping"); return; }
    // The payload is checked before any wait: a malformed spawn is dropped, only a sound one is deferred.
    if (!std::isfinite(payload.x) || !std::isfinite(payload.y) || !std::isfinite(payload.z) ||
        !std::isfinite(payload.pitch) || !std::isfinite(payload.yaw) || !std::isfinite(payload.roll)) {
        UE_LOGW("atv: OnAtvSpawn non-finite pose synthKey='%ls' -- dropping", synthKey.c_str());
        spawn_park::Discard(synthKey);  // a malformed spawn is final
        return;
    }
    std::wstring className = WireClassNameToString(payload.className);
    if (className.empty()) {
        UE_LOGW("atv: OnAtvSpawn empty className synthKey='%ls' -- dropping", synthKey.c_str());
        spawn_park::Discard(synthKey);
        return;
    }
    if (ue_wrap::world_identity::CurrentWorldKind() != ue_wrap::world_identity::WorldKind::Gameplay ||
        !IndexCurrent() || !coop::world_load_episode::HasQuiesced()) {
        spawn_park::Park(synthKey, payload, "the receiving world is not ready");
        return;
    }
    // The ATV spawn drill's park arm: the first spawn is parked as if the world were not ready, so the park's
    // hand-back is what spawns it.
    static const bool s_parkFirst = coop::config::ResolveFlag(coop::config_registry::rows::atv_park_first_spawn);
    static bool s_parkedFirst = false;
    if (s_parkFirst && !s_parkedFirst) {
        s_parkedFirst = true;
        spawn_park::Park(synthKey, payload, "the drill parks the first spawn");
        return;
    }
    if (!A::EnsureResolved()) { spawn_park::Park(synthKey, payload, "the ATV wrapper is not resolved"); return; }
    if (const AtvEntry* known = FindEntry(synthKey)) {
        if (R::IsLiveByIndex(known->actor, known->idx)) { spawn_park::Discard(synthKey); return; }
        EraseEntry(synthKey);   // a dead row is not "already spawned"
    }
    const FVector loc{ payload.x, payload.y, payload.z };
    const FRotator rot{ payload.pitch, payload.yaw, payload.roll };
    void* spawned = A::SpawnMirror(className, loc, rot);  // physics LEFT ON -- a native idle grabbable ATV
    if (!spawned) {
        // In a world that reads ready, a refused spawn is the class or the engine saying no: final, not retried.
        UE_LOGW("atv: OnAtvSpawn SpawnMirror failed synthKey='%ls' class='%ls' -- not retried", synthKey.c_str(),
                className.c_str());
        spawn_park::Discard(synthKey);
        return;
    }
    spawn_park::Discard(synthKey);
    AdoptClientMirror(synthKey, spawned);
    UE_LOGI("atv: spawned runtime-ATV mirror synthKey='%ls' class='%ls' actor=%p loc=(%.0f, %.0f, %.0f)",
            synthKey.c_str(), className.c_str(), spawned, loc.X, loc.Y, loc.Z);
}

uint32_t ParkedEver() { return spawn_park::ParkedEver(); }
size_t ParkPending() { return spawn_park::Pending(); }

void OnAtvDestroy(const coop::net::AtvDestroyPayload& payload, uint8_t /*senderPeerSlot*/) {
    auto* s = LaneSession();
    if (!s || s->role() == coop::net::Role::Host) return;  // client-only
    std::wstring synthKey = StringFromWireKey(payload.synthKey);
    if (synthKey.empty()) return;
    spawn_park::Discard(synthKey);  // a destroy ends a spawn still waiting
    if (!IndexCurrent()) return;  // the pass prunes a dead-world entry itself
    const AtvEntry* row = FindEntry(synthKey);
    if (!row) return;
    const bool destroyed = row->isClientSpawnedMirror && R::IsLiveByIndex(row->actor, row->idx) &&
                           A::DestroyMirror(row->actor);  // our own fresh spawn
    EraseEntry(synthKey);
    UE_LOGI("atv: runtime-ATV mirror synthKey='%ls' %s", synthKey.c_str(),
            destroyed ? "destroyed" : "dropped from the lane, not destroyed here");
}

}  // namespace coop::atv_sync
