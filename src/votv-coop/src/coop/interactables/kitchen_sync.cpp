// coop/interactables/kitchen_sync.cpp -- see coop/interactables/kitchen_sync.h.

#include "coop/interactables/kitchen_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/remote_prop.h"  // KeyToWString

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/devices/kitchen.h"
#include "ue_wrap/engine/world_identity.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::kitchen_sync {
namespace {

namespace K = ue_wrap::kitchen;
using Clock = std::chrono::steady_clock;

constexpr auto kPoll = std::chrono::milliseconds(250);
constexpr float kNaturalEndSeconds = 1.f;  // a run that read more than this at the last poll did not end by its timer

std::atomic<coop::net::Session*> g_session{nullptr};
Clock::time_point g_nextPoll{};
std::unordered_map<std::wstring, K::State> g_base;  // by Key

uint32_t g_sent = 0, g_applied = 0, g_unresolved = 0;

void Collect(std::vector<void*>& out) {
    struct Walk {
        void* world;
        std::vector<void*>* out;
    } walk{ue_wrap::world_identity::CurrentWorld(), &out};
    if (!walk.world) return;
    for (const wchar_t* name : {K::kMicrowaveClass, K::kMiniOvenClass, K::kGrillClass}) {
        void* const cls = ue_wrap::object_index::ClassByName(name);
        if (!cls) continue;
        ue_wrap::object_index::ForEachInstance(
            cls,
            [](void* ctx, void* obj, int32_t) {
                auto* w = static_cast<Walk*>(ctx);
                if (ue_wrap::world_identity::WorldOf(obj) == w->world) w->out->push_back(obj);
            },
            &walk);
    }
}

std::wstring KeyOf(void* actor) {
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
    return key == L"None" ? std::wstring{} : key;
}

bool Stopped(const K::State& was, const K::State& now) {
    return now.kind == K::Kind::Microwave && was.active && !now.active && was.remaining > kNaturalEndSeconds;
}

coop::net::KitchenStatePayload Encode(const std::wstring& key, const K::State& state, bool stopped) {
    coop::net::KitchenStatePayload p{};
    for (size_t i = 0; i < key.size() && i < sizeof(p.key.data); ++i)
        p.key.data[p.key.len++] = static_cast<char>(key[i]);  // save keys are ASCII
    p.kind = static_cast<uint8_t>(state.kind);
    p.active = state.active ? 1 : 0;
    p.lid = state.lid ? 1 : 0;
    p.microwave = static_cast<uint8_t>((state.doorOpen ? coop::net::kitchen_flags::kDoorOpen : 0) |
                                       (stopped ? coop::net::kitchen_flags::kStopped : 0));
    p.mode = state.mode;
    p.remaining = state.remaining;
    return p;
}

// Whether this change is one the other peers must hear from this peer.
bool Crosses(const K::State& was, const K::State& now, bool host) {
    if (now.kind == K::Kind::Grill) {
        if (now.lid != was.lid) return true;
        if (host) return now.mode != was.mode || now.active != was.active;
        return now.mode > was.mode || (now.active && !was.active);  // its burn is the host's to report
    }
    if (now.mode != was.mode) return true;
    if (now.kind != K::Kind::Microwave) return now.active != was.active;
    if (now.doorOpen != was.doorOpen || Stopped(was, now)) return true;
    // A run that ended and restarted between two polls never reads as off: its timer jumping back up is the start.
    return now.active && (!was.active || now.remaining > was.remaining + 1.f);
}

void Poll(coop::net::Session& s) {
    std::vector<void*> machines;
    Collect(machines);
    for (void* actor : machines) {
        K::State now;
        if (!K::Read(actor, now)) continue;
        const std::wstring key = KeyOf(actor);
        if (key.empty()) continue;
        auto it = g_base.find(key);
        if (it == g_base.end()) {  // first sight: its state came with its birth or the save
            g_base[key] = now;
            continue;
        }
        const K::State was = it->second;
        it->second = now;
        if (!Crosses(was, now, s.role() == coop::net::Role::Host)) continue;
        const coop::net::KitchenStatePayload p = Encode(key, now, Stopped(was, now));
        const bool sent = s.role() == coop::net::Role::Host
                              ? s.SendReliable(coop::net::ReliableKind::KitchenState, &p, sizeof(p))
                              : s.SendReliableToSlot(0, coop::net::ReliableKind::KitchenState, &p, sizeof(p));
        if (sent) ++g_sent;
        UE_LOGI("kitchen_sync: '%ls' changed here, mode=%d active=%d -- KitchenState %hs (#%u)", key.c_str(), now.mode,
                now.active ? 1 : 0, sent ? "sent" : "had no peer", g_sent);
    }
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected()) return;
    const auto now = Clock::now();
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPoll;
    Poll(*s);
}

void OnState(int senderSlot, const coop::net::KitchenStatePayload& p) {
    const std::wstring key = coop::remote_prop::KeyToWString(p.key);
    void* const actor = coop::prop_key_index::Find(key);
    if (!actor || static_cast<uint8_t>(K::KindOf(actor)) != p.kind) {
        ++g_unresolved;
        UE_LOGI("kitchen_sync: slot %d's state for '%ls' names no such machine here -- dropped", senderSlot, key.c_str());
        return;
    }
    K::ApplyMode(actor, p.mode);
    if (K::KindOf(actor) == K::Kind::Microwave) {
        // Off in a message is only that peer's state, unless it says it stopped: its setting press must not end a run here.
        if (p.microwave & coop::net::kitchen_flags::kStopped) K::ApplyActive(actor, false, 0.f);
        K::ApplyDoor(actor, (p.microwave & coop::net::kitchen_flags::kDoorOpen) != 0);
        if (p.active) K::ApplyActive(actor, true, p.remaining);
    } else {
        K::ApplyActive(actor, p.active != 0, p.remaining);
        K::ApplyLid(actor, p.lid != 0);
    }
    K::State after;
    if (K::Read(actor, after)) g_base[key] = after;
    ++g_applied;
    UE_LOGI("kitchen_sync: slot %d set '%ls' mode=%d active=%d, now active=%d (#%u)", senderSlot, key.c_str(), p.mode,
            p.active, after.active ? 1 : 0, g_applied);
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    std::vector<void*> machines;
    Collect(machines);
    for (void* actor : machines) {
        K::State now;
        if (!K::Read(actor, now) || (now.mode == 0 && !now.active && !now.doorOpen && (now.kind != K::Kind::Grill || now.lid)))
            continue;
        const std::wstring key = KeyOf(actor);
        if (key.empty()) continue;
        const coop::net::KitchenStatePayload p = Encode(key, now, false);
        s->SendReliableToSlot(slot, coop::net::ReliableKind::KitchenState, &p, sizeof(p));
    }
}

void OnDisconnect() {
    if (g_sent || g_applied || g_unresolved)
        UE_LOGI("kitchen_sync: session end -- sent=%u applied=%u unresolved=%u", g_sent, g_applied, g_unresolved);
    g_base.clear();
    g_nextPoll = {};
    g_sent = g_applied = g_unresolved = 0;
}

}  // namespace coop::kitchen_sync
