// coop/world/evil_ending.cpp -- see coop/world/evil_ending.h.
//
// theEvil_C's graph, cooked (0.10.0; the row below ties these offsets to that bytecode): its tick counts `time` down while
// `active`, then clears `active`, runs checkforevil and, with a copy of the signal found (or `force`), goes on to
//   1168  lib.progressAchievement("ending_evil")      the host's alone
//    674  gamemode.save_main.save()                   the host's alone
//    732  postprocess.bEnabled = true                 <- a client's stand-in enters here
//    765  SpawnSound2D(whitenoise), three delays, the sound's deactivate
//    228  gameover_C minted with the game's own text (a localized constant of the graph), finished at 635.
// The stand-in is a theEvil_C this peer spawns itself with `active` at its class default, false: its tick does nothing, so it
// never counts down, checks no drive and ends nothing by itself. A prop a client spawns outside the intent door stays local.

#include "coop/world/evil_ending.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

namespace coop::evil_ending {
namespace {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
using Clock = std::chrono::steady_clock;

constexpr const wchar_t* kClass = L"theEvil_C";

UE_FACT_FN(kRowGraph, "evil_ending", L"theEvil_C", L"ExecuteUbergraph_theEvil", Declared);
constexpr int32_t kEntryEnding = 732;   // the graph's `postprocess.bEnabled = true`, past the achievement and the save

constexpr long long kHoldMs = 30000;    // how long a cue waits for the class on a client
constexpr long long kHostPollMs = 100;  // the host's look at its Evil
constexpr long long kClientPollMs = 1000;  // a client's look for a copy that counts down
constexpr long long kEndingMaxMs = 90000;  // an ending whose menu trip never came is ended after this

std::atomic<coop::net::Session*> g_session{nullptr};

// HOST: the Evil whose ending was sent (an Evil ends once; `active` stays false after).
int32_t g_hostSentIdx = -1;
Clock::time_point g_lastPoll{};

// Either role: the Evil whose ending runs on this peer (the host's own actor, or a client's stand-in), until its menu trip
// is refused.
void*   g_ending = nullptr;
int32_t g_endingIdx = -1;
bool    g_endingIsStandIn = false;
Clock::time_point g_endingSince{};

// CLIENT: a cue waiting for the class.
bool g_hold = false;
Clock::time_point g_holdSince{};

bool FlagOf(void* obj, const wchar_t* name, int32_t& off, uint8_t& mask) {
    off = -1;
    mask = 0;
    return obj && R::FindBoolProperty(R::ClassOf(obj), name, off, mask) && off >= 0;
}

bool ReadFlag(void* obj, const wchar_t* name) {
    int32_t off;
    uint8_t mask;
    return FlagOf(obj, name, off, mask) && (*(static_cast<const uint8_t*>(obj) + off) & mask) != 0;
}

void WriteFlag(void* obj, const wchar_t* name, bool on) {
    int32_t off;
    uint8_t mask;
    if (!FlagOf(obj, name, off, mask)) return;
    uint8_t* b = static_cast<uint8_t*>(obj) + off;
    *b = on ? static_cast<uint8_t>(*b | mask) : static_cast<uint8_t>(*b & ~mask);
}

// The Evil's post process component, live.
void* PostProcessOf(void* evil) {
    const int32_t off = evil ? R::FindPropertyOffset(R::ClassOf(evil), L"postprocess") : -1;
    if (off < 0) return nullptr;
    void* c = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(evil) + off);
    return (c && R::IsLive(c)) ? c : nullptr;
}

bool EndingStarted(void* evil) { return ReadFlag(PostProcessOf(evil), L"bEnabled"); }

void TrackEnding(void* evil, bool standIn) {
    g_ending = evil;
    g_endingIdx = R::InternalIndexOf(evil);
    g_endingIsStandIn = standIn;
    g_endingSince = Clock::now();
}

// The moment is over on this peer: the screen effect goes, and a stand-in with it. The host's own Evil stays, inert, as
// the game leaves it in the save it writes (while one stands, the computer mints no second).
void FinishEnding(const char* why) {
    if (!g_ending) return;
    const bool live = R::IsLiveByIndex(g_ending, g_endingIdx);
    if (live) {
        WriteFlag(PostProcessOf(g_ending), L"bEnabled", false);
        if (g_endingIsStandIn) E::DestroyActor(g_ending);
    }
    UE_LOGI("evil_ending: the ending is over on this peer (%s) -- the screen effect off%s", why,
            !live ? ", its actor already gone" : g_endingIsStandIn ? ", the stand-in destroyed" : ", the host's Evil kept, inert");
    g_ending = nullptr;
    g_endingIdx = -1;
    g_endingIsStandIn = false;
}

struct Collect {
    std::vector<void*> found;
};

// HOST: an Evil of this world whose post process came on is at its ending; one cue, once per Evil.
void HostPoll(coop::net::Session* s) {
    void* cls = ue_wrap::object_index::ClassByName(kClass);
    if (!cls) return;   // never minted in this process
    Collect c;
    ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        if (obj) static_cast<Collect*>(ctx)->found.push_back(obj);
    }, &c);
    void* world = ue_wrap::world_identity::CurrentWorld();
    for (void* evil : c.found) {
        if (!R::IsLive(evil) || ue_wrap::world_identity::WorldOf(evil) != world) continue;
        const int32_t idx = R::InternalIndexOf(evil);
        if (idx == g_hostSentIdx || !EndingStarted(evil)) continue;
        g_hostSentIdx = idx;
        TrackEnding(evil, false);
        coop::net::EventCuePayload p{};
        p.cueId = kCueEnding;
        const bool sent = s->connected() && s->SendReliable(coop::net::ReliableKind::EventCue, &p, sizeof(p));
        UE_LOGI("evil_ending: the Evil's ending started on the host -- %s", sent ? "cue sent" : "no peer to send the cue to");
    }
}

// CLIENT: the host's Evil reaches this peer as a prop copy. A copy born with `active` up would count down by its own tick
// and judge this peer's own drives and save, with its own achievement, save and game over: the host's Evil alone decides,
// so a copy is kept inert.
void ClientParkCopies() {
    void* cls = ue_wrap::object_index::ClassByName(kClass);
    if (!cls) return;
    Collect c;
    ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        if (obj) static_cast<Collect*>(ctx)->found.push_back(obj);
    }, &c);
    for (void* evil : c.found) {
        if (evil == g_ending || !R::IsLive(evil) || !ReadFlag(evil, L"active")) continue;
        WriteFlag(evil, L"active", false);
        UE_LOGI("evil_ending: this peer's copy of the host's Evil was counting down -- stopped (the host's decides)");
    }
}

// CLIENT: the ending on a stand-in. False while the class is not loaded.
bool PlayHere() {
    void* cls = R::FindClass(kClass);
    if (!cls) return false;
    const auto graph = ue_wrap::facts::ResolveOn(kRowGraph, cls);
    if (!graph) {
        UE_LOGE("evil_ending: theEvil_C's graph did not resolve -- the host's ending is not shown here");
        return true;
    }
    if (g_ending) FinishEnding("a second ending began");
    void* standIn = E::SpawnActor(cls, ue_wrap::FVector{0.f, 0.f, 0.f});
    if (!standIn) {
        UE_LOGE("evil_ending: the stand-in did not spawn -- the host's ending is not shown here");
        return true;
    }
    WriteFlag(standIn, L"active", false);   // the class default, written all the same: the stand-in must never count down
    TrackEnding(standIn, true);
    ue_wrap::ParamFrame f(graph.Fn());
    const bool ran = f.valid() && f.Set<int32_t>(L"EntryPoint", kEntryEnding) && ue_wrap::Call(standIn, f);
    UE_LOGI("evil_ending: the host's Evil ended the run -- this peer's stand-in plays the ending: %d (screen effect %d)",
            ran ? 1 : 0, EndingStarted(standIn) ? 1 : 0);
    if (!ran) FinishEnding("the graph did not run");
    return true;
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const auto now = Clock::now();
    if (g_ending) {
        if (!R::IsLiveByIndex(g_ending, g_endingIdx)) FinishEnding("its actor ended");
        else if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_endingSince).count() > kEndingMaxMs)
            FinishEnding("no menu trip came");
    }
    if (s->role() == coop::net::Role::Host) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastPoll).count() < kHostPollMs) return;
        g_lastPoll = now;
        HostPoll(s);
        return;
    }
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastPoll).count() >= kClientPollMs) {
        g_lastPoll = now;
        ClientParkCopies();
    }
    if (!g_hold) return;
    if (PlayHere()) {
        g_hold = false;
    } else if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_holdSince).count() > kHoldMs) {
        UE_LOGW("evil_ending: theEvil_C did not load in %lld s -- the host's ending is not shown here", kHoldMs / 1000);
        g_hold = false;
    }
}

bool OnCue(const coop::net::EventCuePayload& p) {
    if (p.cueId != kCueEnding) return false;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return true;
    g_hold = true;
    g_holdSince = Clock::now();
    Tick();
    return true;
}

void OnRunEndRefused() { FinishEnding("its menu trip was refused and the player revived"); }

void OnDisconnect() {
    FinishEnding("the session ended");
    g_hold = false;
    g_hostSentIdx = -1;
}

}  // namespace coop::evil_ending
