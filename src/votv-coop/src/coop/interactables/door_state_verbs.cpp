// coop/interactables/door_state_verbs.cpp -- see coop/interactables/door_state_verbs.h.

#include "coop/interactables/door_state_verbs.h"

#include "coop/interactables/interactable_channel.h"  // ShadowProbe, the dev probe's flag
#include "coop/interactables/interactable_sync.h"  // the door lane's key, and the host's edge
#include "coop/net/session.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld: a client's own world load runs natively

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/door.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace coop::door_state_verbs {
namespace {

namespace D  = ue_wrap::door;
namespace sg = ue_wrap::script_gate;

// doorOpen and doorClose are door_C's alone; a timeline named `move` gives other classes a
// move__FinishedFunc too, so each callback gates on the door class, which also skips a door the
// resolve has not reached yet. The swing's end is watched after its body only: a client's copy ends
// the swings the lane applies to it.
struct StateWatch { const wchar_t* name; int tag; const char* what; bool refuseOnClient; };
constexpr StateWatch kWatches[] = {
    { L"doorOpen",           0x44534f50 /*'DSOP'*/, "doorOpen",        true },
    { L"doorClose",          0x4453434c /*'DSCL'*/, "doorClose",       true },
    { L"move__FinishedFunc", 0x4453454e /*'DSEN'*/, "the swing's end", false },
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));

std::atomic<coop::net::Session*> g_session{nullptr};
// Each watch registers once: a refusal (a full gate table) is final, and said once. The set settles
// once every watch is live, or once no name is left to resolve and the rest are dead for good.
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kWatchCount] = {};
bool g_settled = false;

uint64_t g_refused = 0, g_edges = 0;
uint64_t g_loadNative = 0;  // a client's calls run natively as its own world's load (IsInAnnouncedWorld)
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

// The dev probe's watches (channel_shadow_probe), HOST, after the body: the door's bodies that write what the
// lane's intent reads (isMoving, dir) outside doorOpen, doorClose and the swing's end -- the jam shake's end
// (isMoving false, dir 1), the jam's first event (dir 0) and a hit (dir 1, at rest only: it returns on a moving
// door) -- and the sensor's begin and end, the passage of whoever walks through, each said with the swing it
// leaves, so a SHADOW MISS can be matched to what ran before it. The component events' names carry K2Node_,
// as the bytecode listing has them; the pseudo-C++ drops it.
constexpr StateWatch kProbeWatches[] = {
    { L"addDamage",                   0x44534144 /*'DSAD'*/, "a hit (addDamage)",     false },
    { L"openJammed__FinishedFunc",    0x44534a45 /*'DSJE'*/, "the jam shake's end",   false },
    { L"openJammed__jam1__EventFunc", 0x44534a31 /*'DSJ1'*/, "the jam's first event", false },
    { L"BndEvt__door_sensor_K2Node_ComponentBoundEvent_2_ComponentBeginOverlapSignature__DelegateSignature",
      0x44535342 /*'DSSB'*/, "the sensor's begin overlap", false },
    { L"BndEvt__door_sensor_K2Node_ComponentBoundEvent_3_ComponentEndOverlapSignature__DelegateSignature",
      0x44535345 /*'DSSE'*/, "the sensor's end overlap", false },
};
bool g_probeRegistered = false;

// The probe's read of each swing's finish window, HOST, before the move timeline's update body: its final tick
// has stopped it (the update runs after the stop), so a door whose isMoving still holds there is in the window.
// The lane's intent read there is held until the swing's end function has run, and then set against where the
// door settled, the open flag that function writes: a read the window got wrong disagrees with the settle.
constexpr int kTagFinishWindow = 0x4453464e;  // 'DSFN'
constexpr int kTagFinishSettle = 0x44535354;  // 'DSST'
std::unordered_map<void*, bool> g_windowIntent;  // a door in its window -> the intent read there
uint64_t g_windowReads = 0, g_windowDisagreed = 0;

sg::Verdict OnMoveUpdatePre(const sg::Call& call) {
    if (!ConnectedAs(coop::net::Role::Host)) return sg::Verdict::Run;
    if (!call.object || !D::IsDoor(call.object)) return sg::Verdict::Run;
    bool inWindow = false, toOpen = false, intent = false;
    if (!D::TryReadFinishWindow(call.object, inWindow, toOpen) || !inWindow) return sg::Verdict::Run;
    if (!D::TryReadOpenIntent(call.object, intent)) return sg::Verdict::Run;
    g_windowIntent[call.object] = intent;
    return sg::Verdict::Run;
}

void OnMoveFinishedPost(const sg::Call& call) {
    if (!ConnectedAs(coop::net::Role::Host)) return;
    const auto it = call.object ? g_windowIntent.find(call.object) : g_windowIntent.end();
    if (it == g_windowIntent.end()) return;
    const bool intent = it->second;
    g_windowIntent.erase(it);
    bool settled = false;
    if (!D::IsDoor(call.object) || !D::TryReadOpen(call.object, settled)) return;
    ++g_windowReads;
    if (intent != settled) ++g_windowDisagreed;
    char desc[160] = "";
    D::DescribeSwing(call.object, desc, sizeof desc);
    UE_LOGI("[DOOR-STATE] probe: the swing's finish window on key='%ls' read %s; the door settled %s (%llu of %llu "
            "window reads disagree with the settle) -- %s", coop::interactable_sync::DoorKey(call.object).c_str(),
            intent ? "OPEN" : "SHUT", settled ? "OPEN" : "SHUT", static_cast<unsigned long long>(g_windowDisagreed),
            static_cast<unsigned long long>(g_windowReads), desc);
}

void OnProbePost(const sg::Call& call) {
    if (!ConnectedAs(coop::net::Role::Host)) return;
    if (!call.object || !D::IsDoor(call.object)) return;
    char desc[160] = "";
    D::DescribeSwing(call.object, desc, sizeof desc);
    for (const StateWatch& w : kProbeWatches)
        if (w.tag == call.tag)
            UE_LOGI("[DOOR-STATE] probe: %s on key='%ls' -- %s", w.what,
                    coop::interactable_sync::DoorKey(call.object).c_str(), desc);
}

// CLIENT, before the body.
sg::Verdict OnStatePre(const sg::Call& call) {
    if (!ConnectedAs(coop::net::Role::Client)) return sg::Verdict::Run;
    if (!call.object || !D::IsDoor(call.object)) return sg::Verdict::Run;
    // A call on an object of a world this client has not announced ready is that world's load, the
    // host's save: it runs natively, and the host's own states for what the lane owns follow the
    // announce.
    if (!coop::net_pump::IsInAnnouncedWorld(call.object)) {
        ++g_loadNative;
        return sg::Verdict::Run;
    }
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
    if (!g_probeRegistered && coop::interactable_sync::ShadowProbe()) {
        g_probeRegistered = true;
        for (const StateWatch& w : kProbeWatches)
            if (!sg::WatchName(w.name, w.tag, nullptr, &OnProbePost))
                UE_LOGW("[DOOR-STATE] the probe's watch on '%ls' was refused", w.name);
        if (!sg::WatchName(L"move__UpdateFunc", kTagFinishWindow, &OnMoveUpdatePre, nullptr) ||
            !sg::WatchName(L"move__FinishedFunc", kTagFinishSettle, nullptr, &OnMoveFinishedPost))
            UE_LOGW("[DOOR-STATE] the probe's watches on the swing's finish window were refused");
    }
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        if (sg::WatchName(kWatches[i].name, kWatches[i].tag, kWatches[i].refuseOnClient ? &OnStatePre : nullptr,
                          &OnStatePost)) {
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
        UE_LOGI("[DOOR-STATE] the door state gates are live: doorOpen, doorClose and the swing's end");
    } else if (pending == 0 && sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[DOOR-STATE] %d of %d door state gates are dead (refused, or resolved into a full table) -- the "
                "door lane cannot see those verbs", kWatchCount - live, kWatchCount);
    }
}

void OnDisconnect() {
    if (g_refused || g_edges || g_loadNative)
        UE_LOGI("[DOOR-STATE] session end -- a client refused %llu local state verb(s) on %zu door(s) and "
                "ran %llu of its own world's load; the host saw %llu",
                static_cast<unsigned long long>(g_refused), g_saidRefused.size(),
                static_cast<unsigned long long>(g_loadNative), static_cast<unsigned long long>(g_edges));
    g_refused = g_edges = g_loadNative = 0;
    g_saidRefused.clear();
    if (g_windowReads)
        UE_LOGI("[DOOR-STATE] session end -- the probe read %llu swing(s) in their finish window, %llu of them "
                "disagreeing with the settle", static_cast<unsigned long long>(g_windowReads),
                static_cast<unsigned long long>(g_windowDisagreed));
    g_windowReads = g_windowDisagreed = 0;
    g_windowIntent.clear();
}

}  // namespace coop::door_state_verbs
