// coop/interactables/cord_sync_detail.h -- what the cord lane's two files share: the counters, a link as it travels,
// the session test, and the seam between the lane (cord_sync.cpp) and its script-gate watches (cord_sync_watch.cpp).
// Impl-private: nothing outside coop/interactables includes this. Game thread.

#pragma once

#include "coop/net/protocol.h"

#include <cstddef>
#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::cord_sync::detail {

// A claim's reach: an arm's length doubled, the wide device lanes' reach (desk_verb_intent.cpp, camera_toggle_intent.cpp).
inline constexpr float kReachUU = 400.0f;

// A claim is a person's plug: twelve in two seconds is more than a hand makes, the floppy slot lane's window
// (floppy_slot_sync.cpp). A claim past it is answered, unapplied.
inline constexpr uint64_t kRateWindowMs = 2000;
inline constexpr int kMaxClaimsPerWindow = 12;

// Parked canonicals, newest per cord; one past the cap drops the oldest. A base's corded props arrive in one seed,
// so the cap holds them all.
inline constexpr size_t kMaxParks = 256;

// The longest key the wire carries.
inline constexpr size_t kKeyMax = sizeof(coop::net::WireKey::data);

// Every refusal and drop of the lane, said in one line a minute (cord_sync.cpp). Host: claims taken, applied, found
// equal, refused (every refusal is answered; keyPending a key lookup that could not answer yet, laneOff a claim met
// while the lane's watches were not all live), the broadcasts and the seeds. Both: sockBuilds, the times the level
// sockets' table was read from the object index. Client: claims sent, canonicals applied or
// found equal, those dropped by the pending rule, held for a drag, parked, and the stretch breaks refused. The
// dispatcher's refusals, by reason (cord_sync.h, Drop): dropSize, dropKey, dropOp, dropField; badWire counts a
// message that reached the lane off the game thread or a canonical with no key.
struct Counters {
    uint32_t claims = 0, claimsApplied = 0, claimsEqual = 0;
    uint32_t refusedRate = 0, refusedDead = 0, refusedReach = 0, refusedHostDrag = 0, unresolved = 0;
    uint32_t keyPending = 0, laneOff = 0, sockBuilds = 0;
    uint32_t broadcasts = 0, answers = 0, seeds = 0, seedLinks = 0;
    uint32_t claimsSent = 0, applied = 0, equal = 0, stillDiffering = 0, applyFailed = 0;
    uint32_t pendingDropped = 0, held = 0;
    uint32_t parked = 0, parkOverflow = 0, parkDead = 0, parkLeft = 0;
    uint32_t stretchRefused = 0;
    uint32_t badWire = 0, unsyncable = 0, sendFailed = 0;
    uint32_t dropSize = 0, dropKey = 0, dropOp = 0, dropField = 0;
};
Counters& Stat();

// A cord's link as it travels: sockForm 0 unplugged, 1 a socket by its own key, 2 a child socket by its owner prop's
// key and component name. kFormUnsyncable is a link the wire cannot name (a key past the wire's length or none).
inline constexpr uint8_t kFormUnsyncable = 0xFE;
struct LinkId {
    uint8_t form = 0;
    coop::net::WireKey sock{};
    coop::net::WireKey child{};
};
bool SameLink(const LinkId& a, const LinkId& b);

// The connected session, or null; its role.
coop::net::Session* Connected();
bool IsHost(const coop::net::Session* s);
bool IsClient(const coop::net::Session* s);

// ---- the lane, called by the watches (cord_sync.cpp) ----

// CLIENT: a plug or unplug ran inside the local player's cord verbs; claim its outcome when it differs from the last
// link this client reported or applied for that cord.
void OnClientLinkEdge(void* cord);
// HOST: a cord's link may have changed, by any caller; broadcast it when it differs from the last broadcast.
void OnHostLinkEdge(void* cord);
// CLIENT: the local player's drag of that cord ended; deliver the canonical held behind it.
void OnDragEnded(void* cord);
// The cord is being destroyed: drop what the lane holds for it.
void OnCordDying(void* cord);

// ---- the watches (cord_sync_watch.cpp), called by the lane ----

// Register the six watches once; false when one was refused (said). Settle them each Tick.
void InstallWatches();
void TickWatches();
// The session's end: the drag latches go.
void ClearLatches();

// The session's end: the level sockets' table goes (cord_sync_sockets.cpp).
void ClearSockets();

// Whether the lane runs: all six watches live and the release's `pressed` parameter resolved. A watch that is dead or
// a parameter that does not resolve turns the lane off as a unit (said once), since half its rules would hold a
// canonical behind a drag nothing ends, or claim nothing at all.
bool LaneLive();

// Whether the local player is dragging that cord: set at its actionOptionIndex, cleared at its release, its
// disconnect and the session's end.
bool Dragging(void* cord);

// Set while the lane runs a plug or unplug of its own, so the watches read the call as the lane's, not a player's.
class ApplyScope {
public:
    ApplyScope();
    ~ApplyScope();
    ApplyScope(const ApplyScope&) = delete;
    ApplyScope& operator=(const ApplyScope&) = delete;
private:
    bool previous_;
};

}  // namespace coop::cord_sync::detail
