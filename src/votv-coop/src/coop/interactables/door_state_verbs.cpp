// coop/interactables/door_state_verbs.cpp -- see coop/interactables/door_state_verbs.h.

#include "coop/interactables/door_state_verbs.h"

#include "coop/interactables/interactable_sync.h"  // the door lane's key, and the host's edge
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/door.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_set>

namespace coop::door_state_verbs {
namespace {

namespace D  = ue_wrap::door;
namespace sg = ue_wrap::script_gate;

// Only door_C declares either name, so no other class's body reaches these callbacks; each still
// gates on the door class, which also skips a door the resolve has not reached yet.
struct StateWatch { const wchar_t* name; int tag; const char* what; };
constexpr StateWatch kWatches[] = {
    { L"doorOpen",  0x44534f50 /*'DSOP'*/, "doorOpen" },
    { L"doorClose", 0x4453434c /*'DSCL'*/, "doorClose" },
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));

std::atomic<coop::net::Session*> g_session{nullptr};
// Each watch registers once: a refusal (a full gate table) is final, and said once. The set settles
// once every watch is live, or once no name is left to resolve and the rest are dead for good.
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kWatchCount] = {};
bool g_settled = false;

uint64_t g_refused = 0, g_edges = 0;
// A key's first refusal is said; the rest are counted. The set holds each door this client's own
// world tried to move, once.
std::unordered_set<std::wstring> g_saidRefused;

const char* WhatOf(int tag) {
    for (const StateWatch& w : kWatches)
        if (w.tag == tag) return w.what;
    return "?";
}

coop::net::Session* ConnectedAs(coop::net::Role role) {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->connected() && s->role() == role) ? s : nullptr;
}

// CLIENT, before the body.
sg::Verdict OnStatePre(const sg::Call& call) {
    if (!ConnectedAs(coop::net::Role::Client)) return sg::Verdict::Run;
    if (!call.object || !D::IsDoor(call.object)) return sg::Verdict::Run;
    if (coop::interactable_sync::ApplyingDoor(call.object)) return sg::Verdict::Run;  // the lane's own apply
    const std::wstring key = coop::interactable_sync::DoorKey(call.object);
    if (key.empty()) return sg::Verdict::Run;  // no lane owns it: native
    ++g_refused;
    if (g_saidRefused.insert(key).second)
        UE_LOGI("[DOOR-STATE] client refused its own %s on key='%ls' -- the host's copy moves this door "
                "(first refusal on this key; the rest are counted)", WhatOf(call.tag), key.c_str());
    return sg::Verdict::Cancel;
}

// HOST, after the body.
void OnStatePost(const sg::Call& call) {
    if (!ConnectedAs(coop::net::Role::Host)) return;
    if (!call.object || !D::IsDoor(call.object)) return;
    ++g_edges;
    coop::interactable_sync::OnDoorStateVerb(call.object);
}

void RegisterWatches() {
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        if (sg::WatchName(kWatches[i].name, kWatches[i].tag, &OnStatePre, &OnStatePost)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[DOOR-STATE] the script-body gate refused the watch on '%ls' -- for the rest of this process "
                "the door lane cannot see that verb", kWatches[i].name);
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWatches();
}

void Tick() {
    sg::ResolvePendingNames();
    if (g_settled) return;
    int live = 0, pending = 0;
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] == Reg::Pending) ++pending;
        else if (g_reg[i] == Reg::Registered && sg::NameWatchLive(kWatches[i].name, kWatches[i].tag)) ++live;
    }
    if (live == kWatchCount) {
        g_settled = true;
        UE_LOGI("[DOOR-STATE] the door state gates are live: doorOpen and doorClose");
    } else if (pending == 0 && sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[DOOR-STATE] %d of %d door state gates are dead (refused, or resolved into a full table) -- the "
                "door lane cannot see those verbs", kWatchCount - live, kWatchCount);
    }
}

void OnDisconnect() {
    if (g_refused || g_edges)
        UE_LOGI("[DOOR-STATE] session end -- a client refused %llu local state verb(s) on %zu door(s); "
                "the host saw %llu", static_cast<unsigned long long>(g_refused), g_saidRefused.size(),
                static_cast<unsigned long long>(g_edges));
    g_refused = g_edges = 0;
    g_saidRefused.clear();
}

}  // namespace coop::door_state_verbs
