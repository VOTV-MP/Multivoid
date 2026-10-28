// coop/dev/coffee_drill_detail.h -- what the coffee drill's files share: its bounds, the cast (the vending machine,
// the maker and the socket it is plugged into, as this peer holds them), the maker's flags, the props the host
// spawns, and the chat edges. Impl-private: nothing outside coop/dev includes it. Game thread.

#pragma once

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::net { class Session; }

namespace coop::dev::coffee_drill::detail {

// ---- bounds: failure bounds of the instrument, never a wait for a milestone ----

// An arm not closed this long after its bound starts: the peer that waited prints FAIL. The bound starts at the arm's
// opening edge, or, in an arm whose acting peer walks, when that peer has arrived (a walk is its own phase).
inline constexpr uint64_t kArmFailMs = 60000;
// A walk (the cast's arrival, an arm's own walk), which the director's own deadline bounds (+ a margin).
inline constexpr int      kWalkDeadlineS = 300;
inline constexpr uint64_t kStageFailMs = (static_cast<uint64_t>(kWalkDeadlineS) + 30) * 1000;

// ---- the recipe ----

inline constexpr int     kArms = 5;
inline constexpr int32_t kVendingCost = 15;     // the vending machine's debit (lib_C::addPoints(-15))
inline constexpr int32_t kGrantPoints = 100;    // a fresh host holds 10; the drill grants this before arm 1
inline constexpr float   kVendReachCm = 250.f;  // the client's walk ends this near the vending machine's origin
inline constexpr float   kCupReachCm = 150.f;   // ... near the cup it grabs
inline constexpr float   kMakerReachCm = 90.f;  // ... near the maker it carries a cup to
inline constexpr float   kAboveCm = 60.f;       // a drop over the maker starts this far above its origin
inline constexpr float   kCupLiftCm = 30.f;     // the host's spare cups start this far above the stand point
inline constexpr float   kOverMakerCm = 45.f;   // a carried cup counts as over the maker within this, flat
inline constexpr float   kFeetCm = 150.f;       // a carry has arrived only with the player this near the maker's height
inline constexpr float   kReleaseMinCm = 10.f;  // a cup is let go only this far above the maker's origin ...
inline constexpr float   kReleaseMaxCm = 120.f; // ... and not farther

// ---- the cast (coffee_drill_cast.cpp) ----

struct Cast {
    ue_wrap::CachedObjRef vending, maker, sock;
    std::wstring vendKey, makerKey, sockKey;
};
Cast& TheCast();
void ResetCast();

void* Vending();   // the live vending machine or null
void* Maker();     // the live maker or null
void* Sock();      // the live socket or null

// The maker's four flags as the game holds them (each a bit of one shared byte, so each is read under its own
// mask); -1 where a flag did not read.
struct Flags {
    int cup = -1, coffee = -1, ready = -1, power = -1;
};
bool ReadFlags(void* maker, Flags& out);
// True when hasCup, hasCoffee and coffeeReady read as given (hasPower is judged apart).
bool FlagsAre(const Flags& f, int cup, int coffee, int ready);
std::string FlagsText(const Flags& f);   // "hasCup=1 hasCoffee=0 coffeeReady=0 hasPower=1"
// This peer's balance, or -1 when it does not read.
int32_t Points();

// Every live key of the exact class prop_food_coffeeCup_C (what the vending machine and the maker make).
std::vector<std::wstring> CupKeys();
// The first live cup key not in `before`; empty when there is none.
std::wstring NewCupKey(const std::vector<std::wstring>& before);
// A prop of `cls` at `at`; with `nameRow` its `name` field is written before it finishes (the maker takes a prop
// named "cup" as a cup). Null when it did not spawn.
void* SpawnProp(const wchar_t* cls, const ue_wrap::FVector& at, const wchar_t* nameRow);

// True when the key can ride a lobby chat line: printable ASCII, no space, not "None".
bool KeyRides(const std::wstring& k);
// Whether a prop with the key lives here now (prop_key_index, which catches the object index up on a miss).
void* LiveByKey(const std::wstring& key);
// Whether the drive stream still holds the actor to the host's pose (parked or driven); a cup free of it
// simulates on this peer.
bool HeldByDrive(void* actor);

// The live vending machine nearest `player`, or null when the world holds none.
void* NearestVending(void* player);

// Where this peer's copy of the maker is: one line now (`when` names the moment), and every 5 s from the first
// tick that holds a maker one line with its place, its speed, how often its hit handler ran in those 5 s and the
// classes of what touched it (coffee_drill_maker.cpp).
void LogMakerAt(const char* when);
void MakerProbeTick();
void MakerProbeReset();

// The vending machine's floor stand: its place projected onto the nav mesh; false when none lies near it.
bool VendingStand(void* player, void* vending, ue_wrap::FVector& place, ue_wrap::FVector& stand);

// The host's pick of a socket: a free level socket that is powered, with a nominal stand point on the nav mesh, a
// client stand and a host stand beside it, and a clear line from a knee-high point there to the maker's spot.
// Only a socket on the vending machine's floor qualifies: both stands within kFloorCm of height of `vendStand`,
// and a route from there to the client stand that stays within kFloorCm of that height and is at most kRouteCm
// long. The shortest such route wins.
inline constexpr float kFloorCm = 120.f;
inline constexpr float kRouteCm = 2500.f;
struct Pick {
    void* sock = nullptr;
    ue_wrap::FVector at{}, fwd{}, stand{}, makerAt{}, clientNav{}, hostNav{};
    float routeCm = 0.f, maxDzCm = 0.f;
    int free = 0, powered = 0, placed = 0, blocked = 0, offFloor = 0, noRoute = 0;
};
bool ChooseSocket(void* player, const ue_wrap::FVector& vendStand, Pick& out);

// ---- the chat edges (coffee_drill.cpp) ----

// A line both peers watch for, latched on sight (a live row retires after a few more lines). Heard is true once the
// row was seen live; Rest is the text after a prefix, from the newest live row that carried it.
bool Heard(const std::string& line);
bool Rest(const std::string& prefix, std::string* rest);
void Say(const std::string& line);
std::string ArmLine(int n);            // host: arm n opens
std::string AtLine(int n);             // client: it has arrived for arm n, its bound starts
std::string HostSeenLine(int n);       // host: its copies show arm n
std::string ClientSeenLine(int n);     // client: the same
std::string CupPrefix(int n);          // host: "coffee host cup <n> " + the key it names, for arms 1, 3, 4, 5
inline constexpr const char* kFillPrefix = "coffee host fill 2 ";

// A failure line, once; `arm` names the phase. Prints this peer's flags and balance.
void FailLine(const char* peer, const char* arm, const char* why);

uint64_t NowMs();

// ---- the roles (coffee_drill_host.cpp, coffee_drill_client.cpp) ----

void HostTick(coop::net::Session* s);
// The session's end on the host. True when the host is past its five arms: the cast and the phase stay for the
// rejoin (the host's session lives on); otherwise the drill starts over.
bool HostOnDisconnect();
void ClientTick(coop::net::Session* s);
void ClientReset();

}  // namespace coop::dev::coffee_drill::detail
