// coop/dev/coast_drill.cpp -- see coop/dev/coast_drill.h.

#include "coop/dev/coast_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_ground.h"
#include "coop/dev/rest_compare.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace coop::dev::coast_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace DC = coop::dev::drill_chat;
namespace DG = coop::dev::drill_ground;
namespace RC = coop::dev::rest_compare;
using ue_wrap::FVector;

enum class Arm { Off, LetGo, Unelected, Join };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::coast_drill);
        return v == "letgo" ? Arm::LetGo : v == "unelected" ? Arm::Unelected : v == "join" ? Arm::Join : Arm::Off;
    }();
    return a;
}

const char* Tag() {
    return ArmOf() == Arm::LetGo ? "coast-letgo" : ArmOf() == Arm::Unelected ? "coast-unelected" : "coast-join";
}

// The peer whose hand lets go: the client in the unelected arm, the host otherwise.
bool HostActs() { return ArmOf() != Arm::Unelected; }
bool IsHost() { return coop::roster::LocalIsHost(); }
char Who() { return IsHost() ? 'H' : 'C'; }
// The join arm's second client life, which the rig tells by this variable (--rejoin-env).
bool SecondLife() { return coop::config::ReadEnv("VOTVCOOP_COAST_LIFE") == "2"; }

constexpr const wchar_t* kCrateClass = L"prop_box_C";
constexpr const wchar_t* kCratePath  = L"/Game/objects/prop_box.prop_box_C";
constexpr float kHalfCm        = 100.f;  // the footprint kept clear round the crate
constexpr float kLiftCm        = 60.f;   // a spawn's lift above the measured floor
constexpr float kCrowdCm       = 300.f;  // another crate within this of the spot ends the search for it
constexpr float kFindCm        = 700.f;  // the client's search radius round the host's measured spot
constexpr float kReachCm       = 110.f;  // the director's stop distance from the crate, level
constexpr float kMovedCm       = 5.f;    // the crate must have ended further than this from where it stood
constexpr float kHeldOffCm     = 30.f;   // the held crate has come off the floor when it stands this far from where it rested
constexpr int   kMarkTicks     = 180;    // still ticks before a rest is marked: the End has landed
constexpr int   kWalkDeadlineS = 300;
constexpr uint64_t kBoundMs    = 360000; // a wait for the other peer or a rest: INVALID only, never a verdict

const std::string kReadyPrefix = "coast ready ";
const std::string kHeldLine    = "coast held";
const std::string kDroppedLine = "coast dropped";
const std::string kRestedPrefix = "coast rested ";

enum class Step { WaitPeer, Bind, WaitReady, Find, Walk, Grab, Lift, WaitHeld, WaitDropped, Settle, WaitJoiner,
                  WaitJoinerReady, WaitClientRest, Done };

Step g_step = Step::WaitPeer;
bool g_armed = false;
int  g_slot = 0;
uint64_t g_stepMs = 0;
ue_wrap::CachedObjRef g_crate;
RC::Pose g_before;
FVector g_spot{};
DG::Settle g_rest;
RC::Pose g_hostRest;
uint32_t g_eid = 0;                  // the host's element id of the crate
bool g_sawLetGo = false;             // the host flew the let-go: a coast row opened by CoastLetGo stood on the crate
bool g_sawBodyPush = false;          // ... or a body's push opened it instead
bool g_loadingAtRest = false;        // the join arm: the joiner was still loading at the host's last quiet tick
uint64_t g_endMarkMs = 0;            // the client: an End counts only from here (the join arm counts every End of its life)
bool g_over = false;
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
std::unique_ptr<coop::director::AimedGrab> g_grab;
char g_lastWait[2][160] = {};

void Next(Step s) {
    g_step = s;
    g_stepMs = ::GetTickCount64();
    DG::Restart(g_rest);
}
bool Expired() { return ::GetTickCount64() - g_stepMs > kBoundMs; }

void Wait(const char* what) {
    char* last = g_lastWait[IsHost() ? 0 : 1];
    if (std::strcmp(last, what) == 0) return;
    std::snprintf(last, sizeof(g_lastWait[0]), "%s", what);
    UE_LOGI("[COAST-DRILL] [%c] waiting for %s", Who(), what);
}

void Invalid(const char* why) {
    if (g_over) return;
    g_over = true;
    UE_LOGW("[COAST-DRILL] [%c] INVALID %s", Who(), why);
    coop::rig_verdict::Say(Tag(), coop::rig_verdict::Outcome::Invalid, "%s", why);
}

// An Invalid whose reason is built from a step's waiting text, so the log names what never came.
void InvalidWaiting(const char* what) {
    char why[200];
    std::snprintf(why, sizeof(why), "%s never came", what);
    Invalid(why);
}

// The drive Ends this peer applied lately include one for the crate's copy at or after `sinceMs`: the crate's own spawn fall
// is a coast with an End of its own, so an End from before the let-go proves nothing about it.
bool EndSeen(void* crate, uint64_t sinceMs) {
    const uint32_t eid = static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(crate));
    coop::prop_drive_stream::AppliedEnd ends[coop::prop_drive_stream::kRecentEnds];
    const size_t n = coop::prop_drive_stream::RecentEnds(ends, coop::prop_drive_stream::kRecentEnds);
    for (size_t i = 0; i < n; ++i)
        if (ends[i].eid == eid && ends[i].atMs >= sinceMs) return true;
    return false;
}

// The acting peer's walk to the crate and its aimed grab. Returns true once the crate is in the hand; says INVALID itself.
bool Grab(void* player, void* crate) {
    if (g_step == Step::Walk) {
        const int state = g_walk ? g_walk->state.load() : 2;
        if (state == 0) { Wait("the director's walk to the crate"); return false; }
        if (state == 2) { Invalid("the walk to the crate did not arrive"); return false; }
        g_grab = std::make_unique<coop::director::AimedGrab>(player, crate);
        Next(Step::Grab);
        return false;
    }
    const coop::director::GrabState gs = g_grab->Tick();
    if (gs == coop::director::GrabState::Working) return false;
    UE_LOGI("[COAST-DRILL] [%c] the trace took the crate after %d fan pose(s); grab chain: playerGrabbed_pre=%d useAction=%d "
            "playerGrabbed=%d", Who(), g_grab->AimPoses(), g_grab->ChainPre() ? 1 : 0, g_grab->ChainUse() ? 1 : 0,
            g_grab->ChainPost() ? 1 : 0);
    if (gs == coop::director::GrabState::Failed) { Invalid(g_grab->Why()); return false; }
    return true;
}

// The grab has taken the crate but a held prop hangs at the hand's point, which the camera's ray sets: the director aimed
// that ray down at the crate on the floor (rig run: the held crate stayed within 30 cm of where it rested for six minutes), so
// the view is raised to this pitch, as a player lifts what it holds, until the held copy stands kHeldOffCm from where it
// rested. A release before that lets it fall back where it was, a let-go that moved nothing.
constexpr float kLiftViewPitchDeg = 15.f;

bool Lifted(void* player, void* crate) {
    if (void* ctrl = player ? E::GetController(player) : nullptr) {
        ue_wrap::FRotator view = E::GetControlRotation(ctrl);
        view.Pitch = kLiftViewPitchDeg;
        E::SetControlRotation(ctrl, view);
    }
    RC::Pose now;
    if (!crate || !RC::Read(crate, now)) return false;
    const float off = RC::Cm(g_before, now);
    static uint32_t s_said = 0;
    if ((++s_said % 60) == 1)
        UE_LOGI("[COAST-DRILL] [%c] the held crate stands %.1f cm from where it rested (z %+.1f), the view pitched to %.0f deg",
                Who(), off, now.loc.Z - g_before.loc.Z, kLiftViewPitchDeg);
    return off >= kHeldOffCm;
}

// The crate in the hand is let go: a plain release, no throw.
void Drop(void* player) {
    coop::director::CallOnPlayer(player, L"dropGrabObject");
    DC::Say(kDroppedLine);
    UE_LOGI("[COAST-DRILL] [%c] the crate is let go (a plain release)", Who());
}

// ---- The host ------------------------------------------------------------------------------------------------

bool SlotReady(coop::net::Session& s, int slot) {
    return s.IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot) &&
           !coop::join_window_baseline::IsLateWindowOpen(slot);
}

void SayReady() {
    char spot[96];
    std::snprintf(spot, sizeof(spot), "%.0f %.0f %.0f", g_spot.X, g_spot.Y, g_spot.Z);
    DC::Say(kReadyPrefix + spot);
}

void HostWaitPeer(coop::net::Session& s) {
    int slot = 0;
    for (int i = 1; i < coop::net::kMaxPeers && !slot; ++i)
        if (SlotReady(s, i)) slot = i;
    if (!slot) { Wait("a client slot that is world-ready with its join window closed"); return; }
    g_slot = slot;
    void* puppet = coop::puppet_drive::Puppet(slot).GetActor();
    void* host = coop::players::Registry::Get().Local();
    FVector at{};
    if (!puppet || !R::IsLive(puppet) || !E::TryGetActorLocation(puppet, at) || !host || !R::IsLive(host)) {
        Wait("the client's puppet and this host's pawn to read");
        return;
    }
    if (!DG::FindSpot(host, at, kHalfCm, kCrateClass, kCrowdCm, "COAST-DRILL", g_spot)) {
        Invalid("no level open ground with a clear footprint and no other crate near");
        return;
    }
    void* cls = ue_wrap::asset_load::LoadObjectByPath(kCratePath);
    void* crate = cls ? E::SpawnActor(cls, FVector{g_spot.X, g_spot.Y, g_spot.Z + kLiftCm}) : nullptr;
    if (!crate) { Invalid("the crate did not spawn"); return; }
    g_crate.Set(crate);
    UE_LOGI("[COAST-DRILL] [H] spawned a crate (actor %p) over the floor (%.0f, %.0f, %.0f), %.0f cm from slot %d's puppet",
            crate, g_spot.X, g_spot.Y, g_spot.Z, DG::Dist(g_spot, at), slot);
    Next(Step::Bind);
}

void HostBind() {
    void* crate = g_crate.Get();
    if (!crate) { Invalid("the crate died before it rested"); return; }
    const uint32_t eid = static_cast<uint32_t>(coop::prop_element_tracker::GetPropElementIdForActor(crate));
    const bool bound = eid != static_cast<uint32_t>(coop::element::kInvalidId);
    FVector at{};
    const bool stable = E::TryGetActorLocation(crate, at) && DG::Stable(g_rest, at, DG::Quiet(crate));
    // Its spawn fall is a coast too: its End has gone out and the row is closed before the crate is called ready.
    const bool driven = bound && coop::prop_drive_host::ContextOf(crate, eid).open;
    if (!(bound && stable) || driven) {
        Wait("the crate's element id, rest and the end of its spawn fall's drive");
        if (Expired()) InvalidWaiting("the crate's element id and rest");
        return;
    }
    g_eid = eid;
    RC::Read(crate, g_before);
    SayReady();
    UE_LOGI("[COAST-DRILL] [H] READY: the crate rests at (%.1f, %.1f, %.1f)", g_before.loc.X, g_before.loc.Y, g_before.loc.Z);
    if (HostActs()) {
        g_walk = coop::director::StartBackgroundWalk(g_before.loc, kReachCm, kWalkDeadlineS, /*carry=*/false, crate);
        Next(Step::Walk);
    } else {
        Next(Step::WaitDropped);
    }
}

// The host's hold ends: the join arm waits for a joiner whose world was captured and has not come up.
bool JoinerLoading(coop::net::Session& s) {
    for (int i = 1; i < coop::net::kMaxPeers; ++i)
        if (coop::join_window_baseline::HasCapture(i) && !s.IsSlotWorldReady(i)) return true;
    return false;
}

// Who flew the crate: each tick after the spawn, the cause of the host coast row open on it, if one is.
void SampleCause() {
    coop::prop_drive_host::CoastCause c;
    if (!g_eid || !coop::prop_drive_host::OpenCoastCause(g_eid, c)) return;
    if (c == coop::prop_drive_host::CoastCause::LetGo) g_sawLetGo = true;
    if (c == coop::prop_drive_host::CoastCause::BodyPush) g_sawBodyPush = true;
}

void HostTick(coop::net::Session& s) {
    void* player = coop::players::Registry::Get().Local();
    SampleCause();
    switch (g_step) {
    case Step::WaitPeer: HostWaitPeer(s); return;
    case Step::Bind: HostBind(); return;
    case Step::Walk:
    case Step::Grab:
        if (!player || !R::IsLive(player)) return;
        if (!g_crate.Get()) { Invalid("the crate died before the grab"); return; }
        if (Grab(player, g_crate.Get())) {
            DC::Say(kHeldLine);
            UE_LOGI("[COAST-DRILL] [H] the crate is in the host's hand");
            g_grab.reset();
            Next(Step::Lift);
        }
        return;
    case Step::Lift:   // held and drawn up off the floor, then the join arm holds it and the others let go
        if (!player || !R::IsLive(player)) return;
        if (!g_crate.Get()) { Invalid("the crate died in the host's hand"); return; }
        if (!Lifted(player, g_crate.Get())) {
            Wait("the held crate to come off the floor");
            if (Expired()) InvalidWaiting("the held crate to come off the floor");
            return;
        }
        if (ArmOf() == Arm::Join) { Next(Step::WaitJoiner); return; }
        Drop(player);
        Next(Step::Settle);
        return;
    case Step::WaitJoiner:   // the join arm: hold the crate until a joiner is loading, then let go
        if (!player || !R::IsLive(player)) return;
        if (!JoinerLoading(s)) { Wait("a joiner whose world is captured and not yet up"); return; }
        UE_LOGI("[COAST-DRILL] [H] a joiner is loading");
        Drop(player);
        Next(Step::Settle);
        return;
    case Step::WaitDropped:   // the client lets go
        if (DC::SeenHas(kDroppedLine)) { Next(Step::Settle); return; }
        Wait("the client's dropped line");
        if (Expired()) InvalidWaiting("the client's dropped line");
        return;
    case Step::Settle: {
        void* crate = g_crate.Get();
        FVector at{};
        if (!crate || !E::TryGetActorLocation(crate, at)) { Invalid("the crate died before its rest"); return; }
        g_loadingAtRest = JoinerLoading(s);
        if (!DG::Stable(g_rest, at, DG::Quiet(crate), kMarkTicks)) {
            Wait("the host's copy to rest after the let-go");
            if (Expired()) InvalidWaiting("the host's copy's rest");
            return;
        }
        RC::Read(crate, g_hostRest);
        UE_LOGI("[COAST-DRILL] [H] the host's copy RESTED at (%.1f, %.1f, %.1f)", g_hostRest.loc.X, g_hostRest.loc.Y,
                g_hostRest.loc.Z);
        if (RC::Cm(g_before, g_hostRest) <= kMovedCm) {
            Invalid("the crate did not move from where it stood: nothing was let go");
            return;
        }
        if (ArmOf() == Arm::Join && !g_loadingAtRest) {
            Invalid("the joiner's world was up before the crate rested: the join arm needs the crate to rest while the joiner loads");
            return;
        }
        Next(ArmOf() == Arm::Join ? Step::WaitJoinerReady : Step::WaitClientRest);
        return;
    }
    case Step::WaitJoinerReady: {   // the join arm: the joiner missed the ready line, so it is said again when its world is up
        bool ready = false;
        for (int i = 1; i < coop::net::kMaxPeers && !ready; ++i) ready = SlotReady(s, i);
        if (!ready) {
            Wait("the joiner's world to come up");
            if (Expired()) InvalidWaiting("the joiner's world to come up");
            return;
        }
        SayReady();
        UE_LOGI("[COAST-DRILL] [H] the joiner's world is up: ready said again");
        Next(Step::WaitClientRest);
        return;
    }
    case Step::WaitClientRest: {
        std::string rest;
        if (!DC::SeenRestOf(kRestedPrefix, &rest)) {
            Wait("the client's rested line");
            if (Expired()) InvalidWaiting("the client's rested line");
            return;
        }
        // "<end seen 0|1> <pose>"
        if (rest.size() < 3 || (rest[0] != '0' && rest[0] != '1') || rest[1] != ' ') {
            Invalid("the client's rested line could not be read");
            return;
        }
        RC::Pose client;
        if (!RC::Decode(rest.substr(2), client)) { Invalid("the client's rested pose could not be read"); return; }
        const bool endSeen = rest[0] == '1';
        namespace RV = coop::rig_verdict;
        if (ArmOf() == Arm::Join) {
            // A joiner that missed the crate's live End rests with the host's: the snapshot is cut after the crate rested, so it
            // carries the rest (measured: this arm passes with no End sent to the joiner), and no resend is needed.
            if (RC::Judge(Tag(), "the crate that rested while the joiner loaded", g_hostRest, client))
                RV::Say(Tag(), RV::Outcome::Pass, "the joiner's copy, which loaded while the crate rested, rests within %.0f cm "
                        "and %.0f deg of the host's", RC::kBoundCm, RC::kBoundDeg);
        } else if (RC::Judge(Tag(), "the let-go crate", g_hostRest, client)) {
            // The copies' rest comes first: it is what a let-go left unflown costs the player. Within it, the row that flew the
            // crate must be the let-go's, and its End must have reached the client (a body's push that also covered the crate
            // would hide a let-go that flew nothing).
            if (!g_sawLetGo)
                RV::Say(Tag(), RV::Outcome::Fail, "no let-go coast flew the crate (%s): CoastLetGo did not run for it",
                        g_sawBodyPush ? "a body's push flew it instead" : "nothing flew it");
            else if (!endSeen)
                RV::Say(Tag(), RV::Outcome::Fail, "no drive End after the let-go reached the client's copy");
            else
                RV::Say(Tag(), RV::Outcome::Pass, "the let-go coast flew the crate, its End reached the client, and the copies "
                        "rest within %.0f cm and %.0f deg", RC::kBoundCm, RC::kBoundDeg);
        }
        UE_LOGI("[COAST-DRILL] [H] DONE");
        Next(Step::Done);
        return;
    }
    default: return;
    }
}

// ---- The client ----------------------------------------------------------------------------------------------

void ClientTick(coop::net::Session& /*s*/) {
    void* player = coop::players::Registry::Get().Local();
    if (!coop::net_pump::HasAnnouncedWorldReady() ||
        coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle || !player || !R::IsLive(player)) {
        Wait("this client's join to end and its pawn");
        return;
    }
    switch (g_step) {
    case Step::WaitPeer: Next(Step::WaitReady); return;
    case Step::WaitReady: {
        std::string rest;
        if (!DC::SeenRestOf(kReadyPrefix, &rest)) {
            Wait("the host's ready line");
            if (Expired()) InvalidWaiting("the host's ready line");
            return;
        }
        float x = 0.f, y = 0.f, z = 0.f;
        if (sscanf_s(rest.c_str(), "%f %f %f", &x, &y, &z) != 3) { Invalid("the host's ready line could not be read"); return; }
        g_spot = {x, y, z};
        UE_LOGI("[COAST-DRILL] [C] the host's crate stands over (%.0f, %.0f, %.0f)", x, y, z);
        Next(Step::Find);
        return;
    }
    case Step::Find: {
        void* crate = DG::NearestOf(kCrateClass, g_spot, kFindCm);
        if (!crate) {
            Wait("this client's copy of the crate to arrive");
            if (Expired()) InvalidWaiting("this client's copy of the crate");
            return;
        }
        g_crate.Set(crate);
        RC::Read(crate, g_before);
        g_endMarkMs = ArmOf() == Arm::Join ? 0 : ::GetTickCount64();
        UE_LOGI("[COAST-DRILL] [C] found its copy of the crate at (%.1f, %.1f, %.1f)", g_before.loc.X, g_before.loc.Y,
                g_before.loc.Z);
        if (!HostActs()) {
            g_walk = coop::director::StartBackgroundWalk(g_before.loc, kReachCm, kWalkDeadlineS, /*carry=*/false, crate);
            Next(Step::Walk);
        } else if (ArmOf() == Arm::Join && !SecondLife()) {
            Next(Step::WaitHeld);
        } else if (ArmOf() == Arm::Join) {
            Next(Step::Settle);   // the second life: the crate was let go before this client's world came up
        } else {
            Next(Step::WaitDropped);
        }
        return;
    }
    case Step::Walk:
    case Step::Grab:
        if (!g_crate.Get()) { Invalid("the crate's copy died before the grab"); return; }
        if (Grab(player, g_crate.Get())) {
            g_grab.reset();
            Next(Step::Lift);
        }
        return;
    case Step::Lift:   // held and drawn up off the floor, then let go
        if (!g_crate.Get()) { Invalid("the crate's copy died in the hand"); return; }
        if (!Lifted(player, g_crate.Get())) {
            Wait("the held crate to come off the floor");
            if (Expired()) InvalidWaiting("the held crate to come off the floor");
            return;
        }
        Drop(player);
        Next(Step::Settle);
        return;
    case Step::WaitHeld:   // the join arm's first life: the host holds the crate, the rig may rejoin this client now
        if (DC::SeenHas(kHeldLine)) {
            UE_LOGI("[COAST-DRILL] [C] REJOIN-NOW: the host holds the crate");
            Next(Step::Done);
            return;
        }
        Wait("the host's held line");
        if (Expired()) InvalidWaiting("the host's held line");
        return;
    case Step::WaitDropped:
        if (DC::SeenHas(kDroppedLine)) {
            Next(Step::Settle);
        } else {
            Wait("the host's dropped line");
            if (Expired()) InvalidWaiting("the host's dropped line");
        }
        return;
    case Step::Settle: {
        void* crate = g_crate.Get();
        FVector at{};
        if (!crate || !E::TryGetActorLocation(crate, at)) { Invalid("this client's copy died before its rest"); return; }
        if (!DG::Stable(g_rest, at, DG::Quiet(crate), kMarkTicks)) {
            Wait("this client's copy to rest");
            if (Expired()) InvalidWaiting("this client's copy's rest");
            return;
        }
        RC::Pose pose;
        if (!RC::Read(crate, pose)) { Invalid("this client's copy's pose could not be read"); return; }
        const bool endSeen = EndSeen(crate, g_endMarkMs);
        UE_LOGI("[COAST-DRILL] [C] this client's copy RESTED at (%.1f, %.1f, %.1f); a drive End reached it: %d", pose.loc.X,
                pose.loc.Y, pose.loc.Z, endSeen ? 1 : 0);
        DC::Say(kRestedPrefix + (endSeen ? "1 " : "0 ") + RC::Encode(pose));
        UE_LOGI("[COAST-DRILL] [C] DONE");
        Next(Step::Done);
        return;
    }
    default: return;
    }
}

}  // namespace

bool IsEnabled() { return ArmOf() != Arm::Off; }

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || !session || !session->connected() || g_over || g_step == Step::Done) return;
    if (!g_armed) {
        // The first Seen* read starts the chat history: lines reach a peer while its join window is still open, and the feed
        // retires a row after about ten seconds.
        g_armed = true;
        DC::SeenHas(kReadyPrefix);
        UE_LOGI("[COAST-DRILL] [%c] started (arm %s): reading the chat history", Who(), Tag());
    }
    if (IsHost()) HostTick(*session);
    else ClientTick(*session);
}

void OnDisconnect() {
    // The join arm's host keeps its crate in its hand across the first client's departure: the rejoin is the arm.
    if (IsHost() && ArmOf() == Arm::Join && (g_step == Step::Lift || g_step == Step::WaitJoiner)) return;
    g_step = Step::WaitPeer;
    g_armed = false;
    g_over = false;
    g_slot = 0;
    g_crate.Reset();
    g_eid = 0;
    g_sawLetGo = g_sawBodyPush = g_loadingAtRest = false;
    g_endMarkMs = 0;
    DG::Restart(g_rest);
    g_walk.reset();
    g_grab.reset();
    g_lastWait[0][0] = g_lastWait[1][0] = '\0';
}

}  // namespace coop::dev::coast_drill
