// coop/dev/prop_act_drill.h -- [dev] the act drill: a client's take or use of a shared prop asks the host first
// (ini prop_act_drill=honest|race|depart|edge, env VOTVCOOP_PROP_ACT_DRILL; BOTH peers, the same value).
//
//   HOST   -- spawns each step's target in front of the client's puppet, waits until it is announced, preps it, checks
//             the judge's census of it against the row (logged), says a ready line (step, eid) in the lobby chat, and
//             judges its counters on the client's done line; a grab row also waits for the grab and the drive.
//   CLIENT -- reads the line, resolves the mirror by eid, walks within reach with the director, grabs it with the
//             game's grab on a grab row, runs the verb by reflection as a press would, waits until no request is
//             pending, judges what its own copies show, and says its done line.
// honest: one client takes and uses each family of shared prop. race: two clients and the host contend for one prop
// (with --observer; prop_act_drill_race.cpp, prop_act_drill_race_client.cpp). depart: a client leaves holding a
// reservation and a newcomer takes its slot (with --rejoin; prop_act_drill_depart.cpp). edge: the windows of the ask
// (prop_act_drill_edge.cpp, prop_act_drill_edge_client.cpp). A FAIL is the lane failing; every wait on the other peer
// ends on a chat line or a counter, and its bound is a bound that FAILs the step, never a wait.

#pragma once

#include "coop/element/element.h"
#include "coop/props/prop_act_host.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::prop_act_drill {

// Session start, game thread: says which phase is armed (a phase this build has no steps for is said too).
void Install(coop::net::Session* s);

// Advances this peer's steps; a latched read when off. Game thread.
void Tick(coop::net::Session* s);

// The session ended: the drill starts over.
void OnDisconnect();

// What the core (prop_act_drill.cpp), the client half (prop_act_drill_client.cpp) and a phase's steps
// (prop_act_drill_<phase>.cpp) share: the core runs the host's step machine and the client half the client's, over a
// phase's table, and the phase says what each step is and how the host judges it.
namespace detail {

inline constexpr int kClientSlot = 1;   // the client a single-client phase drives
inline constexpr const char* kTag = "[ACT-DRILL]";
inline constexpr uint64_t kPollEveryMs = 200;   // a chat walk, a few times a second

inline constexpr float kReachCm = 120.f;       // the client's stop beside the mirror
inline constexpr int kWalkDeadlineS = 60;      // the director's own bound on the walk
inline constexpr float kPairGapCm = 60.f;      // a pair's second prop, further along the spawn line

// Bounds: each ends a step that did not get its line or its count, and FAILs it.
inline constexpr uint64_t kReadyBoundMs = 120000;   // CLIENT: a step's start to the host's ready line
inline constexpr uint64_t kMirrorBoundMs = 60000;   // CLIENT: the ready line to the mirror being here
inline constexpr uint64_t kReplyBoundMs = 30000;    // CLIENT: the call to no request pending
inline constexpr uint64_t kGatherBoundMs = 10000;   // CLIENT: the walk's arrival to the mirror being gatherable
inline constexpr uint64_t kHandClearBoundMs = 10000; // CLIENT: the hand's clear before a grab to both slots reading empty
inline constexpr uint64_t kGrabBoundMs = 45000;    // CLIENT: a grab row's grab step, a wall-clock backstop; the grab ends itself
inline constexpr uint64_t kDrivenBoundMs = 20000;   // CLIENT: its grabbed line to the host's driven line
inline constexpr uint64_t kDragBoundMs = 15000;     // HOST: the client's grabbed line to the prop driven by the client
inline constexpr uint64_t kAnnounceBoundMs = 60000; // HOST: the spawn to the target having a row
inline constexpr uint64_t kDoneBoundMs = 180000;    // HOST: the ready line to the client's done line
inline constexpr uint64_t kJudgeBoundMs = 30000;    // HOST: the done line to the counters it must show

// The targets two phases share.
inline constexpr const wchar_t* kCrowbar = L"/Game/objects/prop_crowbar.prop_crowbar_C";
inline constexpr const wchar_t* kFood    = L"/Game/objects/prop_food.prop_food_C";

enum class Verb : uint8_t { Pocket, Hold, Use, Soap };   // putObjectInventory2, Hold Object, actionOptionIndex 4, playerUsedOn

// Where a consumed target ends, on the client's own copies: the inventory gains one of its class, the hand holds one
// of its class, or nowhere (an eaten or used-up prop). Stays: the take bails (a full inventory) and the mirror and the
// inventory are as they were. Unchecked: the call runs natively and only the absence of a request is judged. NativeGone:
// the same, and the eaten copy is gone from this client afterwards. Refused: the host answered WrongFamily (this
// client's RefusedCount for it rose by one), no request is open, and the mirror, the inventory and the hand are as
// they were. Taken: the mirror is gone after the commit and the hand is not asserted (a heavy item's hand view is
// unmeasured).
enum class Ends : uint8_t { Inventory, Hand, Gone, Stays, Unchecked, NativeGone, Refused, Taken };

// Who makes a step's target: the host, on the floor in front of the client's puppet (HostTwo: a second one beside it;
// HostKerfur: a kerfur NPC the host turns off into its prop), or the client itself (the host spawns nothing).
enum class Target : uint8_t { Host, HostTwo, HostKerfur, Client };

struct Row {
    const char* name;           // the step's name in every line
    const wchar_t* path;        // the target's class path, which the host spawns
    const wchar_t* leaf;        // its class name, as an inventory record and the hand's actor carry it
    Verb verb;
    Ends ends;
    const char* (*setup)(void* player, void* mirror);   // client, before the act: null when done, else why not; or null
    Target target = Target::Host;
    bool asks = true;         // the call opens a request; false: it runs natively and none is open
    bool hostKeeps = false;   // !asks: the host's prop is still live after the step
    // Target::Client: the client's own target, spawned and its cell read back; null when made, else why not.
    const char* (*make)(void* player, void** out) = nullptr;
    // One own spawn pocketed natively per tick, `*full` set once the native return is false (the take then bails);
    // null when done, else why not. The step starts only once the addHint watch is live.
    const char* (*fillStep)(void* player, bool* full) = nullptr;
    // The client grabs the mirror with the game's grab (the director's aimed grab) before its act, and the host must
    // see the prop driven by the client before the act runs.
    bool grab = false;
    // A refused row: the term of the host's judge that refuses it, the tally its count must rise by one.
    coop::prop_act_host::Tally fitTally = coop::prop_act_host::Tally::kCount;
    // HOST, after the target is announced and before the ready line: called each poll until `*done`. `sinceMs` is 0 on
    // the first call, then the time since it. A non-null return FAILs the step with it.
    const char* (*hostPrep)(void* actor, uint64_t sinceMs, bool* done) = nullptr;
};

// The host's commit counters for the client's slot when it said the step was ready, and the second prop's eid; the
// raw PropDestroy counts live in the watch (remote_prop.h).
struct Baseline {
    uint32_t destroys = 0;      // CommitsOf(slot, Destroy)
    uint32_t aborts = 0;        // CommitsOf(slot, Abort)
    coop::element::ElementId eid2 = coop::element::kInvalidId;   // a HostTwo row's second prop, as the host holds it
    uint32_t answered = 0;      // AnsweredTotal(slot)
    uint32_t wrongFamily = 0;   // AnsweredFor(slot, WrongFamily)
    uint32_t fit = 0;           // TallyOf(the row's fitTally)
    bool kerfurBound = false;   // GetKerfurIdForEid(eid) named a kerfur record
};

enum class Judged : uint8_t { Wait, Pass, Fail };

// The host's verdict on a step once its client said it was done; `why` says what a Wait is on or what failed, and on a
// Pass what held. Wait is called again each check until the step's bound.
using JudgeFn = Judged (*)(const Row& row, coop::element::ElementId eid, const Baseline& base, std::string* why);

struct Phase {
    const char* name;
    const Row* rows;
    size_t count;
    JudgeFn judge;
};

const Phase& Honest();   // prop_act_drill_honest.cpp

// The race phase runs all three peers by itself (prop_act_drill_race.cpp is the host's half,
// prop_act_drill_race_client.cpp the clients'): the host's steps, then a client's by its slot, 1 or 2. The core routes
// to them.
void RaceHostTick(coop::net::Session* s);
void RaceClientTick(coop::net::Session* s);
void RaceReset();

// The depart and edge phases run their peers by themselves too, each routed by role as race is. The edge client half
// also holds this client's announce from the session's start (EdgeInstall).
void DepartHostTick(coop::net::Session* s);
void DepartClientTick(coop::net::Session* s);
void DepartReset();
void EdgeInstall(coop::net::Session* s);
void EdgeHostTick(coop::net::Session* s);
void EdgeClientTick(coop::net::Session* s);
void EdgeReset();

// The core's, which the client half uses: the clock, the armed phase's name, the two lines the peers say to each other
// in the lobby chat (the host's ready head and the client's done head) and the chat send.
uint64_t NowMs();
const std::string& PhaseName();
std::string ReadyHead(const Row& r);
std::string DoneHead(const Row& r, int slot = kClientSlot);
// The head of a grab row's two mid-step lines: the client's "grabbed" and the host's "driven" follow it.
std::string DragHead(const Row& r, bool fromHost);
void Say(const std::string& line);

// The text after `head` in the first row of the lobby record that `slot` said; false when none yet. HOST.
bool ClientSaid(int slot, const std::string& head, std::string* rest);

// Spawns the row's class on the first floor round slot 1's puppet (its facing first, then a fan of 8 directions),
// `extraCm` further along the same direction when it is a pair's second. Null on success, else why not. HOST. ClientPuppetReady is what an arm gate waits on before it: the puppet
// is live and has a pose.
const char* SpawnTarget(const Row& row, float extraCm, void** out);
bool ClientPuppetReady();

// True when the mirror is there and the host's drive no longer parks it: the state a player meets it in. The host's
// drive of a prop that fell parks the mirror with physics off until the drive ends, and a take then bails. A pocket
// also needs the game's own gather test (ue_wrap::prop_gather::IsGatherable). CLIENT.
bool MirrorGatherable(void* mirror, Verb verb);

// The mirror's state for a FAIL line, " (child=%d frozen=%d simulating=%d parked=%d)" from the reads MirrorGatherable
// makes, or " (no mirror)". CLIENT.
std::string MirrorState(void* mirror);

// What a client's steps use (prop_act_drill_client.cpp). HostSaid reads the text after `head` in the first row of this
// client's feed that carries it. RunVerb is the row's verb on this player and the mirror, the call a press makes: null
// on success, else why it could not run. InventoryCount is -1 when the store cannot be read.
bool HostSaid(const std::string& head, std::string* rest);
// The same, but only a row that arrived live: a row the join seeded from the host's record is history.
bool HostSaidLive(const std::string& head, std::string* rest);
bool OwnJoinOver();
int InventoryCount(const wchar_t* leaf);
void* RootOf(void* actor);
const char* RunVerb(const Row& row, void* player, void* mirror);

// A client's copy on its last use, so an eat of it is a destroy the watch asks the host about (prop_act_drill_honest.cpp).
const char* SetLastUse(void* player, void* mirror);

// The client half: one check of its steps, and its state back to the start.
void ClientTick(coop::net::Session* s, const Phase& ph);
void ClientReset();

// What the honest rows need of the engine (prop_act_drill_honest.cpp). The hint watch tells a full inventory's refusal
// ("Not enough space") from any other game hint.
bool HintWatchLive();   // registered and its names resolved; a tick drives the resolve
void HintWatchOff();    // retired when the phase ends: a live name watch costs every script call
void ClearHint();
bool HintSeen();        // the hint was shown since ClearHint
void* FindKerfurProp(coop::element::ElementId kerfurId);        // the live prop whose eid names that kerfur record

}  // namespace detail
}  // namespace coop::dev::prop_act_drill
