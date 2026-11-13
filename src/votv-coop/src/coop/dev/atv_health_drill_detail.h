// coop/dev/atv_health_drill_detail.h -- what the ATV health drill's files share: the arm table, the phase clock, the
// verdict lines, the host's phase record and the lobby-chat lines the two peers say to each other
// (coop/dev/drill_chat.h). Private to coop/dev: atv_health_drill*.cpp.

#pragma once

#include "atv_seat.h"  // SitTry (co-located private header)

#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"
#include "coop/interactables/atv_authority.h"
#include "coop/interactables/atv_sync.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/types.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::atv_health_drill::detail {

using Clock = std::chrono::steady_clock;

enum class Arm : uint8_t {
    Off, Sit, Grab, Occupied, Synced, Silent, Gate, Owner, Trap, Spawned, CarryStill, SyncedPeer, Join, StreamPace
};

// An arm's names: its own, the host's first edge, the client's act and the client's undo (the phases' names).
struct Spec {
    Arm         arm;
    const char* name;
    const char* first;
    const char* act;
    const char* undo;
};
const Spec& SpecOf(Arm a);
Arm         ArmOf();

inline constexpr uint8_t           kSlot = 1;               // the one slot the drill acts on
// The world's own ATV, under its game key: only `trap` acts on it (the game's atvExplode arms the gamemode's `car`).
// Every other arm acts on the ATV the host spawns (SpawnStep) under the key it says in the lobby chat.
inline constexpr const wchar_t*    kWorldAtvKey = L"ATV";
inline constexpr uint8_t           kNoSeat = 0xFF;
inline constexpr auto              kFailBound = std::chrono::seconds(180);      // a bound: INVALID when it passes, never a wait
inline constexpr auto              kHostFirstBound = std::chrono::seconds(360); // the client's walk and its act
inline constexpr int               kWalkDeadlineS = 300;
inline constexpr float             kSitReachCm = 400.f;     // the director's walk ends this close to the ATV's root
// The grab walk ends well inside the use key's reach: this is measured to the ATV's root, the game's to its body
// (stopped 116-119 cm away the grab took; at 144 it did not).
inline constexpr float             kGrabReachCm = 100.f;
// One role's phase: the arm's own enum value, its name for the lines, and when it began. `done` once the role has
// printed its verdict.
struct Run {
    int               phase = 0;
    const char*       name = "wait";
    Clock::time_point at{};
    bool              done = false;
};
void Enter(Run& r, int phase, const char* name);
bool Past(const Run& r, std::chrono::seconds bound);
long long Ms(Clock::time_point from, Clock::time_point to);

// The verdict lines. Each ends the role's run. A PASS and a FAIL are decided by an event; a bound that runs out with no
// event is INVALID (and says the rig's `rig: VERDICT atv-health-drill INVALID` line).
void Fail(const char* role, Arm a, Run& r, const std::string& what, const std::string& got, const std::string& want);
void Invalid(const char* role, Arm a, Run& r, const std::string& what, const std::string& got, const std::string& want);
void Pass(const char* role, Arm a, Run& r, const std::string& numbers);

// The lobby-chat lines: the client's ready line (the host's start), the host's phase edges (the client's) and the
// client's other edges (the host's).
std::string ReadyLine(Arm a, const char* role = "client");
std::string PhaseLine(const char* name);
std::string ClientLine(const char* what);

// The ATV the host spawns for every arm but `trap` (the steps are below): the actor, the clock of its spawn, its table
// and its rest, and the key it says in the lobby chat.
struct SpawnStep {
    ue_wrap::CachedObjRef atv;
    bool                  spawned = false;
    Clock::time_point     spawnAt{}, tableAt{}, parkedAt{}, lastStampAt{};
    std::wstring          key;
    ue_wrap::FVector      lastAt{};      // the last stamp's root and when it was read
    int                   calm = 0;      // successive stamp pairs under 5 cm/s
};

// The host's phases. Spawn and Park come first: the ATV is born and at rest before the arm's first edge. Wait..Yielded
// are the arms where the client acts (`sit`, `grab`, `silent`: First names the edge the client's act makes, Left the
// freed seat, Silenced and Yielded the silent syncer's loss and the client's yield); Walk, Act and Judge are the arms
// where the host acts (`occupied`, `synced`: atv_health_drill_host.cpp).
enum class HP : int { Wait, First, Left, Back, Silenced, Yielded, Walk, Act, Judge, Spawn, Park };

// The run of idle-stream blocks at rest since a baseline. Counted not clocked: the stream sends a block every 50 ms
// (kDriveSendMs), the rest rule needs kRestMs = 1 s of rest and its pass runs once a second, so an at-rest hand-back lands
// 1.05-2.05 s after the first still block; 70 still ones in a row (3.5 s) cover that whole span, so a hand-back that did not
// happen is known there. A block is still under kRestCmS, the lane's own speed test (atv_authority.cpp kRestCmS). The idle
// arm's `back` and the peers arms' release both decide on it, never on blocks that were moving.
constexpr uint64_t kRestBlocks = 70;
constexpr float    kRestCmS = 5.f;
struct RestRun {
    uint64_t seen = 0, blocks = 0;   // the lane's idle-stream blocks read so far, and the run of them at rest
    bool     last = false;           // the last block read was at rest
};
// `idleSinceBase` is the lane's idle-stream count since the baseline; `lastBlockSpeed` the table's last admitted block's speed.
void CountRest(RestRun& r, uint64_t idleSinceBase, float lastBlockSpeed);

struct Host {
    Run                                             run;
    SpawnStep                                       sp;       // the ATV every edge of the arm is read on
    coop::atv_authority::Counters                   base{};   // the lane's counters once the ATV is parked
    coop::atv_authority::Counters                   snap{};   // and at the host's own act
    Clock::time_point                               firstAt{}, leftAt{}, walkAt{}, arrivedAt{}, mutedAt{}, silencedAt{};
    std::shared_ptr<coop::director::BackgroundWalk> walk;
    atv_seat::SitTry                                sit;
    std::unique_ptr<coop::director::AimedGrab>      grab;
    bool                                            sawSent = false, sawMuted = false, sawYielded = false;
    RestRun                                         rest;   // `back`: the idle-stream blocks at rest
};
extern Host g_h;

inline void HostEnter(HP p, const char* name) { Enter(g_h.run, static_cast<int>(p), name); }
std::string Plus(uint64_t n);
std::string GrabWhy(const coop::director::AimedGrab& g);

// The arms where the host acts: its walk, its sit or grab, and the judgement of the client's raw request.
void ActHostTick(Arm a, const coop::atv_sync::TableView* t);
void HostEnd();   // a verdict or a reset: a walk still walking is cancelled, the grab let go of

void HostTick(coop::net::Session* s, Arm a);
void ClientTick(coop::net::Session* s, Arm a);
void HostReset();
void ClientReset();

// The client acts once its own streams run, as a player in a settled world does.
bool StreamsRun();

// The client half the walk-and-sit arms share (atv_health_drill_seated.cpp): the run, the ATV, the director's walk and
// the seating. An arm's own client state derives from it and keeps its phases; a step that fails prints the FAIL line
// and returns Failed, and the arm ends its run its own way. `role` is the name its lines print (`client`, or
// `observer` for the third peer of the peers arms).
struct SeatedClient {
    Run                                             run;
    const char*                                     role = "client";
    bool                                            idleSaid = false;
    ue_wrap::CachedObjRef                           atv;
    std::shared_ptr<coop::director::BackgroundWalk> walk;
    atv_seat::SitTry                                sit;
    Clock::time_point                               walkAt{}, arrivedAt{};
};

// Wait: still going. Next: this step is done, enter the next phase. Failed: the FAIL line is printed.
enum class Stage : uint8_t { Wait, Next, Failed };

// The ATV the table holds and where its root is.
struct AtvAt {
    void*           actor = nullptr;
    ue_wrap::FVector at{};
};

// A walking client's wait for the host's spawned ATV: its ready line once its streams run, the key from the host's chat
// line and the host's `phase parked`, each latched on sight (a live chat row retires after seconds).
struct SpawnedWait {
    bool         readySaid = false, haveKey = false, sawParked = false;
    std::wstring key;
};

// The head of a client tick: true with this machine's player and slot when the client acts. False when its run is done,
// its world is not announced or its slot is not the drill's (said once).
bool SeatedActs(SeatedClient& c, void** player, uint8_t* self);
// Once the streams run and the table holds the ATV under `key`: true with the ATV and its root (`trap`'s world ATV).
bool AtvReady(const std::wstring& key, AtvAt* out);
// The spawned ATV: true with the ATV and its root once the key and `parked` came and the table holds it. `due` is this
// tick's chat poll (DC::PollDue, called once a tick by the caller).
bool SpawnedAtvReady(SpawnedWait& w, Arm a, bool due, AtvAt* out);
// The director's walk to within `reachCm` of the ATV's root, the ATV kept, the ready line said.
void WalkStart(SeatedClient& c, Arm a, const AtvAt& t, float reachCm);
// The wait for the ATV: Failed (with INVALID said) once kFailBound has passed.
// `w` is the spawned ATV's wait (null for trap): its missing edge goes into the INVALID line.
Stage WaitBound(SeatedClient& c, Arm a, const SpawnedWait* w);
// The walk: Next once it arrived (arrivedAt set).
Stage WalkStep(SeatedClient& c, Arm a);
// The seat through the game's own verb, then the host's grant: Next once the local peer is the ATV's seat in the lane.
Stage SitStep(SeatedClient& c, Arm a, void* player, void* atv, uint8_t self);
// A walk still walking is cancelled.
void WalkCancel(SeatedClient& c);

// The damage arms (`gate`, `owner`, `trap`): the client sits in the spawned ATV (`trap`: the world's) and the host's blast, the client's kill
// or the host's story-event trap is judged on the owner's copy and the host's table (atv_health_drill_damage.cpp). They
// keep their own phases and state; Tick routes them past HostTick and ClientTick.
inline bool IsDamageArm(Arm a) { return a == Arm::Gate || a == Arm::Owner || a == Arm::Trap; }
void DamageHostTick(coop::net::Session* s, Arm a);
void DamageClientTick(coop::net::Session* s, Arm a);
void DamageReset();

// The `spawned` arm: the host spawns its own ATV beside the players and says its key; the client waits for the key in the
// table, walks to the ATV and sits (atv_health_drill_spawned.cpp). Own phases and state; Tick routes it past
// HostTick and ClientTick.
inline bool IsSpawnedArm(Arm a) { return a == Arm::Spawned; }
void SpawnedHostTick(coop::net::Session* s, Arm a);
void SpawnedClientTick(coop::net::Session* s, Arm a);
void SpawnedReset();

// The host half of the spawn every arm but `trap` shares (atv_health_drill_spawned.cpp): the host spawns an ATV beside
// its player and says its key in the lobby chat, then waits for it to park and says `phase parked`. The steps print the
// FAIL line on the host's own run and return Failed. The other peers read the key from the chat line.
Stage SpawnAtvStep(SpawnStep& s, Run& run, Arm a);   // Next once the key is said
Stage ParkAtvStep(SpawnStep& s, Run& run, Arm a);    // Next once parked and said
void  SayKey(const std::wstring& key);
bool  KeyFromChat(std::wstring* out);                // the last live key line, or false

// The peers arms (carry_still, synced_peer, join, stream_pace): the host spawns an ATV, the client acts on it and the
// observer is the third peer (atv_health_drill_peers.cpp). Own phases and state; Tick routes them past HostTick and
// ClientTick, and the observer is the client path of slot 2.
inline bool IsPeersArm(Arm a) {
    return a == Arm::CarryStill || a == Arm::SyncedPeer || a == Arm::Join || a == Arm::StreamPace;
}
void PeersHostTick(coop::net::Session* s, Arm a);
void PeersClientTick(coop::net::Session* s, Arm a);
void PeersReset();

}  // namespace coop::dev::atv_health_drill::detail
