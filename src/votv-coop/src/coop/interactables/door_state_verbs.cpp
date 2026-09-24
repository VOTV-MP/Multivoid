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
bool g_watchInstalled[kWatchCount] = {};
bool g_announcedLive = false;

uint64_t g_refused = 0, g_edges = 0;
// A key's first refusal is said; the rest are counted. One door refuses at most one autoclose per
// open, so the set stays the size of the doors this client's own world tried to move.
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
    if (call.fromOurCode) return sg::Verdict::Run;  // our apply, or a replay's own door call
    if (!call.object || !D::IsDoor(call.object)) return sg::Verdict::Run;
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
    for (int i = 0; i < kWatchCount; ++i)
        if (!g_watchInstalled[i])
            g_watchInstalled[i] = sg::WatchName(kWatches[i].name, kWatches[i].tag, &OnStatePre, &OnStatePost);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWatches();
}

void Tick() {
    sg::ResolvePendingNames();
    if (g_announcedLive) return;
    for (int i = 0; i < kWatchCount; ++i)
        if (!g_watchInstalled[i] || !sg::NameWatchLive(kWatches[i].name, kWatches[i].tag)) return;
    g_announcedLive = true;
    UE_LOGI("[DOOR-STATE] the door state gates are live: doorOpen and doorClose");
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
