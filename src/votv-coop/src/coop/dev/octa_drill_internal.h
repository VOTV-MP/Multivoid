// coop/dev/octa_drill_internal.h -- what the Octa drill's files share: the constants and arm rules, the one state of the
// run, and the host's, the births arm's and the client's entry points (octa_drill_host.cpp, octa_drill_births.cpp,
// octa_drill.cpp). See coop/dev/octa_drill.h. Dev only; game thread.

#pragma once

#include "coop/creatures/geom_octa_relay.h"
#include "coop/creatures/geom_octa_sync.h"
#include "coop/dev/director/director.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/sdk_profile.h"  // NpcClass_GeomOcta
#include "ue_wrap/core/types.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace coop::net { class Session; }

namespace coop::dev::octa_drill::detail {

namespace D  = coop::director;
namespace GS = coop::geom_octa_sync;
using ue_wrap::FVector;

constexpr const char*    kTag         = "[OCTA-DRILL]";
constexpr const wchar_t* kOctaClass   = ue_wrap::profile::name::NpcClass_GeomOcta;
constexpr const wchar_t* kGuardMember = L"startInvinc";

constexpr uint64_t kEveryMs          = 250;
constexpr float    kSpawnRingCm      = 1500.f;  // births: the Octa stands this far from the host
constexpr float    kPlaceFromCm      = 2200.f;  // measure: the walker takes its place this far from the other player
constexpr float    kSpawnNearCm      = 600.f;   // measure: the Octa spawns about this far from the walker, inside minSphere
constexpr float    kSpawnAwayCm      = 1300.f;  // measure: and at least this far from the other player, outside minSphere
constexpr uint64_t kStillMs          = 2000;    // measure: a puppet that has not moved for this long is in place
constexpr float    kStillCm          = 30.f;    // ... where "not moved" is less than this between two polls
// The walk-out ends this far from the Octa, outside minSphere: the first ring with a route, else the next.
constexpr float    kOutRingsCm[]     = {1800.f, 2400.f, 3000.f};
constexpr size_t   kOutRingCount     = sizeof(kOutRingsCm) / sizeof(kOutRingsCm[0]);
constexpr float    kSphereCm         = 1000.f;  // minSphere's radius
constexpr int      kStandpoints      = 8;
constexpr float    kStandReachCm     = 60.f;    // a standpoint is reached within this
constexpr int      kFirstWalkDeadlineS = 400;
constexpr int      kOutWalkDeadlineS   = 120;
constexpr uint32_t kBlastsWanted     = 3;       // the volleys the walker waits for, and the verdicts the host waits for
// One laser makes several explosions, which land within a tick of each other on loopback, and a re-fire follows about
// 1.8 s after. The client cannot see verdicts before the relay, so an explosion applied later than this after the
// previous one starts a new volley. It is above the 250 ms poll, so two explosions straddling a poll stay one volley.
constexpr uint64_t kVolleyGapMs      = 400;
constexpr uint64_t kAggroBoundMs     = 30000;   // HOST: no aggro this long after the spawn is a dead Octa
constexpr uint64_t kFireBoundMs      = 90000;   // CLIENT: this long with no new blast applied is a stalled wait
constexpr uint64_t kBirthsBoundMs    = 60000;   // births: the enrol, and either arm's mirror, within this
// The join arm: the joiner applies this many host verdicts after its join Aggro, then says so.
constexpr uint32_t kJoinVerdicts     = 1;
// The join arm's Octa is spawned this long before the joiner's world-ready, so its Aggro lands inside the Octa's pre-fire
// window (13.8 s) and ahead of any knockback; the joiner's load time is that of the witness, measured at its own join.
constexpr uint64_t kJoinLeadMs       = 8000;

// ---- the arm and its rows (octa_drill.cpp) ----

enum class Arm : uint8_t { Off, Births, Measure, Attack, Join };
Arm ArmOf();
// The arms that place a walker and let the Octa fire: measure (no relay judged), attack and join (the relay judged).
bool Fires();
// The arms the client's relay is judged on: attack, and join (where the relaunched client joins mid-attack).
bool RelayJudged();
// The join arm's observer (client 2): the joiner. Client 1 is a witness that stands away.
bool IsJoiner();
const char* FiringArmName();
// Where the measure and attack arms spawn the Octa (octa_spawn_route): the NPC spawn of the dev tool, or the gamemode's
// own spawn verb with the spawn menu's Octa row, whose Octa the host's EX catch enrols at the drain.
bool SpawnByGamemode();
// Who walks into the Octa's sphere: the host's own player in the join arm, else what the control row
// octa_measure_target says (the client, or the host's own player).
bool HostWalks();

// ---- the run's state ----

enum class HStep : uint8_t { Wait, Place, Timing, Spawning, Watch, Done };
enum class WStep : uint8_t { Stand, Out, Finished };   // the host's own legs after the spawn (octa_measure_target=host)
enum class CStep : uint8_t { Ready, Place, Find, Stand, Out, Done };
extern HStep g_hstep;
extern CStep g_cstep;

extern uint64_t g_nextMs;
extern uint64_t g_stepMs;            // the step began; a FAIL bound counts from here
extern int      g_invinc;            // the value startInvinc is put back to (-1: not held)
extern ue_wrap::CachedObjRef g_octa; // HOST: the Octa it spawned or the gamemode's spawn made
extern std::shared_ptr<D::BackgroundWalk> g_walk;

// HOST (measure)
extern GS::Counters g_base;          // the lane's counters when the Octa stood
extern uint64_t g_spawnMs;           // the Octa was spawned; the aggro bound counts from here
extern bool     g_aggroSaid;
extern bool     g_eidSaid;           // the Octa's NPC element id has been said (the gamemode route enrols it a tick late)
extern uint8_t  g_walkerSlot;        // the slot the Octa must aggro: the host's own (0), or the client's puppet's
extern FVector  g_puppetLast;        // the client puppet's place at the last poll, and since when it has not moved
extern uint64_t g_puppetStillAt;
extern WStep    g_wstep;             // octa_measure_target=host: the host's legs after the spawn
extern uint32_t g_volleysSeen;       // the host's own verdicts (one per fire) since the Octa was spawned, as last polled
extern bool     g_snapJudged;        // join, host: the joiner's snapshot has been judged (mid-attack, or between attacks)
// CLIENT (measure)
extern uint32_t g_appliedBase;       // the explosion lane's applied count when the Octa mirror appeared
extern uint32_t g_appliedSeen;
extern uint32_t g_volleys;           // the volleys applied since then
extern coop::geom_octa_relay::Counters g_relay0;   // attack: the relay's counters when the mirror appeared
extern uint32_t g_verdictsSeen;      // attack: the verdicts it has applied since
extern uint32_t g_laserSeen;         // attack: this client's own lasers counted so far
extern uint64_t g_laserPendingMs;    // attack: the stamp of a laser still waiting for the blast that follows it
extern uint64_t g_joinHoldAt;       // join, the host: when it began to wait for the joiner's chat line
extern bool     g_joinerSeen;        // join: the joiner (the observer) has said it is done
extern size_t   g_outRing;           // the walk-out ring in use (kOutRingsCm)
extern uint64_t g_lastAppliedMs;     // when the last explosion was seen applied
extern bool     g_calledEvent;       // CLIENT (births): the night roll's event has been called

// ---- helpers shared by the halves (octa_drill.cpp) ----

bool Due();
bool Expired(uint64_t bound);
unsigned long long Now();
bool Guard();
void Unguard();
void Fail(const char* what);   // the arm ends on a FAIL line; the guard goes back with it
void Go(CStep s);
bool DamageProbeOn();
void* FirstInstance(const wchar_t* className);
uint32_t NpcEidOf(void* actor);
float FlatDist(const FVector& a, const FVector& b);
void* LivePuppetBody(uint8_t& slot);
// The walker's way to its place, 2200 cm from the other player (kPlaceFromCm): one standpoint at a time, the next one
// when the walk ends with no route or the walker has not moved kPlaceMoveCm in kPlaceStuckMs, since the director itself
// never gives up. Called each tick until Reached; Failed after the last standpoint, said as the WALKER's failure.
enum class PlaceStep : uint8_t { Walking, Reached, Failed };
constexpr float    kPlaceMoveCm   = 100.f;
constexpr uint64_t kPlaceStuckMs  = 10000;
PlaceStep PlaceWalkTick(void* me, const FVector& about, const char* who);
// The join arm's chat line the joiner says when it has applied its verdicts, and its poll: the holder leaves on it.
constexpr const char* kJoinerDoneLine = "OCTAJOIN joiner done";
// How long the holder waits for the joiner (a relaunched process that boots, joins and replays): a cap on a wait that has
// stopped progressing, never a pace.
constexpr uint64_t kJoinWaitMs = 360000;
bool JoinerDone();
// The walkers' per-run state back to its start (the session's end).
void ResetWalkers();
// The join arm's host timing (octa_drill_host.cpp): the peers' admit and world-ready edges, back to their start.
void ResetJoinHost();
// The walker's way out of the sphere, ring by ring; false, said, once the last ring has failed.
bool BeginWalkOut(void* me, void* octa, const char* who);

// ---- the host's half (octa_drill_host.cpp) ----

void HostTick(coop::net::Session* s);

// ---- the births arm (octa_drill_births.cpp) ----

// The gamemode's own spawn verb with the spawn menu's Octa row, at a point; the measure and attack arms' gamemode route
// calls it too. False when the verb did not run.
bool CallGamemodeSpawn(const FVector& at);
// The births arm's host leg, after HostWait's checks: spawn by the gamemode's verb on a ring about the host.
void HostBirths(void* me);
void HostWatchBirths();
// The births arm's client legs: the night roll's event, called once (false, after Fail, if it did not run); and the
// PASS once the host's Octa has materialised as a mirror with `eid`.
bool ClientBirthsCall();
void ClientBirthsFound(uint32_t eid);

}  // namespace coop::dev::octa_drill::detail
