// harness/autotest/autotest_driveslot_detail.h -- what the drive-slot test's two files share:
// the host half (autotest_driveslot_host.cpp), the client half (autotest_driveslot.cpp). The arm,
// the seeded signal, the game-thread waits, the reads of the world's drives and the verdict builder.

#pragma once

#include "harness/autotest.h"  // IsClientRole

#include <windows.h>

#include "coop/config/config.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"                // IsSlotWorldReady: the host's half of the shared origin
#include "coop/player/hand_item.h"           // the host's puppet's hand mirror, and the hand axis a census skips
#include "coop/player/players_registry.h"
#include "coop/props/prop_act_host.h"        // what the host answered and committed
#include "coop/props/prop_act_request.h"     // the client's request being open, refused
#include "coop/session/join_progress.h"      // the client's join being over, not just announced
#include "coop/session/net_pump.h"           // HasAnnouncedWorldReady: the client's half
#include "harness/session_runtime.h"         // Session()
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/inventory_item.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/desk/signal_dynamic.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_physics.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace harness::autotest::driveslot_detail {

namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace GT  = ue_wrap::game_thread;
namespace DC  = ue_wrap::drive_chain;
namespace SD  = ue_wrap::signal_dynamic;
namespace P   = ue_wrap::profile;
namespace PAR = coop::prop_act_request;
namespace PAH = coop::prop_act_host;
namespace HI  = coop::hand_item;

enum class Arm { Grab, Hold, Pocket };

inline Arm ArmFromEnv() {
    const std::string v = coop::config::ReadEnv("VOTVCOOP_RUN_DRIVESLOT");
    return v == "hold" ? Arm::Hold : v == "pocket" ? Arm::Pocket : Arm::Grab;
}

inline const char* ArmName(Arm a) { return a == Arm::Hold ? "hold" : a == Arm::Pocket ? "pocket" : "grab"; }

constexpr float kCarryAsideCm = 150.f;  // how far the client carries the drive (grab)
constexpr float kOffPortCm    = 75.f;   // the host's copy has left the port by at least this (grab)
constexpr float kFollowCm     = 100.f;  // and then moved this much further as the carry goes on (the carry is 150 aside)
constexpr float kNearPortCm   = 100.f;  // a drive this close to the port that was not here before is a stray
constexpr DWORD kReplayWindowMs = 10000;  // the host's connect replay to a fresh joiner
constexpr wchar_t kDriveLeaf[] = L"prop_drive_C";

// The signal the host seeds into data_0, one per arm so two runs' logs differ and no peer mistakes a save's drive for
// it: the name and id are strings the saved data carries, the size a number the row lane compares.
inline std::wstring SignalName(Arm a) { return std::wstring(L"DRVSLOT-") + (a == Arm::Hold ? L"HOLD" : a == Arm::Pocket ? L"POCKET" : L"GRAB"); }

inline SD::Row SeedRow(Arm a) {
    SD::Row row;
    row.name = SignalName(a);
    row.id = SignalName(a) + L"-ID";
    row.object = L"selftest";
    row.signal = L"selftest";
    row.size = 10.f;
    row.decoded = 10.f;
    row.level = 2;
    row.date = 638000000000000000LL;   // non-zero: no receiver stamps Now
    row.frequency = 2;
    row.quality = 3;
    row.hasData = true;
    return row;
}

inline bool RowIsSignal(const SD::Row& r, Arm a) {
    return r.name == SignalName(a) && r.id == SignalName(a) + L"-ID" && r.size == 10.f;
}

// Run a game-thread closure and block until it stores into `done` (1 ok, 2 fail).
template <class Fn>
int RunGT(Fn&& body) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, body]() mutable { body(*done); });
    while (done->load() == 0) ::Sleep(5);
    return done->load();
}

// Poll `pred` on the game thread every quarter second until it holds or `ms` pass.
template <class Pred>
bool WaitFor(int ms, Pred pred) {
    for (int t = 0; t < ms; t += 250) {
        if (RunGT([pred](std::atomic<int>& d) { d.store(pred() ? 1 : 2); }) == 1) return true;
        ::Sleep(250);
    }
    return false;
}

// The shared origin both peers wait on, the broom drill's: the client's world coming up, which the
// client announces and the host learns as that slot going world-ready. A run reaches it when it
// reaches it -- a fixed wait here measured nothing when the load ran long and burned the rest of
// the window when it did not.
inline bool WaitForPeerWorld(const char* who) {
    const bool isClient = IsClientRole();
    UE_LOGI("driveslot: %hs -- waiting for the client's world", who);
    for (int i = 0; i < 1800; ++i) {
        // The client's own join being OVER, not merely announced: its phase returns to Idle when
        // the host's snapshot has been applied and the cover is down. The host has no predicate for
        // the other end of its own replay, so it waits out that window -- it is the one thing here
        // that is not a state either peer publishes. Acting inside it loses: the replay's seed
        // carries the slot as it was BEFORE the drill touched it, and it lands after the drill's
        // own edges and puts the old drive back.
        const bool ready = isClient ? (coop::net_pump::HasAnnouncedWorldReady() &&
                                       coop::join_progress::CurrentPhase() ==
                                           coop::join_progress::Phase::Idle)
                                    : harness::session_runtime::Session().IsSlotWorldReady(1);
        if (ready) {
            if (!isClient) ::Sleep(kReplayWindowMs);
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("driveslot: %hs never saw the client's world -- aborting", who);
    return false;
}

inline uint32_t EidOf(void* actor) {
    return static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(actor));
}

inline float Distance(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Call a Blueprint verb on `obj` with the local player as its `player` argument.
inline bool CallWithPlayer(void* obj, const wchar_t* verb, void* player) {
    void* fn = R::FindDispatchFunction(R::ClassOf(obj), verb, nullptr);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && ue_wrap::Call(obj, f);
}

// A verdict being built: each need that does not hold is named, and the verdict is PASS when none failed.
struct Checks {
    std::string failed;
    bool Need(bool ok, const char* what) {
        if (!ok) {
            if (!failed.empty()) failed += "; ";
            failed += what;
        }
        return ok;
    }
    bool Pass() const { return failed.empty(); }
};

// ---- reads shared by both roles -----------------------------------------------------------------------------------

inline std::string NarrowAscii(const std::wstring& w) {
    std::string s;
    for (wchar_t ch : w) s += ch < 128 ? static_cast<char>(ch) : '?';
    return s;
}

inline std::vector<void*> LiveDrives() {
    std::vector<void*> out;
    void* const cls = DC::DriveClass();
    if (!cls) return out;
    ue_wrap::object_index::ForEachInstance(
        cls,
        [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        },
        &out);
    return out;
}

inline bool IsHandAxis(void* actor, void* const* axis, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (axis[i] == actor) return true;
    return false;
}

struct Census {
    int signalWorld = 0;      // world drives carrying the signal (the hand axis is not the world)
    int newNearPort = 0;      // drives that were not here before the action, within kNearPortCm of the port, any row
    std::string detail;
};

// The world's drives, as a stray would be found: the hand axis (this peer's two hands and every puppet's mirrors) is
// player expression and left out; a drive seated in the slot is the slot's.
inline Census TakeCensus(Arm arm, void* slot, const std::vector<void*>& pre, const ue_wrap::FVector& port) {
    Census c;
    void* axis[HI::kHandAxisCap];
    const size_t axisN = HI::CollectHandAxisActors(axis, HI::kHandAxisCap);
    void* const seated = slot ? DC::SlotDrive(slot) : nullptr;
    char buf[200];
    for (void* d : LiveDrives()) {
        if (IsHandAxis(d, axis, axisN) || d == seated) continue;
        SD::Row row;
        const bool read = DC::ReadDriveRow(d, row);
        const bool sig = read && RowIsSignal(row, arm);
        ue_wrap::FVector at{};
        const bool atRead = E::TryGetActorLocation(d, at);
        const float dist = atRead ? Distance(at, port) : -1.f;
        bool wasHere = false;
        for (void* p : pre) wasHere = wasHere || p == d;
        if (sig) ++c.signalWorld;
        const bool nearNew = !wasHere && atRead && dist <= kNearPortCm;
        if (nearNew) ++c.newNearPort;
        if (sig || nearNew) {
            std::snprintf(buf, sizeof(buf), " [eid=%u sig=%d wasHere=%d %.0f cm from the port]", EidOf(d), sig ? 1 : 0,
                          wasHere ? 1 : 0, dist);
            c.detail += buf;
        }
    }
    return c;
}

// The hand's drive: the main hand's actor when it is a live drive.
inline void* MainHandDrive(void* player) {
    E::MainPlayerGrabState g{};
    if (!player || !E::ReadMainPlayerGrabState(player, g) || !g.mainHand || !R::IsLive(g.mainHand)) return nullptr;
    return DC::IsDriveClass(R::ClassOf(g.mainHand)) ? g.mainHand : nullptr;
}

// Does the saved stream hold `text`, as an FString stores it: ANSI when it is plain ASCII, UTF-16 otherwise.
inline bool StreamHas(const std::vector<uint8_t>& bytes, const std::wstring& text) {
    if (text.empty() || bytes.size() < text.size()) return false;
    std::string ansi;
    ansi = NarrowAscii(text);
    for (size_t i = 0; i + ansi.size() <= bytes.size(); ++i)
        if (std::memcmp(bytes.data() + i, ansi.data(), ansi.size()) == 0) return true;
    const size_t wbytes = text.size() * sizeof(wchar_t);
    for (size_t i = 0; i + wbytes <= bytes.size(); ++i)
        if (std::memcmp(bytes.data() + i, text.data(), wbytes) == 0) return true;
    return false;
}

// How many of `items` are a drive whose saved data holds the signal.
inline int DriveRecordsWithSignal(const std::vector<ue_wrap::inventory_item::Item>& items, Arm arm) {
    int n = 0;
    for (const auto& it : items)
        if (ue_wrap::inventory_item::ClassLeaf(it) == kDriveLeaf && StreamHas(it.actorData, SignalName(arm))) ++n;
    return n;
}

inline int DriveRecords(const std::vector<ue_wrap::inventory_item::Item>& items) {
    int n = 0;
    for (const auto& it : items)
        if (ue_wrap::inventory_item::ClassLeaf(it) == kDriveLeaf) ++n;
    return n;
}

struct Subject {
    void* slot = nullptr;
    void* drive = nullptr;
    int32_t driveIdx = 0;
    uint32_t eid = 0;
    ue_wrap::FVector port{};
    std::vector<void*> pre;   // the drives that stood in this world before the action
};

// The host's half: births the drive, seats it in the play slot and judges its own copy (autotest_driveslot_host.cpp).
void RunHost(Arm arm);

// The client's half: takes the drive out of the play slot as a player does and judges its own copy (autotest_driveslot.cpp).
void RunClient(Arm arm);

}  // namespace harness::autotest::driveslot_detail
