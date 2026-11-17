// harness/autotest/container_gaps_shared.h -- what the container gaps drill's two files share: its case actors, its
// waits and its ending. Private to autotest_container_gaps.cpp and autotest_container_gaps_bin.cpp.

#pragma once

#include "harness/autotest/peer_signal.h"

#include "coop/session/rig_verdict.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <string>

namespace harness::autotest::container_gaps {

inline constexpr const char* kTag = "container-gaps";
inline constexpr const wchar_t* kDiscPath = L"/Game/objects/prop_floppyDisc_R.prop_floppyDisc_R_C";
inline constexpr int kStepSec = 120;           // a bound: past it the drill says INVALID, never a verdict

// The case actors, by the key the host names (game thread only).
struct Actors {
    void* box = nullptr;
    void* pack = nullptr;
    void* bin = nullptr;
    void* brk1 = nullptr;     // break: the host's
    void* brk2 = nullptr;     // break: the client's
    void* inner = nullptr;    // nested: the host's inner basket before it is filed
};
inline Actors g;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

// ---- the ending ----

inline bool Aborted() {
    std::string s;
    return harness::autotest::peer_signal::Peek("abort", s);
}

// A peer that said FAIL or INVALID tells the other to stop quietly.
inline void Stop() { harness::autotest::peer_signal::Mark("abort"); }

template <class... A>
void Fail(const char* fmt, A... a) {
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Fail, fmt, a...);
    Stop();
}

// Polls an event every 100 ms; a bound passed says INVALID (PS) and stops the other peer. False when it did not come or
// the other peer stopped.
template <class Pred>
bool Wait(const char* what, int seconds, Pred pred) {
    const bool ok = harness::autotest::peer_signal::WaitFor(what, seconds, [&] { return pred() || Aborted(); });
    if (!ok) { Stop(); return false; }
    return !Aborted();
}

inline bool WaitMark(const char* name, std::string* text = nullptr) {
    std::string t;
    const bool ok = harness::autotest::peer_signal::AwaitText(name, t, kStepSec);
    if (text) *text = t;
    if (!ok) { Stop(); return false; }
    return !Aborted();
}

// ---- helpers ----

inline bool Live(void* a) { return a && ue_wrap::reflection::IsLive(a); }

inline void* SpawnPath(const wchar_t* path, const ue_wrap::FVector& at) {
    void* cls = ue_wrap::asset_load::LoadObjectByPath(path);
    return cls ? ue_wrap::engine::SpawnActor(cls, at) : nullptr;
}

// Whether the actor held in `*p` is gone.
inline bool Dead(void** p) {
    bool live = true;
    OnGameThread([&] { live = Live(*p); });
    return !live;
}

// ---- the bin case (autotest_container_gaps_bin.cpp) ----

// HOST: a disc in the garbage container, tipped out, the container destroyed. False when a wait failed.
bool HostBinLeg();

// CLIENT: the container's copy read against each state the host reaches; its disc free at the end. False when a case was
// judged wrong or a wait failed.
bool ClientBinLeg();

}  // namespace harness::autotest::container_gaps
