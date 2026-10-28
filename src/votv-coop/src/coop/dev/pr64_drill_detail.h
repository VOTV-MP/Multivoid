// coop/dev/pr64_drill_detail.h -- SRC-TREE PRIVATE: what the pr64 drill's files share (coop/dev/pr64_drill.h). The arm,
// the chat words the two peers say to each other, the bounds, the verdict lines and the aim. Game thread.

#pragma once

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/types.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace coop::net { class Session; }

namespace coop::dev::pr64_drill::detail {

enum class Arm : uint8_t { Off, Coin, Lid };
Arm ArmOf();
const char* ArmName();
const char* Role();
uint64_t NowMs();

// The lines a peer says. A bare line is a flag; a prefix is followed by the values (the last live row of the prefix).
inline constexpr const char* kHostFail     = "pr64 host fail";
inline constexpr const char* kClientFail   = "pr64 client fail";
inline constexpr const char* kCoinReady    = "pr64 coin-ready ";     // <eid> <x> <y> <z>
inline constexpr const char* kCoinPressed  = "pr64 coin-pressed ";   // B0 <client balance>
inline constexpr const char* kCoinHost     = "pr64 coin-host ";      // <before> <after> <alive>
inline constexpr const char* kCoinFinal    = "pr64 coin-final ";     // <client balance> <alive>
inline constexpr const char* kLidReady     = "pr64 lid-ready ";      // <box key>
inline constexpr const char* kLidFound     = "pr64 lid-found ";      // <opened as the client first reads it>
inline constexpr const char* kLidHost      = "pr64 lid-host ";       // <opened> <drive0>, the host's box after both changes
inline constexpr const char* kLidClient    = "pr64 lid-client ";     // <opened> <drive0> <ms until it read the host's pair>

inline constexpr unsigned kUseKey = 'E';      // the game's use key

// ---- bounds: failure bounds of the instrument, each stamped when its step begins ----

inline constexpr float kStandCm = 150.f;       // the client stops this near what it presses
inline constexpr int kWalkDeadlineS = 300;     // the director's bound on one walk
inline constexpr uint64_t kObserveMs = 3000;   // how long the client reads after a press
inline constexpr uint64_t kPuppetBoundMs = 60000, kEidBoundMs = 5000, kFinalBoundMs = 30000, kHearBoundMs = 180000;
inline constexpr uint64_t kMirrorBoundMs = 15000, kAimBoundMs = 3000, kHostWordBoundMs = 30000;
inline constexpr uint64_t kPressBoundMs = (static_cast<uint64_t>(kWalkDeadlineS) + 90) * 1000;
inline constexpr uint64_t kKeyBoundMs = 5000, kFindBoundMs = 20000, kFoundBoundMs = 40000;   // the lid arm's waits
inline constexpr uint64_t kMutateBoundMs = 5000;     // one held E: from the key down until the change reads on the host
inline constexpr uint64_t kFixtureBoundMs = 10000;   // the box's arrays up, the drive named, the drive in its slot
inline constexpr uint64_t kNearBoundMs = 2000;       // the far box reads the host's pair within this of the host's word

// ---- the lid arm's recipe ----

inline constexpr float kBoxAheadCm = 120.f, kBoxRiseCm = 40.f;   // the box starts this far in front of the host, above the floor
inline constexpr float kDriveRiseCm = 60.f;    // the fixture drive spawns this far above the box

// ---- the coin arm's recipe ----

inline constexpr int32_t kPrice = 3;           // sell's Index; under 5 it mints one coin carrying it as points
inline constexpr float kRingCm = 450.f;        // the fixture's floor point is on a ring this far about the host
inline constexpr int kRingPoints = 8;
inline constexpr float kFromUpCm = 100.f, kDownCm = 400.f, kMinColumnCm = 60.f;   // the floor trace under a ring point
inline constexpr float kRiseCm = 60.f, kPropSideCm = 80.f;   // the gun and the prop start this far above the floor
inline constexpr float kRouteMaxCm = 3000.f;   // the longest route from the client's body to the point
inline constexpr float kNearGunCm = 600.f;     // a coin within this of the gun is counted as the gun's
inline constexpr float kRestCm = 2.f;          // a coin that stays within this of one place for kRestTicks ticks is at rest
inline constexpr int kRestTicks = 10;
inline constexpr uint64_t kRestBoundMs = 10000;   // the coin never settled: the instrument's failure bound

void Say(const std::string& line);
bool Heard(const char* line);
bool Rest(const char* prefix, std::string* rest);
inline bool Expired(uint64_t sinceMs, uint64_t boundMs) { return NowMs() - sinceMs > boundMs; }

// `n` numbers read from `text`, space separated, after its first `skip` characters; false when fewer read.
inline bool Numbers(const std::string& text, size_t skip, double* out, int n) {
    const char* p = text.c_str() + (skip < text.size() ? skip : text.size());
    for (int i = 0; i < n; ++i) {
        char* e = nullptr;
        out[i] = std::strtod(p, &e);
        if (e == p) return false;
        p = e;
    }
    return true;
}

// `<role> <arm> PASS` then `<role> DONE`; or `<role> <arm> FAIL: <why>`, the peer told, then `<role> DONE`.
// A non-null `note` follows PASS on the same line.
void Pass(const char* note = nullptr);
void Fail(const char* fmt, ...);

// The camera turned at `target`, as a player's mouse does; false without a controller.
bool Aim(void* player, const ue_wrap::FVector& target);

// The client's world is up and its join has gone idle.
bool ClientReady();

// The coin arm's fixtures on `at` and the coins its sell minted (pr64_drill_coin.cpp). False, with the failure printed,
// when no coin exists.
bool SpawnAndMint(const ue_wrap::FVector& at, std::vector<ue_wrap::CachedObjRef>& coins);

// The client's count of local credits (pr64_drill_coin_credit.cpp): an observe-only watch of lib_C::addPoints. Start registers
// (false: the gate refused); State is 1 live, 0 pending, -1 dead; Arm zeroes the count and starts counting calls that are not
// ours; Stop retires the watch and clears the count for a new session.
bool CreditWatchStart();
int CreditWatchState();
void CreditWatchArm();
int CreditWatchCalls();
void CreditWatchStop();

// The lid arm (pr64_drill_lid.cpp): the host's and the client's ticks, and the steps put back for a new session.
void LidHostTick(coop::net::Session* s);
void LidClientTick();
void LidReset();

}  // namespace coop::dev::pr64_drill::detail
