// coop/dev/fireext_drill_runaway.cpp -- the runaway arm of the fire extinguisher drill; see coop/dev/fireext_drill.h.
//
// Leg 1: the host's dropped extinguisher takes damage and runs away; the client's copy must show the thrust (its `active` and
// the host's charge at the thrust's end) and never run its own `shoot` (its thruster stays off), and the copies rest alike.
// Leg 2: a fresh extinguisher D is set on the floor before the client's pawn; the client's copy takes the damage, the client's
// own `shoot` is refused and the client asks the host, whose answer (fireext_sync::LastRunawayAnswer) and D's thruster the host
// reads at the client's "damaged" line, which carries the counts of `shoot` calls its copy ran and had cancelled inside the
// damage (a damage whose shoot was neither is no verdict of the cancel). Then the copies rest alike. Every verdict is the
// host's, decided at the line that carries what decides it.

#include "fireext_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_ground.h"
#include "coop/dev/rest_compare.h"
#include "coop/element/registry.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/fireext_sync.h"
#include "coop/props/fireext_thrust_sync.h"
#include "coop/props/prop_drive_host.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/fire_extinguisher.h"
#include "ue_wrap/actors/int_objects.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace coop::dev::fireext_drill::detail {

namespace E  = ue_wrap::engine;
namespace FX = ue_wrap::fire_extinguisher;
namespace PR = ue_wrap::prop;
namespace DC = coop::dev::drill_chat;
namespace DG = coop::dev::drill_ground;
namespace RC = coop::dev::rest_compare;
namespace FS = coop::fireext_sync;
namespace EL = coop::element;

constexpr float kDamage          = 10.f;   // above the 5 that a hit or an impact must exceed to start the runaway
constexpr int   kMarkTicks       = 180;    // still and not thrusting this long: the End has landed
constexpr int   kStartMaxTicks   = 300;    // the damage must start the thrust within this; INVALID only
constexpr int   kRunawayMaxTicks = 14400;  // a runaway thrusts as long as its charge lasts (a minute); INVALID only
// The game's runaway is a UPhysicsThrusterComponent (prop_fireExt: ThrustStrength = 50000 * charge, along the nozzle's
// reaction). Dropped upright, the thrust presses the body into the floor and it only hovers on it (measured: 3 cm of jitter and
// 7 cm of drift in a minute), so the host's extinguisher is turned upside down at the damage and the thrust lifts it. The leg
// then proves the host's Claim stream: the copy must have flown kFlightMinCm while thrusting, else the leg is INVALID.
constexpr float kFlightMinCm  = 300.f;
constexpr float kPlaceUpCm    = 200.f;     // above the host's pawn: clear of the mount's box, its capsule, the floor and the body-push
                                           // poll's 150 cm reach, so no body's push streams the flight the claim is to stream
constexpr int   kFallenMaxTicks = 900;     // the dropped extinguisher must have left its mount and rested by then; INVALID only
constexpr float kFlightStepCm = 2.f;       // a tick's move that counts toward the flight, so a hover's jitter does not
constexpr float kChargeTol    = 0.05f;     // the client copy's charge at the thrust's end against the host's
constexpr float kBeforeClientCm = 200.f;   // leg 2's extinguisher D is set this far before the client's pawn, on the floor
constexpr float kDropCm       = 15.f;      // and let go this high over it: a fall from the pawn's height starts its runaway
constexpr float kAskReachCm   = 800.f;     // and the client must be within this of D, inside the host's reach for an ask
const char* const kRunTag = "fireext-runaway";
const std::string kDamagedPrefix = "fireext damaged ";
const std::string kRestedPrefix  = "fireext rested ";       // "<active seen 0|1> <charge at the host's thrust end> <pose>"
const std::string kHostRestedLine = "fireext hostrested";
const std::string kClientThrustLine = "fireext clientthrust";   // the client's own copy's thruster read on: its own shoot ran
const std::string kLeg2Prefix = "fireext dd ";                 // the host's D: its element id
const std::string kDamaged2Prefix = "fireext damaged2 ";       // the client's damage on D: "<shoot calls run> <shoot calls cancelled> <thruster 0|1>"
const std::string kHostRested2Line = "fireext hostrested2";
const std::string kRested2Prefix = "fireext rested2 ";         // "<pose>"

constexpr const wchar_t* kExtCls  = L"prop_fireExt_C";
constexpr const wchar_t* kExtPath = L"/Game/objects/prop_fireExt.prop_fireExt_C";

DG::Settle g_runRest;
float g_flightCm = 0.f;            // the host: the path its copy flew while thrusting
bool g_flightHasPrev = false;
bool g_placed = false;             // the host: the extinguisher stands clear of its mount, its thrust pointing up
ue_wrap::FVector g_flightPrev{};
bool g_sawThrust = false;          // the host: it read the extinguisher thrusting
bool g_offSampled = false;         // the host: its charge at the thrust's end is read
float g_hostOffCharge = -1.f;
int  g_runTicks = 0;
bool g_leg2 = false;               // the host: the first leg passed, the client's own damage is being judged
bool g_dSpawned = false;
ue_wrap::CachedObjRef g_d;         // the host's D
uint32_t g_dEid = 0;
RC::Pose g_hostRunRest;

// The client's stages: wait for the host's damaged line, for its flight to rest, rest here, wait for D, the damage and the
// host's second rest, rest here again.
enum class CS { WaitDamaged, WaitHostRest, Settle, WaitDD, WaitHostRest2, Settle2, Done };
CS g_cs = CS::WaitDamaged;
bool g_cliArmed = false;
ue_wrap::CachedObjRef g_runCopy;
uint32_t g_runEid = 0;
bool g_sawActive = false;          // the client: its copy read `active` while the host's thrust ran (the display)
bool g_clientThrustSaid = false;
bool g_sawOnMark = false;
float g_offCharge = -1.f;          // the charge at the marker of the host's thrust-off applied here
uint32_t g_markSeq = 0;

// The client red arm (fireext_cancel_off): a client copy is not refused its own shoot, which only the client-damage leg can show.
// A host leg that rests apart first is no failure of the lever, so that leg says INVALID in this arm. Resolved once.
bool CancelArm() {
    static const bool on = coop::config::ResolveFlag(coop::config_registry::rows::fireext_cancel_off);
    return on;
}

std::string Ascii(const std::wstring& w) {
    std::string a;
    for (const wchar_t c : w) a.push_back(c < 128 ? static_cast<char>(c) : '?');
    return a;
}

uint32_t EidOf(void* actor) {
    const EL::ElementId eid = EL::Registry::Get().EidForActor(actor);
    return eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid);
}

// The thruster's force is along its own -X axis (UPhysicsThrusterComponent), so the body is turned until that axis points up: of
// the rotations about the body's pitch and roll in quarter turns, the one whose thruster forward points most downward. The
// axis read before and after is logged, the measure the leg stands on.
void OrientThrustUp(void* t, float yaw) {
    constexpr float kDeg[4] = {0.f, 90.f, 180.f, -90.f};
    constexpr float kRad = 3.14159265f / 180.f;
    ue_wrap::FRotator best{0.f, yaw, 0.f};
    float bestZ = 2.f;
    ue_wrap::FRotator thr{};
    if (FX::ReadComponentRotation(t, L"PhysicsThruster", thr))
        UE_LOGI("[FIREEXT-DRILL] [H] the thruster stands at pitch %.1f yaw %.1f roll %.1f as dropped: forward z %+.2f",
                thr.Pitch, thr.Yaw, thr.Roll, std::sin(thr.Pitch * kRad));
    for (float p : kDeg) {
        for (float r : kDeg) {
            const ue_wrap::FRotator cand{p, yaw, r};
            E::SetActorRotation(t, cand);
            if (!FX::ReadComponentRotation(t, L"PhysicsThruster", thr)) continue;
            const float fz = std::sin(thr.Pitch * kRad);   // the thruster's forward z; the thrust is its negative
            if (fz < bestZ) { bestZ = fz; best = cand; }
        }
    }
    E::SetActorRotation(t, best);
    if (FX::ReadComponentRotation(t, L"PhysicsThruster", thr)) {
        UE_LOGI("[FIREEXT-DRILL] [H] body turned to pitch %.0f roll %.0f: the thruster forward z %+.2f, so the thrust points %s (z %+.2f)",
                best.Pitch, best.Roll, bestZ, bestZ < -0.5f ? "UP" : "not up", -bestZ);
    } else {
        UE_LOGW("[FIREEXT-DRILL] [H] the thruster's rotation could not be read; the body is left at pitch %.0f roll %.0f",
                best.Pitch, best.Roll);
    }
}

namespace {

// Leg 2's first steps on the host: D is set on the floor before the client's pawn (a fall over it would start its runaway by
// itself), rests, is checked not to have gone off and is named to the client.
void Leg2Spawn() {
    if (!g_dSpawned) {
        void* const puppet = coop::puppet_drive::Puppet(1).GetActor();
        void* const me = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        void* const cls = ue_wrap::object_index::ClassByName(kExtCls);
        void* const c = cls ? cls : ue_wrap::asset_load::LoadObjectByPath(kExtPath);
        if (!puppet || !me || !E::TryGetActorLocation(puppet, at)) return Invalid("the client's pawn could not be read to stand D before it");
        const ue_wrap::FVector fwd = E::GetActorForwardVector(puppet);
        const ue_wrap::FVector ahead{at.X + fwd.X * kBeforeClientCm, at.Y + fwd.Y * kBeforeClientCm, at.Z};
        ue_wrap::trace::Hit floor;
        if (!ue_wrap::trace::LineHitStatDyn(me, ue_wrap::FVector{ahead.X, ahead.Y, ahead.Z + 100.f},
                                            ue_wrap::FVector{ahead.X, ahead.Y, ahead.Z - 300.f}, &floor) || !floor.blocked)
            return Invalid("no floor before the client's pawn to set D on");
        void* const d = c ? E::SpawnActor(c, ue_wrap::FVector{ahead.X, ahead.Y, floor.point.Z + kDropCm}) : nullptr;
        if (!d) return Invalid("the world refused D's spawn before the client");
        g_d.Set(d);
        g_dSpawned = true;
        g_runTicks = 0;
        DG::Restart(g_runRest);
        UE_LOGI("[FIREEXT-DRILL] [H] spawned D %.0f cm before the client's pawn, %.0f cm over the floor", kBeforeClientCm, kDropCm);
        return;
    }
    void* const d = g_d.Get();
    ue_wrap::FVector at{};
    if (!d || !E::TryGetActorLocation(d, at)) return Invalid("D died or could not be read before the damage");
    ++g_runTicks;
    bool active = false;
    FX::ReadThrusting(d, active);
    if (active) return Invalid("D went off by itself before the client's damage");
    g_dEid = EidOf(d);
    if (!g_dEid || coop::prop_drive_host::IsDriven(d) || !DG::Stable(g_runRest, at, DG::Quiet(d))) {
        if (g_runTicks > kFallenMaxTicks) Invalid("D never got an element id and came to rest");
        return;
    }
    void* const puppet = coop::puppet_drive::Puppet(1).GetActor();
    ue_wrap::FVector client{};
    if (!puppet || !E::TryGetActorLocation(puppet, client)) return Invalid("the client's pawn could not be read to check its reach of D");
    const float away = DG::Dist(at, client);
    if (away > kAskReachCm) {
        char why[160];
        std::snprintf(why, sizeof(why), "D came to rest %.0f cm from the client, past the %.0f the ask is built for", away, kAskReachCm);
        return Invalid(why);
    }
    DC::Say(kLeg2Prefix + std::to_string(g_dEid));
    UE_LOGI("[FIREEXT-DRILL] [H] D (eid %u) rests %.0f cm from the client, not thrusting", g_dEid, away);
    g_runTicks = 0;
    Go(Step::Leg2Wait);
}

// Leg 2 on the host, at the client's "damaged" line, which carries how many `shoot` calls its copy ran inside its damage and
// its thruster read: that count first, then the host's answer to the ask, then D's thruster.
void Leg2Decide() {
    void* const d = g_d.Get();
    if (!d) return Invalid("D died before the client's damage was judged");
    std::string rest;
    ++g_runTicks;
    if (!DC::SeenRestOf(kDamaged2Prefix, &rest)) {
        if (g_runTicks > kRunawayMaxTicks) Invalid("the client's damaged line never came");
        return;
    }
    namespace RV = coop::rig_verdict;
    unsigned ran = 0, cancelled = 0, clientThruster = 0;
    if (sscanf_s(rest.c_str(), "%u %u %u", &ran, &cancelled, &clientThruster) != 3) return Invalid("the client's damaged line could not be read");
    const char* const answer = FS::LastRunawayAnswer(g_dEid);
    bool thruster = false;
    FX::ReadThrusterOn(d, thruster);
    UE_LOGI("[FIREEXT-DRILL] [H] the client's damaged line: its copy ran %u and cancelled %u shoot call(s), its thruster read %u; the "
            "host's answer %s, D's thruster %d", ran, cancelled, clientThruster, answer ? answer : "(none)", thruster ? 1 : 0);
    if (ran > 0) {
        RV::Say(kRunTag, RV::Outcome::Fail, "a client copy thrust: its own shoot ran %u time(s) inside its damage", ran);
    } else if (cancelled == 0) {
        RV::Say(kRunTag, RV::Outcome::Fail, "the client copy's shoot was neither cancelled nor run");
    } else if (!answer) {
        RV::Say(kRunTag, RV::Outcome::Fail, "the client never asked: its copy took the damage and the host heard nothing");
    } else if (std::strcmp(answer, "applied") == 0) {
        if (!thruster) {
            RV::Say(kRunTag, RV::Outcome::Fail, "the host answered the ask as applied and D's thruster is still off");
            Go(Step::Done);
            return;
        }
        UE_LOGI("[FIREEXT-DRILL] [H] the client's damage was asked and answered (applied): D's thruster is on");
        g_target.Set(d);
        g_targetKey = L"D";
        g_runTicks = 0;
        g_flightCm = 0.f;
        g_flightHasPrev = false;
        g_sawThrust = false;
        g_offSampled = false;
        DG::Restart(g_runRest);
        Go(Step::RunRest);
        return;
    } else if (std::strcmp(answer, "alreadyActive") == 0 || std::strcmp(answer, "notExtinguisher") == 0) {
        RV::Say(kRunTag, RV::Outcome::Fail, "the host refused the ask as %s, for an extinguisher the drill had just spawned", answer);
    } else {
        Invalid(answer);   // the fixture's own reason, or a verb that did not run: a budget, a joiner not ready, a reach, a body, verbFailed
        return;
    }
    Go(Step::Done);
}

}  // namespace

// The host: damage the dropped extinguisher through its own addDamage, wait for the runaway to start and end and the copy to
// rest, then judge it against the client's rest pose; then leg 2.
void RunawayHostStep(void* player) {
    if (g_step == Step::Leg2Spawn) return Leg2Spawn();
    if (g_step == Step::Leg2Wait) return Leg2Decide();
    void* t = g_target.Get();
    if (!t) { Invalid("the extinguisher died during the runaway arm"); return; }
    ++g_runTicks;
    namespace RV = coop::rig_verdict;
    if (!g_leg2 && DC::SeenHas(kClientThrustLine)) {
        RV::Say(kRunTag, RV::Outcome::Fail, "a client copy thrust: its own copy's thruster read on while the host's thrust ran");
        Go(Step::Done);
        return;
    }
    if (g_step == Step::Damage) {
        if (coop::director::Grabbing(player) == t) return;   // the hand has not let go yet
        ue_wrap::FVector at{};
        ue_wrap::FRotator rot{};
        if (!E::TryGetActorLocation(t, at) || !E::TryGetActorRotation(t, rot)) {
            Invalid("the extinguisher's place could not be read before the damage");
            return;
        }
        if (!g_placed) {
            // A turned extinguisher left at its mount is taken back by the mount (measured: it froze there and the thrust ran
            // for a minute on a static body), so it first falls off, then is stood clear of it, thrust up. The let-go's own
            // drive row must have ended too (its End is the event): a runaway flown while it stands is streamed by that row,
            // and the host's claim, the half this leg proves, would carry nothing.
            if (PR::IsFrozen(t) || !DG::Quiet(t) || coop::prop_drive_host::IsDriven(t)) {
                if (g_runTicks > kFallenMaxTicks) Invalid("the dropped extinguisher never left its mount, rested and ended its let-go drive");
                return;
            }
            void* me = coop::players::Registry::Get().Local();
            ue_wrap::FVector pawn{};
            if (!me || !E::TryGetActorLocation(me, pawn)) { Invalid("the host's pawn could not be read to place the extinguisher"); return; }
            E::SetActorLocation(t, ue_wrap::FVector{pawn.X, pawn.Y, pawn.Z + kPlaceUpCm});
            OrientThrustUp(t, rot.Yaw);
            g_placed = true;
            return;
        }
        if (!ue_wrap::int_objects::InvokeAddDamage(t, nullptr, kDamage, at, ue_wrap::FVector{})) {
            Invalid("the extinguisher's addDamage did not run");
            return;
        }
        UE_LOGI("[FIREEXT-DRILL] [H] DAMAGED key='%ls': addDamage(%.0f) ran", g_targetKey.c_str(), kDamage);
        DC::Say(kDamagedPrefix + Ascii(g_targetKey));
        g_runTicks = 0;
        g_flightCm = 0.f;
        g_flightHasPrev = false;
        DG::Restart(g_runRest);
        Go(Step::RunRest);
        return;
    }
    if (g_step == Step::RunRest) {
        bool th = false;
        FX::ReadThrusting(t, th);
        if (th) g_sawThrust = true;
        // The game leaves the thrusting flag set when the charge runs out (measured: set for the minute after the thrust
        // was spent), so a thrust that flies the body is the flag with charge left.
        float charge = 0.f;
        const bool flying = th && FX::ReadCharge(t, charge) && charge > 0.f;
        if (g_sawThrust && !flying && !g_offSampled) {
            g_offSampled = true;
            g_hostOffCharge = std::fmax(charge, 0.f);
            UE_LOGI("[FIREEXT-DRILL] [H] the host's thrust ended: its charge %.4f", g_hostOffCharge);
        }
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(t, at)) { Invalid("the extinguisher's place could not be read in the runaway"); return; }
        if (!g_sawThrust) {
            if (g_runTicks > kStartMaxTicks) Invalid("the damage did not start the runaway thrust");
            return;
        }
        if (flying && g_flightHasPrev) {
            const float dx = at.X - g_flightPrev.X, dy = at.Y - g_flightPrev.Y, dz = at.Z - g_flightPrev.Z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d > kFlightStepCm) g_flightCm += d;
        }
        g_flightPrev = at;
        g_flightHasPrev = true;
        if (!DG::Stable(g_runRest, at, DG::Quiet(t) && !flying, kMarkTicks)) {
            if (g_runTicks > kRunawayMaxTicks) Invalid("the runaway never came to rest");
            return;
        }
        if (!RC::Read(t, g_hostRunRest)) { Invalid("the extinguisher's pose could not be read at its rest"); return; }
        UE_LOGI("[FIREEXT-DRILL] [H] the runaway flew %.0f cm while thrusting", g_flightCm);
        if (!g_leg2 && g_flightCm < kFlightMinCm) {
            char why[160];
            std::snprintf(why, sizeof(why), "the damaged extinguisher flew %.0f cm while thrusting, under the %.0f the leg needs",
                          g_flightCm, kFlightMinCm);
            Invalid(why);
            return;
        }
        LogTarget("RESTED");
        DC::Say(g_leg2 ? kHostRested2Line : kHostRestedLine);
        g_runTicks = 0;
        Go(Step::WaitClient);
        return;
    }
    std::string rest;   // WaitClient: leg 1 "<active seen 0|1> <charge> <pose>", leg 2 "<pose>"
    if (!DC::SeenRestOf(g_leg2 ? kRested2Prefix : kRestedPrefix, &rest)) {
        if (g_runTicks > kRunawayMaxTicks) Invalid("the client's rested line never came");
        return;
    }
    RC::Pose client;
    if (g_leg2) {
        RC::Pose hostNow;
        if (!RC::Decode(rest, client)) { Invalid("the client's rested line could not be read"); return; }
        if (!RC::Read(t, hostNow)) { Invalid("D's pose could not be read after the client's damage"); return; }
        if (RC::Judge(kRunTag, "D after the client's own damage", hostNow, client))
            RV::Say(kRunTag, RV::Outcome::Pass, "the client's damage asked the host, the host's runaway thrust showed on the client "
                    "and D's copies rested within %.0f cm and %.0f deg, with no client copy thrust", RC::kBoundCm, RC::kBoundDeg);
        Go(Step::Done);
        return;
    }
    int active = 0;
    float offCharge = -1.f;
    int used = 0;
    if (sscanf_s(rest.c_str(), "%d %f %n", &active, &offCharge, &used) != 2 || used <= 0 || !RC::Decode(rest.substr(static_cast<size_t>(used)), client)) {
        Invalid("the client's rested line could not be read");
        return;
    }
    if (active != 1) {
        RV::Say(kRunTag, RV::Outcome::Fail, "the client's copy never showed the host's thrust: it never read active while the host's thrust ran");
        Go(Step::Done);
        return;
    }
    if (offCharge < 0.f || std::fabs(offCharge - g_hostOffCharge) > kChargeTol) {
        RV::Say(kRunTag, RV::Outcome::Fail, "the client's copy took %s of the host's charge at the thrust's end (%.4f against %.4f)",
                offCharge < 0.f ? "no word" : "a different count", offCharge, g_hostOffCharge);
        Go(Step::Done);
        return;
    }
    if (!RC::Judge(kRunTag, "the runaway extinguisher", g_hostRunRest, client, /*sayFail=*/!CancelArm())) {
        if (CancelArm()) Invalid("red arm fireext_cancel_off: the host's runaway rested apart in leg 1, so the client-damage leg this red is built to fail was not reached");
        Go(Step::Done);
        return;
    }
    // Leg 1 passed. Leg 2: D stands over the client, whose copy takes the damage.
    g_leg2 = true;
    g_runTicks = 0;
    UE_LOGI("[FIREEXT-DRILL] [H] leg 1 held; a fresh extinguisher is stood over the client");
    Go(Step::Leg2Spawn);
}

namespace {

// What the client reads of its copy while the host's thrust runs: `active` (the display), its own thruster (its own shoot), and
// the marker of the host's thrust end applied here.
void TrackCopy(void* o) {
    bool th = false, thr = false;
    if (FX::ReadThrusting(o, th) && th) g_sawActive = true;
    if (!g_clientThrustSaid && FX::ReadThrusterOn(o, thr) && thr) {
        g_clientThrustSaid = true;
        UE_LOGW("[FIREEXT-DRILL] [C] this client's own copy's thruster reads on: its own shoot ran");
        DC::Say(kClientThrustLine);
    }
    const FS::Applied a = FS::LastThrustApplied(g_runEid);
    if (a.seq == g_markSeq) return;
    g_markSeq = a.seq;
    if (a.on) g_sawOnMark = true;
    else if (g_sawOnMark) g_offCharge = a.charge;
}

void ClientInvalid(const char* why) {
    UE_LOGW("[FIREEXT-DRILL] [C] INVALID %s", why);
    coop::rig_verdict::Say(kRunTag, coop::rig_verdict::Outcome::Invalid, "%s", why);
    g_cs = CS::Done;
}

// True once the copy has rested not thrusting: the pose to say.
bool Rested(void* o, RC::Pose& pose) {
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(o, at)) { ClientInvalid("this client's copy died during the runaway"); return false; }
    bool th = false;
    float charge = 0.f;
    FX::ReadThrusting(o, th);
    const bool flying = th && FX::ReadCharge(o, charge) && charge > 0.f;   // the flag stays set after the charge is spent
    if (!DG::Stable(g_runRest, at, DG::Quiet(o) && !flying, kMarkTicks)) {
        if (g_runTicks > kRunawayMaxTicks) ClientInvalid("this client's copy never rested");
        return false;
    }
    if (!RC::Read(o, pose)) { ClientInvalid("this client's copy's pose could not be read"); return false; }
    return true;
}

}  // namespace

// The client's copy: wait for the host's damaged line, then for the copy to rest, and say what it read of the host's thrust and
// its pose; then, at the host's D line, take D's damage in its own copy, say what its thruster read in that tick, and say the
// rest.
void RunawayClientTick() {
    if (!g_cliArmed) {
        g_cliArmed = true;
        DC::SeenHas(kDamagedPrefix);   // the first read starts the chat history
    }
    if (g_cs == CS::Done) return;
    ++g_runTicks;
    void* o = g_runCopy.Get();
    if (g_cs == CS::WaitDamaged) {
        std::string key;
        if (!DC::SeenRestOf(kDamagedPrefix, &key)) return;
        for (auto& w : g_watched)
            if (!w.gone && Ascii(w.key) == key) g_runCopy.Set(w.ref.Get());
        if (!g_runCopy.Raw()) { ClientInvalid(("this client has no copy of key " + key).c_str()); return; }
        g_runEid = EidOf(g_runCopy.Get());
        // A marker prints on an `on` change only, so the thrust's start may already be applied when the host's line arrives:
        // what the copy shows at arming is the first thing seen.
        const FS::Applied armed = FS::LastThrustApplied(g_runEid);
        g_markSeq = armed.seq;
        g_sawOnMark = armed.on;
        g_runTicks = 0;
        DG::Restart(g_runRest);
        g_cs = CS::WaitHostRest;
        return;
    }
    if (g_cs == CS::WaitDD) {
        std::string rest;
        if (!DC::SeenRestOf(kLeg2Prefix, &rest)) {
            if (g_runTicks > kRunawayMaxTicks) ClientInvalid("the host's D line never came");
            return;
        }
        g_runEid = static_cast<uint32_t>(std::strtoul(rest.c_str(), nullptr, 10));
        void* const d = EL::LiveActorOfType(static_cast<EL::ElementId>(g_runEid), EL::ElementType::Prop);
        ue_wrap::FVector at{};
        if (!d || !DG::Quiet(d) || !E::TryGetActorLocation(d, at)) {
            if (g_runTicks > kRunawayMaxTicks) ClientInvalid("this client's copy of D never came to rest");
            return;
        }
        // A copy that thrusts before the damage went off by itself, and no verdict of the ask can be read from it.
        bool activeBefore = false;
        FX::ReadThrusting(d, activeBefore);
        if (activeBefore) {
            ClientInvalid("D went off by itself before this client's damage");
            return;
        }
        // The client's own damage: the `shoot` calls its copy ran inside it are the count the host judges (a thruster read
        // after the call cannot say whether `shoot` ran); the thruster is read in the same tick as a logged extra.
        const uint32_t ranBefore = coop::fireext_thrust_sync::ClientShootsRun();
        const uint32_t cancelledBefore = coop::fireext_thrust_sync::ClientShootsCancelled();
        if (!ue_wrap::int_objects::InvokeAddDamage(d, nullptr, kDamage, at, ue_wrap::FVector{})) {
            ClientInvalid("this client copy's addDamage did not run");
            return;
        }
        const uint32_t ran = coop::fireext_thrust_sync::ClientShootsRun() - ranBefore;
        const uint32_t cancelled = coop::fireext_thrust_sync::ClientShootsCancelled() - cancelledBefore;
        bool thruster = false;
        FX::ReadThrusterOn(d, thruster);
        UE_LOGI("[FIREEXT-DRILL] [C] DAMAGED this client's own copy of D (eid %u): addDamage(%.0f) ran, %u shoot call(s) ran and %u "
                "were cancelled inside it, its thruster reads %d", g_runEid, kDamage, ran, cancelled, thruster ? 1 : 0);
        DC::Say(kDamaged2Prefix + std::to_string(ran) + " " + std::to_string(cancelled) + " " + (thruster ? "1" : "0"));
        g_runCopy.Set(d);
        g_runTicks = 0;
        DG::Restart(g_runRest);
        g_cs = CS::WaitHostRest2;
        return;
    }
    if (!o) { ClientInvalid("this client's copy died during the runaway"); return; }
    if (g_cs == CS::WaitHostRest) {   // the host's flight is not over: this copy is marked only after the host's rest
        TrackCopy(o);
        if (DC::SeenHas(kHostRestedLine)) {
            g_runTicks = 0;
            DG::Restart(g_runRest);
            g_cs = CS::Settle;
        } else if (g_runTicks > kRunawayMaxTicks) {
            ClientInvalid("the host's rest line never came");
        }
        return;
    }
    if (g_cs == CS::WaitHostRest2) {
        if (DC::SeenHas(kHostRested2Line)) {
            g_runTicks = 0;
            DG::Restart(g_runRest);
            g_cs = CS::Settle2;
        } else if (g_runTicks > kRunawayMaxTicks) {
            ClientInvalid("the host's second rest line never came");
        }
        return;
    }
    RC::Pose pose;
    if (g_cs == CS::Settle) {
        TrackCopy(o);
        if (!Rested(o, pose)) return;
        UE_LOGI("[FIREEXT-DRILL] [C] RESTED at (%.1f, %.1f, %.1f); active seen here: %d, the thrust end's charge %.4f", pose.loc.X,
                pose.loc.Y, pose.loc.Z, g_sawActive ? 1 : 0, g_offCharge);
        char head[48];
        std::snprintf(head, sizeof(head), "%d %.4f ", g_sawActive ? 1 : 0, g_offCharge);
        DC::Say(kRestedPrefix + head + RC::Encode(pose));
        g_runTicks = 0;
        g_cs = CS::WaitDD;
        return;
    }
    if (g_cs == CS::Settle2) {
        if (!Rested(o, pose)) return;
        UE_LOGI("[FIREEXT-DRILL] [C] D RESTED at (%.1f, %.1f, %.1f)", pose.loc.X, pose.loc.Y, pose.loc.Z);
        DC::Say(kRested2Prefix + RC::Encode(pose));
        g_cs = CS::Done;
    }
}

void RunawayReset() {
    g_runCopy.Reset();
    g_d.Reset();
    g_dSpawned = false;
    g_dEid = 0;
    g_placed = false;
    g_flightCm = 0.f;
    g_flightHasPrev = false;
    DG::Restart(g_runRest);
    g_sawThrust = false;
    g_offSampled = false;
    g_hostOffCharge = -1.f;
    g_runTicks = 0;
    g_cs = CS::WaitDamaged;
    g_cliArmed = false;
    g_leg2 = false;
    g_runEid = 0;
    g_sawActive = g_clientThrustSaid = g_sawOnMark = false;
    g_offCharge = -1.f;
    g_markSeq = 0;
}

}  // namespace coop::dev::fireext_drill::detail
