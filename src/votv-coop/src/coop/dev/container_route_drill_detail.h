// coop/dev/container_route_drill_detail.h -- what the census drill's three files share. A private header of
// coop/dev/container_route_drill*.cpp (src tree, not include/).

#pragma once

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::container_route_drill::detail {

inline constexpr const wchar_t* kPackPath   = L"/Game/objects/prop_backpack.prop_backpack_C";
inline constexpr const wchar_t* kPackClass  = L"prop_backpack_C";
inline constexpr const wchar_t* kDriveClass = L"prop_drive_C";
inline constexpr int      kItems        = 2;      // the records the fixture holds
inline constexpr float    kAheadCm      = 100.f;  // the fixture before the puppet
inline constexpr float    kSideAheadCm  = 60.f;   // the R4 drive, to one side of the backpack
inline constexpr float    kSideCm       = 80.f;
inline constexpr float    kDropCm       = 30.f;
inline constexpr float    kNearCm       = 400.f;  // CLIENT: the fixtures' mirrors are this near its player
inline constexpr uint64_t kCheckEveryMs = 250;
inline constexpr uint64_t kSeamBoundMs  = 30000;  // the census's six watches to settle
inline constexpr uint64_t kRegenWindowMs = 1000;  // a write's applying to the open screen's rebuild
inline constexpr uint64_t kHandBoundMs  = 10000;  // CLIENT: a hold settling in the hand, the screen closing

// The lines the peers say to each other (drill_chat).
inline constexpr const char* kLineRoutesDone    = "ci-census client routes-done";
inline constexpr const char* kLineR4Done        = "ci-census client r4-done";
inline constexpr const char* kLineBeforeReady   = "ci-census client before-ready";
inline constexpr const char* kLineBeforeWritten = "ci-census host before-written";
inline constexpr const char* kLineRefilled      = "ci-census host refilled";
inline constexpr const char* kLineRefillRefused = "ci-census host refilled ok=0";   // the refill the backpack refused
inline constexpr const char* kLineBeforeFailed  = "ci-census client before-failed";   // the client ended the take route
inline constexpr const char* kPrefixR1Done      = "ci-census client r1-done ";   // + "sent=<slices sent so far>"
inline constexpr const char* kPrefixR4Start     = "ci-census client r4-start ";  // + "eid=<the held prop, or 0>"
inline constexpr const char* kPrefixR4Thrown    = "ci-census host r4-thrown ";   // + "eid=<the host's drive for the throw>"
inline constexpr const char* kPrefixR4Hold     = "ci-census client r4-hold ";   // + "eid=<the held prop, or 0>"
inline constexpr const char* kLineR4Armed       = "ci-census host r4-armed";
inline constexpr const char* kPrefixClientDone  = "ci-census client done ";   // + "rb=<0|1> ic=<0|1>"
inline constexpr const char* kPrefixHostDone    = "ci-census host done ";     // + "ih=<0|1>"

// Each side's steps (container_route_drill_host.cpp, container_route_drill_client.cpp).
void HostTick(coop::net::Session* s);
void ClientTick(coop::net::Session* s);
void HostReset();
void ClientReset();

// The element id of an actor, or kInvalidId.
uint32_t EidOf(void* actor);

// The record count of the world container `eid`, or -1 when it does not read.
int32_t RecordCount(uint32_t eid);

// Says the lane's hash of each record of the container, in order, then the whole slice (role "host" / "client").
void LogHashes(const char* role, uint32_t eid, const char* what);

// Says the encoded size of each record the container actor holds, and keeps the largest.
void LogSizes(const char* role, const char* what, void* container);
void LogLargest(const char* role);

// Prints a line of the drill (every one carries the tag the rig reads).
void Abandon(const char* role, const char* why);

// A route step that failed or whose bound passed: a FAIL line naming the row's arm and the phase. The step ends; the
// drill goes on to the next route (only a fixture failure Abandons).
void RouteFail(const char* phase);

// A trace stream's rate: the first 20 lines, then every 10th, at most 200 in all. Take() once per read.
class TraceRate {
public:
    bool Take() {
        ++seen_;
        const bool line = (seen_ <= 20 || seen_ % 10 == 0) && emitted_ < 200;
        if (line) ++emitted_;
        return line;
    }
    void Reset() { seen_ = emitted_ = 0; }
private:
    int seen_ = 0, emitted_ = 0;
};

// The screen the gamemode holds for a container view (ui_playerInventory_C), its container slot widgets, and a
// no-argument call of a function on an object. Null / 0 / false when they do not resolve.
void* Screen();
int SlotCount(void* screen);
void* SlotAt(void* screen, int i);
bool CallNoArg(void* obj, const wchar_t* fn);

// The screen's first container slot as its widget holds it (its prop name and amount), or empty when it does not read.
std::string FirstSlotText();

// The first-slot part of a regen line (before and after the rebuild), or empty when the slot did not read after.
std::string SlotChange(const std::string& before, const std::string& after);

// The census's six watches, asked each tick until Go: Wait while they settle, Dead (the drill ended, ABANDONED: seam
// <name> is dead) when one settled without a hook or the bound passed. `role` is "host" / "client".
enum class SeamGate : uint8_t { Wait, Go, Dead };
SeamGate SeamsGate(const char* role);
void ResetSeamsGate();

// True when the open screen rebuilt since the baseline: a gen_player body ran after `gen0`, or the first slot widget is
// no longer `slot0`.
bool ScreenRebuilt(uint64_t gen0, void* slot0);

// The integer after "key=" in a chat line's text, or false when the key is absent.
bool IntAfter(const std::string& text, const char* key, int& out);

// Live, non-default instances of prop_drive_C on this peer.
int LiveDrives();

}  // namespace coop::dev::container_route_drill::detail
