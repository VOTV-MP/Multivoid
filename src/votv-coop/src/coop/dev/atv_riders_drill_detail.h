// coop/dev/atv_riders_drill_detail.h -- private to the ATV riders drill: the verdict, the waits and the record cursor
// (atv_riders_drill.cpp), and the reads and the steps that judge them (atv_riders_drill_steps.cpp, atv_riders_drill_late.cpp). Every read hops to the game
// thread for the engine; every step is called from the drill's own thread and returns false when the run ended (a verdict was
// said here or on another peer) or the step could not be reached.
#pragma once

#include "coop/dev/atv_drive.h"
#include "coop/interactables/atv_drill_events.h"

#include "harness/autotest/peer_signal.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace coop::dev::atv_riders_drill::detail {

namespace DE = coop::atv_drill_events;
namespace PS = harness::autotest::peer_signal;
namespace AD = coop::dev::atv_drive;
using AD::OnGt;

inline constexpr const char* kTag = "atv-riders";
inline constexpr uint8_t kSlotH = 0, kSlotO = 1, kSlotC = 2;   // the host, the observer that rejoins, the driver
inline constexpr float kRigidCm = 5.f;                         // a body's or a kerfur's drift from its seat, at most
inline constexpr float kDriveCm = 600.f;                       // the host's ATV has moved this far from where the drive began
inline constexpr float kPlacedCm = 50.f;                       // an unseated mirror's next pose puts it this near the streamed position

inline constexpr int kJoinS = 480;     // a peer's boot, join and world load
inline constexpr int kSignalS = 240;   // a step of another peer
inline constexpr int kWalkS = 420;     // another peer's director walk
inline constexpr int kRideS = 240;     // the host's kerfur walks to the ATV and sits
inline constexpr int kDriveS = 180;    // the ATV's drive to its 600 cm
inline constexpr int kRejoinS = 480;   // the observer's restart, boot and join
inline constexpr int kEventS = 60;     // a decision event of one step
inline constexpr int kBarrierS = 60;   // a barrier between peers that are up and past the step before it

enum Role : int { kHost = 0, kObserver = 1, kDriver = 2, kRoles = 3 };
const char* RoleName(Role r);

// The run's shared facts: the ATV's key, the riding kerfur's eid and the driver's body's distance from the ATV root at its attach.
extern std::wstring g_key;
extern uint32_t g_eid;
extern float g_attachCm;
int32_t Tag();   // the key's record tag
std::string Narrow(const std::wstring& w);   // the key as a signal's text (keys are ASCII)

// ---- the verdict and the waits ----

bool Aborted();
void Fail(int step, const char* red, const std::string& why);   // FAIL: `red` names the red it is, "" for none
void Invalid(const std::string& why);
void StepPass(int step, const std::string& what);
bool ParseMark(const std::string& text, const char* what, uint32_t& out);   // a peer's number mark; malformed: INVALID, false
// Waits for a peer's mark, saying first what it waits for and for how long (`ctx` names the wait's frame, e.g. a barrier).
bool AwaitMark(const std::string& name, std::string& out, int boundS, const char* ctx = nullptr);
// This peer marks `name`; waits for the other two, who are already up and past the step before (kBarrierS).
bool Barrier(const char* name, int boundS = kBarrierS);

// Polls `fn` each step: 1 done, 0 not yet, -1 stop (a verdict was said). False when the run ended, `fn` stopped it or the bound
// passed (INVALID said).
bool PollFn(const char* what, int boundS, const std::function<int()>& fn);

// ---- the records ----

struct Cur {
    uint32_t c[DE::kEventCount] = {};
};
Cur Snap();                                                    // each ring's count now
std::vector<DE::Rec> Recs(const Cur& cur, DE::Event e);        // the records of `e` after the cursor, oldest first

template <class Pred>
const DE::Rec* Find(const std::vector<DE::Rec>& v, Pred pred) {
    for (const DE::Rec& r : v)
        if (pred(r)) return &r;
    return nullptr;
}

// ---- the reads ----

struct BodyRead {
    bool exists = false, parentIsAtv = false, onAtv = false, collision = false, ragdoll = false;
    float offsetCm = -1.f;
};
BodyRead ReadBody(uint8_t slot);   // the puppet of `slot` against the ATV under g_key

struct KerfurRead {
    bool exists = false, parentIsAtv = false, hidden = false;
    float sitarrowCm = -1.f;
};
KerfurRead ReadKerfur(uint32_t eid);   // the NPC mirror of `eid`, a kerfur
bool KerfurMirrorUp(uint32_t eid);     // eid 0: any kerfur mirror
bool PuppetUp(uint8_t slot);
bool WorldUp();                        // the announced world and the host's puppet exist here

// The helpers the steps share (atv_riders_drill_steps.cpp). SeatSilent says the red `seat_announce_off`'s FAIL when a record
// shows a seat change of this ATV announced to no subscriber. SitLocal tries the local player's seat verb to its end and
// reads, in the call it finishes in, how many kerfur mirrors the game's own sit left invited and unseated (-1: not seated).
std::string Num(const char* fmt, float v);
uint8_t OccBefore(const DE::Rec& r);
uint8_t OccAfter(const DE::Rec& r);
bool SeatSilent(const std::vector<DE::Rec>& seats);
bool EntryGone();                                   // the ATV's copy under g_key is gone from this peer
bool ReleasedRec(const DE::Rec& r, uint8_t slot);   // a body-seat record: slot released, seat pose off, collision on
bool SitLocal(int& invited);

// ---- the steps, in the order the drill runs them ----

// Host
bool HostFixture();    // the kerfur beside the ATV (a kerfur of the save's own for `join`); posts `kerfur-eid`
bool HostRide(const Cur& cur);       // steps 1, 2 and 4 on the host
bool HostDrive();                    // step 3 on the host
bool HostRejoin();                   // waits the observer's leaving and return; names the key and the rider again
bool LeaveHost(const Cur& cur);      // step 6
bool HostSits(const Cur& cur);       // step 7
bool HostDestroys();                 // step 9

// Observer and driver
bool ObserverSeat(const Cur& cur);                       // step 1 on the observer
bool ClientRide(Role me, const Cur& cur);                // step 2 on a client (and 4 on the observer)
bool WatchDrive();                                       // step 3 on the observer
bool RejoinedObserver();                                 // step 5, the observer's second life
bool LeaveWatch(Role me, const Cur& cur);                // step 6 on a client
bool WatchHostSeat(Role me, const Cur& cur);             // step 7 on a client
bool WatchExit(Role me, const Cur& cur);                 // step 9 on a client

// Both host and observer, for the driver's ragdoll
bool RagdollWatch(Role me, const Cur& cur);              // step 8

// Driver
bool DriverSit(const Cur& cur);        // the seat, and the local check of step 2
bool DriverDrive();                    // step 3's act
bool DriverLeave();                    // step 6's act
bool DriverRagdoll();                  // step 8's act: sit again, ragdoll, get up

}  // namespace coop::dev::atv_riders_drill::detail
