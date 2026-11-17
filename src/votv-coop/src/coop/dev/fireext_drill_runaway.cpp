// coop/dev/fireext_drill_runaway.cpp -- the runaway arm of the fire extinguisher drill; see coop/dev/fireext_drill.h.

#include "fireext_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_ground.h"
#include "coop/dev/rest_compare.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_drive_host.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/fire_extinguisher.h"
#include "ue_wrap/actors/int_objects.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace coop::dev::fireext_drill::detail {

namespace E  = ue_wrap::engine;
namespace FX = ue_wrap::fire_extinguisher;
namespace PR = ue_wrap::prop;
namespace DC = coop::dev::drill_chat;
namespace DG = coop::dev::drill_ground;
namespace RC = coop::dev::rest_compare;

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
const char* const kRunTag = "fireext-runaway";
const std::string kDamagedPrefix = "fireext damaged ";
const std::string kRestedPrefix  = "fireext rested ";
const std::string kHostRestedLine = "fireext hostrested";
// The second leg: the CLIENT's copy takes the damage locally. Its own shoot is cancelled (coop/props/fireext_thrust_sync), so
// it must not thrust, and the host's copy, which was not damaged, must stay where it rests: the two stay together. Relaying a
// client's damage to the host, so that the host's copy runs away too, is the extinguisher lane's own change (#134, row 11),
// not this one.
const std::string kClientDamageLine = "fireext clientdamage";
const std::string kRested2Prefix = "fireext rested2 ";   // the host's copy rests: the client marks its own after this

DG::Settle g_runRest;
float g_flightCm = 0.f;            // the host: the path its copy flew while thrusting
bool g_flightHasPrev = false;
bool g_placed = false;             // the host: the extinguisher stands clear of its mount, its thrust pointing up
ue_wrap::FVector g_flightPrev{};
bool g_sawThrust = false;          // this peer read the extinguisher thrusting
int  g_runTicks = 0;
int  g_cliStage = 0;               // the client's copy: 0 waits for the damaged line, 1 for the host's rest, 3 settles, 4 waits
                                   // for the client-damage line, 5 settles after its own damage, 2 done
bool g_cliArmed = false;
bool g_leg2 = false;                // the host: the first leg passed, the client's own damage is being judged
ue_wrap::CachedObjRef g_runCopy;
RC::Pose g_hostRunRest;

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

// The host: damage the dropped extinguisher through its own addDamage, wait for the runaway to start and end and the copy to
// rest, then judge it against the client's rest pose.
void RunawayHostStep(void* player) {
    void* t = g_target.Get();
    if (!t) { Invalid("the extinguisher died during the runaway arm"); return; }
    ++g_runTicks;
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
        if (g_flightCm < kFlightMinCm) {
            char why[160];
            std::snprintf(why, sizeof(why), "the damaged extinguisher flew %.0f cm while thrusting, under the %.0f the leg needs",
                          g_flightCm, kFlightMinCm);
            Invalid(why);
            return;
        }
        LogTarget("RESTED");
        DC::Say(kHostRestedLine);
        g_runTicks = 0;
        Go(Step::WaitClient);
        return;
    }
    std::string rest;   // WaitClient: "<thrust seen 0|1> <pose>"
    if (!DC::SeenRestOf(g_leg2 ? kRested2Prefix : kRestedPrefix, &rest)) {
        if (g_runTicks > kRunawayMaxTicks) Invalid("the client's rested line never came");
        return;
    }
    RC::Pose client;
    if (rest.size() < 3 || rest[1] != ' ' || !RC::Decode(rest.substr(2), client)) {
        Invalid("the client's rested line could not be read");
        return;
    }
    namespace RV = coop::rig_verdict;
    if (rest[0] == '1') {
        RV::Say(kRunTag, RV::Outcome::Fail, "a client copy's shoot ran (%s): its runaway thrust was not cancelled",
                g_leg2 ? "its own damage" : "the host's damage");
        Go(Step::Done);
        return;
    }
    if (!g_leg2) {
        if (!RC::Judge(kRunTag, "the runaway extinguisher", g_hostRunRest, client, /*sayFail=*/!CancelArm())) {
            if (CancelArm()) Invalid("red arm fireext_cancel_off: the host's runaway rested apart in leg 1, so the client-damage leg this red is built to fail was not reached");
            Go(Step::Done);
            return;
        }
        // Leg 1 passed. Leg 2: the client's copy takes the damage itself.
        g_leg2 = true;
        g_runTicks = 0;
        DC::Say(kClientDamageLine);
        UE_LOGI("[FIREEXT-DRILL] [H] leg 1 held; the client's copy is now damaged locally");
        return;
    }
    RC::Pose hostNow;
    if (!RC::Read(t, hostNow)) { Invalid("the host's copy's pose could not be read after the client's damage"); return; }
    if (RC::Judge(kRunTag, "after the client's copy took damage locally", hostNow, client))
        RV::Say(kRunTag, RV::Outcome::Pass, "the host's runaway and the client's own damage left the copies within %.0f cm and "
                "%.0f deg, and no client copy thrust", RC::kBoundCm, RC::kBoundDeg);
    Go(Step::Done);
}

// The client's copy: wait for the host's damaged line, then for the copy to rest not thrusting, and say its pose and whether
// it ever read thrusting.
void RunawayClientTick() {
    if (!g_cliArmed) {
        g_cliArmed = true;
        DC::SeenHas(kDamagedPrefix);   // the first read starts the chat history
    }
    if (g_cliStage == 2) return;
    ++g_runTicks;
    if (g_cliStage == 0) {
        std::string key;
        if (!DC::SeenRestOf(kDamagedPrefix, &key)) return;
        for (auto& w : g_watched)
            if (!w.gone && Ascii(w.key) == key) g_runCopy.Set(w.ref.Get());
        if (!g_runCopy.Raw()) { UE_LOGW("[FIREEXT-DRILL] [C] INVALID no extinguisher of key '%s' here", key.c_str());
            coop::rig_verdict::Say(kRunTag, coop::rig_verdict::Outcome::Invalid, "this client has no copy of key %s", key.c_str());
            g_cliStage = 2; return; }
        g_runTicks = 0;
        DG::Restart(g_runRest);
        g_cliStage = 1;
        return;
    }
    void* o = g_runCopy.Get();
    ue_wrap::FVector at{};
    bool th = false;
    if (!o || !E::TryGetActorLocation(o, at)) {
        coop::rig_verdict::Say(kRunTag, coop::rig_verdict::Outcome::Invalid, "this client's copy died during the runaway");
        g_cliStage = 2;
        return;
    }
    FX::ReadThrusting(o, th);
    if (th) g_sawThrust = true;
    if (g_cliStage == 4) {   // leg 2: when the host says so, this copy takes the damage itself
        if (DC::SeenHas(kClientDamageLine)) {
            g_sawThrust = false;
            if (!ue_wrap::int_objects::InvokeAddDamage(o, nullptr, kDamage, at, ue_wrap::FVector{})) {
                coop::rig_verdict::Say(kRunTag, coop::rig_verdict::Outcome::Invalid, "this client copy's addDamage did not run");
                g_cliStage = 2;
                return;
            }
            UE_LOGI("[FIREEXT-DRILL] [C] DAMAGED this client's own copy: addDamage(%.0f) ran", kDamage);
            g_runTicks = 0;
            DG::Restart(g_runRest);
            g_cliStage = 5;
        } else if (g_runTicks > kRunawayMaxTicks) {
            coop::rig_verdict::Say(kRunTag, coop::rig_verdict::Outcome::Invalid, "the host's client-damage line never came");
            g_cliStage = 2;
        }
        return;
    }
    if (g_cliStage == 1) {   // the host's flight is not over: this copy is marked only after the host's rest
        if (DC::SeenHas(kHostRestedLine)) {
            g_runTicks = 0;
            DG::Restart(g_runRest);
            g_cliStage = 3;
        } else if (g_runTicks > kRunawayMaxTicks) {
            coop::rig_verdict::Say(kRunTag, coop::rig_verdict::Outcome::Invalid, "the host's rest line never came");
            g_cliStage = 2;
        }
        return;
    }
    float charge = 0.f;
    const bool flying = th && FX::ReadCharge(o, charge) && charge > 0.f;   // the flag stays set after the charge is spent
    if (!DG::Stable(g_runRest, at, DG::Quiet(o) && !flying, kMarkTicks)) {
        if (g_runTicks > kRunawayMaxTicks) {
            coop::rig_verdict::Say(kRunTag, coop::rig_verdict::Outcome::Invalid, "this client's copy never rested");
            g_cliStage = 2;
        }
        return;
    }
    RC::Pose pose;
    if (!RC::Read(o, pose)) {
        coop::rig_verdict::Say(kRunTag, coop::rig_verdict::Outcome::Invalid, "this client's copy's pose could not be read");
        g_cliStage = 2;
        return;
    }
    UE_LOGI("[FIREEXT-DRILL] [C] RESTED at (%.1f, %.1f, %.1f); thrust seen here: %d", pose.loc.X, pose.loc.Y, pose.loc.Z,
            g_sawThrust ? 1 : 0);
    DC::Say((g_cliStage == 5 ? kRested2Prefix : kRestedPrefix) + (g_sawThrust ? "1 " : "0 ") + RC::Encode(pose));
    g_runTicks = 0;
    g_cliStage = g_cliStage == 5 ? 2 : 4;
}


void RunawayReset() {
    g_runCopy.Reset();
    g_placed = false;
    g_flightCm = 0.f;
    g_flightHasPrev = false;
    DG::Restart(g_runRest);
    g_sawThrust = false;
    g_runTicks = 0;
    g_cliStage = 0;
    g_cliArmed = false;
    g_leg2 = false;
}

}  // namespace coop::dev::fireext_drill::detail
