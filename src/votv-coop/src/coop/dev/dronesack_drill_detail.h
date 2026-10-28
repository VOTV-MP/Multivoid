// coop/dev/dronesack_drill_detail.h -- what the drone sack drill's files share: its bounds, the chat lines the two
// peers say to each other, the record of destroys the seam hooks fill, and the reads of a record count.
// Impl-private: nothing outside coop/dev includes it. Game thread.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coop::net { class Session; }

namespace coop::dev::dronesack_drill::detail {

// ---- the measure ----

inline constexpr float    kSackAheadCm = 300.f;    // the sack stands this far in front of the host
inline constexpr float    kCrateAboveCm = 150.f;   // arm 1: the crate starts this far above the sack's origin
inline constexpr float    kFloorDownCm = 400.f;
inline constexpr float    kMinColumnCm = 20.f;
inline constexpr float    kRiseCm = 40.f;          // a spawned prop starts this far above its floor
inline constexpr uint64_t kWatchMs = 15000;        // arm 1: the host watches the crate this long from the drop

// Arm 2: the loose props and the client's carry.
inline constexpr int      kPropCount = 6;          // loose props round the sack
inline constexpr float    kPropRingCm = 150.f;     // ... on a ring this far from the sack's origin
inline constexpr int      kPropSpots = 12;         // ... tried at this many angles for a floor
inline constexpr float    kGrabReachCm = 150.f;    // the client's walk ends this near a prop it grabs
inline constexpr float    kSackReachCm = 55.f;     // ... and this near the sack's origin (flat) when it carries one
inline constexpr float    kFeetCm = 150.f;         // a carry has arrived only with the player this near the sack's height
// The release window, measured on the held prop: within kOverFlatCm of the sack's origin, flat, and between
// kOverMinCm and kOverMaxCm above it. The aim steers the held prop toward kOverAimCm above the origin.
inline constexpr float    kOverFlatCm = 35.f;
inline constexpr float    kOverMinCm = 40.f;
inline constexpr float    kOverMaxCm = 90.f;
inline constexpr float    kOverAimCm = 65.f;
inline constexpr int      kAimTicks = 40;          // the aim is corrected for at most this many ticks per prop
inline constexpr float    kAimStepDeg = 3.f;       // ... by at most this much on each axis per tick
inline constexpr float    kAimGain = 0.5f;         // ... and half of the measured angle between the prop and its target
inline constexpr int      kGrabTries = 2;
inline constexpr int      kWalkDeadlineS = 300;    // the director's own bound on one walk

// ---- failure bounds of the instrument, never a wait for a milestone ----

inline constexpr uint64_t kPlaceBoundMs = 60000;   // a floor in front of the host
inline constexpr uint64_t kEidBoundMs = 30000;     // a spawned prop takes its element id
inline constexpr uint64_t kBoundBoundMs = 120000;  // the client says its sack mirror is bound
inline constexpr uint64_t kMirrorBoundMs = 120000; // the client's mirror of the host's sack or prop exists
inline constexpr uint64_t kDriveBoundMs = 60000;   // a host-spawned prop leaves the host's drive
inline constexpr uint64_t kPutBoundMs = 15000;     // a prop let go over the sack is seen to die on the client
inline constexpr uint64_t kFillItemBoundMs = 10000; // arm 3: a dropped item is absorbed within this, else it is logged as missed
inline constexpr uint64_t kTakeBoundMs = 180000;    // arm 4: the client says whether it took from the sold sack
inline constexpr uint64_t kTakeSettleMs = 5000;     // arm 4: the host waits this long for a taken slice to be judged (else: none)
inline constexpr uint64_t kClientBoundMs = 1800000; // the client's whole route
inline constexpr uint64_t kPutsSettleMs = 20000;   // the host waits this long for the props still outside the sack
inline constexpr uint64_t kParkBoundMs = 240000;   // a drone in flight at the call lands within minutes
inline constexpr uint64_t kSaleBoundMs = 600000;   // the drone flies out, sells and the container is cleared
inline constexpr uint64_t kCensusLaterMs = 5000;  // arm 2: the census after the sale is read again this long after it

// ---- the chat lines ("rest" is the text after the prefix) ----

inline constexpr const char* kSackPrefix = "sack host sack ";     // host: the sack's element id
inline constexpr const char* kBoundPrefix = "sack client bound "; // client: its mirror of that id is bound
inline constexpr const char* kCratePrefix = "sack host crate ";   // host: arm 1: the crate's element id
inline constexpr const char* kPropsPrefix = "sack host props ";   // host: arm 2: the loose props' element ids, comma-separated
inline constexpr const char* kIndexPrefix = "sack host index ";   // host: arm 3: the crate's inventory index, when it read
inline constexpr const char* kTookPrefix = "sack client took ";   // client: arm 4: 1 if it took a record out of the sold sack, else 0
inline constexpr const char* kPutsDoneLine = "sack client puts done"; // client: arm 2: every prop was tried
inline constexpr const char* kCallLine = "sack host call";        // host: arm 2: the census before is taken, the drone is called
inline constexpr const char* kSoldLine = "sack host sold";        // host: arm 2: the drone container is cleared
inline constexpr const char* kDoneLine = "sack host done";        // host: the run is over

// ---- shared state (dronesack_drill.cpp) ----

// One destroy of a watched actor seen on this peer while the drill is armed: an exact prop_container_C, or, in
// arm 2, an exact prop_C.
struct Destroyed {
    void* actor = nullptr;
    uint32_t eid = 0;
    uint64_t ms = 0;            // GetTickCount64 at the destroy
    std::wstring by;            // the caller's class, or "<native>"
    bool broadcast = false;     // the destroy seam sent a PropDestroy for it
    bool wire = false;          // this peer was destroying it on the wire's word (the echo guard's mark)
    bool container = false;     // a prop_container_C, else a prop_C
    bool printed = false;
};

// A PropDestroy for a watched eid that arrived from a client (host only).
struct Received {
    uint32_t eid = 0;
    int slot = 0;
    uint64_t ms = 0;
    bool alive = false;         // the actor still lived on this peer when it arrived
    bool printed = false;
};

uint64_t NowMs();
void Say(const std::string& line);
bool Rest(const std::string& prefix, std::string* rest);
bool Heard(const std::string& line);

// 1 or 2: the run's arm (the dronesack_drill row's value).
int Mode();
// Start recording the destroys the hooks see. Resolves the container class (and, in arm 2, prop_C); false when one
// does not.
bool Arm(bool isHost);
// The crate named by its element id and key: the received-PropDestroy hook matches it.
void SetCrate(uint32_t eid, const std::string& key);
// Arm 2: the loose props' element ids, which the received-PropDestroy hook matches.
void SetWatch(const std::vector<uint32_t>& eids);
// Prints one recorded destroy once: the crate's own line (`crateEid` its element id, `withBroadcast` the client's
// form) or an "other" line; times are +ms from `refMs`.
void PrintDestroy(const char* peer, Destroyed& d, uint32_t crateEid, uint64_t refMs, bool withBroadcast);
std::vector<Destroyed>& Destroys();
std::vector<Received>& Recv();
uint32_t CrateEid();

// The record count of a container actor's propInventory slot, or -1 when it does not read.
int RecordsOf(void* containerActor);
// The container a holder (a drone sack, the drone) is bound to through its `container` field, or null.
void* ContainerOf(void* holder);
// The record count of the container a drone sack is bound to (sack.container), or -1.
int SackRecords(void* sack);

// ---- the census (dronesack_drill_census.cpp), arm 2 ----

// The three moments a peer reads every world container: after the puts and before the drone is called, right after
// the sale, and kCensusLaterMs after that.
enum class Moment : uint8_t { Before, After, AfterLater };
// Logs one line per world container and the length of the save slot's GObjStack, and keeps the rows. `role` is
// "host" or "client".
void CensusTake(const char* role, Moment m);
// Logs the containers that differ between the Before and AfterLater rows, then the count; the sack's container and
// the drone's are excluded. `sack` is this peer's sack (null: only the drone's is excluded).
void CensusDiff(const char* role, void* sack);
// Arm 3: logs which of the Moment's containers report GObjStack index `index` (the one read off the dropped crate), or that
// none does. A negative index (unread) is said and skipped.
void CensusIndexHolders(const char* role, Moment m, int32_t index);
void CensusReset();
// The GObjStack index the container's propInventory holds, or -1.
int32_t ContainerIndex(void* container);
// Whether the container is one of container_contents_sync's world containers on this peer (it has an element id and
// ForEachWorldContainer walks it).
bool ContainerInSyncScope(void* container);

// ---- the roles ----

void HostTick(coop::net::Session* s);
void HostReset();

}  // namespace coop::dev::dronesack_drill::detail
