// coop/dev/atv_riders_drill_detail.h -- private to the ATV riders drill: the verdict, the waits and the record cursor
// (atv_riders_drill.cpp), and the reads and the steps that judge them (atv_riders_drill_steps.cpp, atv_riders_drill_late.cpp, atv_riders_drill_drive.cpp). Every read hops to the game
// thread for the engine; every step is called from the drill's own thread and returns false when the run ended (a verdict was
// said here or on another peer) or the step could not be reached.
#pragma once

#include "coop/dev/atv_drive.h"
#include "coop/dev/atv_riders_arm.h"
#include "coop/interactables/atv_drill_events.h"

#include "harness/autotest/peer_signal.h"

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace coop::dev::atv_riders_drill::detail {

namespace DE = coop::atv_drill_events;
namespace PS = harness::autotest::peer_signal;
namespace AD = coop::dev::atv_drive;
using AD::OnGt;
using AD::DriveBack;      // the host's drive leg: the reverse key down
using AD::DriveBackOff;   // and up

inline constexpr const char* kTag = "atv-riders";
inline constexpr uint8_t kSlotH = 0, kSlotO = 1, kSlotC = 2;   // the host, the observer that rejoins, the driver
inline constexpr float kRigidCm = 5.f;                         // a body's or a kerfur's drift from its seat, at most
inline constexpr float kDriveCm = 600.f;                       // the host's ATV has moved this far from where the drive began

inline constexpr int kJoinS = 480;     // a peer's boot, join and world load
inline constexpr int kSignalS = 240;   // a step of another peer
inline constexpr int kWalkS = 420;     // another peer's director walk
inline constexpr int kRideS = 240;     // the host's kerfur walks to the ATV and sits
inline constexpr int kDriveS = 180;    // the ATV's drive to its 600 cm
inline constexpr int kRejoinS = 480;   // the observer's restart, boot and join
inline constexpr int kEventS = 60;     // a decision event of one step
// A barrier between peers that are up and past the step before it. The skew at a barrier is the gap between the peers' previous
// steps, not measured: a peer later than this ends the run INVALID, never with a wrong verdict.
inline constexpr int kBarrierS = 60;

enum Role : int { kHost = 0, kObserver = 1, kDriver = 2, kRoles = 3 };
const char* RoleName(Role r);
Role MyRole();   // the role this peer plays

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
// passed (INVALID said, with `atBound`'s text after the bound's own when one is given).
bool PollFn(const char* what, int boundS, const std::function<int()>& fn, const std::function<std::string()>& atBound = {});

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
bool ReadRoot(ue_wrap::FVector& out);   // the ATV root under g_key; false when it does not read

// The kerfur's distance to the ATV's sitarrow, its offset from the ATV's root, and the sitarrow component's own offset from the
// root (cm, world axes). The reads are the copy's own: on the host the kerfur is the game's, on a client the mirror.
struct KerfurRead {
    bool exists = false, parentIsAtv = false, hidden = false;
    float sitarrowCm = -1.f;
    float off[3] = {0.f, 0.f, 0.f}, sitOff[3] = {0.f, 0.f, 0.f};
};
KerfurRead ReadKerfur(uint32_t eid);   // the NPC element of `eid` on this peer (the mirror on a client), a kerfur
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

// The measurement of where a peer seats the driver (atv_riders_drill_seat_offset.cpp), one line per seating, in the ATV root's
// axes: the driver's own machine says where its pawn, the ATV's camera and its sitarrow are; every other peer says where the
// driver's body, the ATV's `playerHit` (the game's own driver seat) and the sitarrow are. Both Say calls run on the game thread.
// JudgeRemoteSeat says the line from the drill's own thread and judges it: the body of `slot` within kRigidCm of `playerHit`, else
// FAIL `remote_seat_off` for `step` (naming both places), and INVALID when the body or `playerHit` does not read. False when it
// said a verdict.
void SayLocalSeatOffset();
void SayRemoteSeatOffset(uint8_t slot);
bool JudgeRemoteSeat(uint8_t slot, int step);

// ---- the steps, in the order the drill runs them ----

// Step 0, the reveal route, on the driver's first join (the values on, bind_ride_off and reveal_ride_off). In the driver's join
// window, the host's world taken for it and its world not yet ready (save_transfer::WorldTakenFor), the host spawns the kerfur
// beside the ATV and sits, so its game seats the kerfur; the driver's mirror is born hidden in the join's snapshot, stores the ride
// row, and rides at the reveal. The driver decides at the reveal and marks `s0_done`; the host then gets off and waits for its
// kerfur to leave the ATV. Every other value skips the step (coop/dev/atv_riders_arm.h RunsFirstJoin). The driver's lift of the
// join window is held until the kerfur's mirror is bound (coop/dev/atv_riders_join_hold), so the mirror is born hidden.
bool HostFirstJoin();
bool DriverFirstJoin();

// Host
bool HostFixture();    // the kerfur beside the ATV (step 0's when that ran); posts `kerfur-eid`
bool HostRide(const Cur& cur);       // steps 1, 2 and 4 on the host
bool HostDrive();                    // step 3 on the host
bool HostRejoin();                   // waits the observer's leaving and return; names the key and the rider again
bool LeaveHost(const Cur& cur);      // step 6
bool HostSits(const Cur& cur);       // step 7: the host sits (its kerfur came off a client's ride first), then the two reads
bool HostDriveSeated();              // step 7's second leg on the host: its own 600 cm drive in reverse, then the read at its end
// The control run (`drive_control`): step 7 is the green until the host sits, but the host's kerfur takes no seat, so leg B
// measures the host driving the same ATV with no kerfur on it. The ATV's own sit invites every kerfur whose state is 0
// (follow), so the control sets the host's kerfur idle through the game's own radial verb before the host sits.
inline bool Control() { return coop::dev::atv_riders_arm::Is("drive_control"); }
bool HostKerfurIdle();               // the control's act: the host's kerfur takes the game's 'idle' verb; false when it ended
// Step 7's read on this peer: the kerfur of g_eid stands within kRigidCm of the ATV's sitarrow. `leg` 'a' is the host's next block
// after the kerfur's seat, 'b' the end of the host's drive. Says the reading, leaves it as a mark for the others, and FAILs
// `host_kerfur_off_seat` naming this peer, the offsets, and the readings the other peers have already left. False when it ended.
bool JudgeKerfurSeat(char leg, const char* when);
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
