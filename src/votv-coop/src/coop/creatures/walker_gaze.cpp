// coop/creatures/walker_gaze.cpp -- see coop/creatures/walker_gaze.h.

#include "coop/creatures/walker_gaze.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/walker_state_client.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <cstdint>
#include <cstring>

namespace coop::walker_gaze {
namespace {

namespace AC = ue_wrap::ambient_creature;
namespace PL = coop::players;
namespace R = ue_wrap::reflection;

constexpr size_t kRows = 8;

// A copy that hunts this client: the walker's key (the word names it so), the copy, and the level the host holds from this
// client (-1 none yet, so the first judgment is sent whatever it is).
struct Hunted {
    bool               used = false;
    coop::net::WireKey key{};
    void*              actor = nullptr;
    int32_t            index = -1;
    int32_t            serial = 0;
    int8_t             told = -1;
};
Hunted                g_rows[kRows];
size_t                g_count = 0;
bool                  g_fullSaid = false;
bool                  g_installAsked = false;  // the subscription is asked for once: a full table stays full, said once
coop::net::Session*   g_session = nullptr;
ue_wrap::CachedObjRef g_controller;  // player 0's controller, held across frames

bool SameKey(const coop::net::WireKey& a, const coop::net::WireKey& b) {
    return a.len == b.len && std::memcmp(a.data, b.data, a.len) == 0;
}

Hunted* Find(const coop::net::WireKey& key) {
    for (Hunted& r : g_rows)
        if (r.used && SameKey(r.key, key)) return &r;
    return nullptr;
}

void Drop(Hunted& r) {
    r = Hunted{};
    if (g_count > 0) --g_count;
}

bool StillThere(const Hunted& r) {
    return R::SlotSerial(r.index) == r.serial && R::InternalIndexOf(r.actor) == r.index && R::IsLive(r.actor);
}

bool Send(const coop::net::WireKey& key, bool seen) {
    if (!g_session) return false;
    coop::net::WalkerSeenPayload p{};
    p.key = key;
    p.seen = seen ? 1 : 0;
    if (!g_session->SendReliable(coop::net::ReliableKind::WalkerSeen, &p, sizeof(p))) return false;
    UE_LOGI("walker_gaze: this player %s walker '%ls'", seen ? "sees" : "no longer sees",
            coop::net::StringFromWireKey(key).c_str());
    return true;
}

// The state client's word: a copy was applied (with the slot its walker hunts) or ended. A copy that hunts this client gets a
// row, with nothing told yet; any other target drops the row and sends nothing, the host having cleared its level. A copy's end
// sends nothing either: a copy ends because the host's walker ended, and the host drops that walker's levels at its end.
void OnHunt(const coop::net::WireKey& key, void* copy, uint8_t target, bool ended) {
    Hunted*       r = Find(key);
    const uint8_t me = PL::Registry::Get().LocalPeerId();
    if (ended) {
        if (r) Drop(*r);
        return;
    }
    if (target == 0xFF || me >= PL::kMaxPeers || target != me) {
        if (r) Drop(*r);
        return;
    }
    if (!r) {
        for (Hunted& slot : g_rows)
            if (!slot.used) {
                r = &slot;
                break;
            }
        if (!r) {
            if (!g_fullSaid) {
                g_fullSaid = true;
                UE_LOGW("walker_gaze: %zu copies hunt this player -- another is not judged (said once)", kRows);
            }
            return;
        }
        r->used = true;
        r->key = key;
        ++g_count;
    }
    if (r->actor != copy) {
        r->actor = copy;
        r->index = R::InternalIndexOf(copy);
        r->serial = R::SlotSerial(r->index);
        r->told = -1;
    }
}

}  // namespace

void Install(coop::net::Session& session) {
    UE_ASSERT_GAME_THREAD("walker_gaze::Install");
    if (session.role() != coop::net::Role::Client) return;
    g_session = &session;
    if (g_installAsked) return;
    g_installAsked = true;
    const bool subscribed = coop::walker_state_client::SubscribeHunt(&OnHunt);
    if (!subscribed) UE_LOGE("walker_gaze: the walker state table's subscriber list is full -- this player's gaze is not sent");
}

void Tick() {
    if (g_count == 0) return;
    // The walker's own read: the controller of player 0 and the mesh's bounds, as its tick reads them. The Blueprint's test has
    // no range term, so a copy is judged at any distance (the walker keeps moving toward its target past its acquire range, and
    // a target that backs away while still looking at it must not unfreeze it). The controller is held across frames and read
    // again only when it is gone or the world changed.
    void* controller = g_controller.Get();
    for (Hunted& r : g_rows) {
        if (!r.used) continue;
        if (!StillThere(r)) {
            Drop(r);
            continue;
        }
        if (!controller) {
            controller = AC::WalkerGazeController(r.actor);
            g_controller.Set(controller);
        }
        const bool seen = controller && AC::WalkerInView(r.actor, controller);
        const int8_t level = seen ? 1 : 0;
        // The mannequin drill's red: an unseen level is never sent, so the host keeps the last seen one and the drill's release
        // step must FAIL.
        static const bool s_redNoLevel = coop::config::ResolveEnum(::coop::config_registry::rows::mannequin_drill) == "no_level";
        if (s_redNoLevel && !seen && r.told == 1) continue;
        if (level != r.told && Send(r.key, seen)) r.told = level;
    }
}

void OnDisconnect() {
    for (Hunted& r : g_rows) r = Hunted{};
    g_count = 0;
    g_fullSaid = false;
    g_session = nullptr;
    g_controller.Reset();
}

}  // namespace coop::walker_gaze
