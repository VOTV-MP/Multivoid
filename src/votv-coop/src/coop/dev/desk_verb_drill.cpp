// coop/dev/desk_verb_drill.cpp -- see coop/dev/desk_verb_drill.h. The census, the desk's readings and the host's
// fixtures are coop/dev/desk_verb_drill_desk.cpp's.

#include "coop/dev/desk_verb_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/desk_verb_drill_internal.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/interactables/desk_verb_effects.h"  // CountsNow: the glosses and sounds sent and made
#include "coop/interactables/signal_catch_sync.h"  // LocalCatchesRelayed: the caught signal is on the wire
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/desk_press.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/desk/meadow_store.h"
#include "ue_wrap/desk/saved_signals.h"
#include "ue_wrap/engine/engine_component.h"  // GetComponentLocation
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::desk_verb_drill {
namespace {

using namespace detail;
namespace CD = ue_wrap::console_desk;
namespace DC = ue_wrap::drive_chain;
namespace DP = ue_wrap::desk_press;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace SS = ue_wrap::saved_signals;

enum class Mode : uint8_t { Off, Run, Join };
Mode ModeOf() {
    static const Mode m = [] {
        const std::string v = coop::config::ResolveString(::coop::config_registry::rows::desk_verb_drill);
        return v == "run" ? Mode::Run : v == "join" ? Mode::Join : Mode::Off;
    }();
    return m;
}

constexpr float    kReachCm = 150.f;  // the director's stop at a button; the use trace reaches 200
constexpr int      kWalkDeadlineS = 240;
constexpr int      kAimFanHalf = 4;
constexpr float    kAimFanStepDeg = 4.f;
constexpr int      kAimTicksPerPose = 4;
// Where a fan from the first stop finds the button hidden (the download unit's opened cap hides DELETE from the
// side), a player steps round it: points on a ring about the button, kept only where a NavMesh route from the
// player ends within reach of them (a point inside the desk has a route that ends short, and a walk to it only
// grinds), walked by the director shortest route first.
constexpr int      kStandpoints = 6;
constexpr float    kStandRingCm = 110.f;
constexpr float    kStandReachCm = 60.f;
constexpr int      kStandWalkDeadlineS = 30;  // a walk of a metre or two
// Failure bounds only: each step ends on the state it waits for.
constexpr uint64_t kStartBoundMs = 60000;  // a ready world to a started drill: the join settled, the census live,
                                           // the desk, the saved signals and the player resolved
constexpr uint64_t kArmBoundMs = 30000;
constexpr uint64_t kDoneBoundMs = 30000;

// ---- the host -------------------------------------------------------------------------------------------------

enum class HStep : uint8_t { WaitClient, JoinRender, Census, Arm, ArmDownload, ArmRender, WaitDone, Done };
HStep    g_host = HStep::WaitClient;
int      g_hostLeg = kSave;
uint64_t g_hostMs = 0;
Census   g_hostBefore;
size_t   g_hostBand = 0;       // band rows before the leg
int32_t  g_hostLaptop = 0;     // the laptop's rows before the leg
bool     g_hostPass = true;
uint64_t g_hostCatches = 0;    // the catches relayed when the leg's caught signal was written
uint64_t g_hostRow = 0;        // the band row the drive legs move, by identity
bool     g_hostDecoding = false;  // join: the host's own decode runs as the client joins
coop::desk_verb_effects::Counts g_hostEffects;  // the glosses and sounds sent when the leg armed

void HostGo(HStep s) {
    g_host = s;
    g_hostMs = ::GetTickCount64();
}

void HostDone(bool pass, const char* why) {
    RestoreProcessLevel();
    UE_LOGI("[DESK-VERB-DRILL] host DONE %s%s%s", pass ? "PASS" : "FAIL", why ? " -- " : "", why ? why : "");
    g_host = HStep::Done;
}

void HostAbandon(const char* why) {
    RestoreProcessLevel();
    UE_LOGW("[DESK-VERB-DRILL] ABANDONED on the host: %s", why);
    g_host = HStep::Done;
}

void HostJudge(int leg) {
    const Census d = Since(g_hostBefore);
    bool ok = true;
    const coop::desk_verb_effects::Counts e = coop::desk_verb_effects::CountsNow();
    if (leg == kSave)  // its gloss entered here and was refused, and its clicks were sent, not played
        ok = d.game[kGetSigObj] == 1 && d.game[kSetSignalID] == 1 && d.game[kSaveSignal] >= 1 &&
             d.game[kAddGloss] == 1 && d.ran[kAddGloss] == 0 && e.glossesSent - g_hostEffects.glossesSent == 1 &&
             e.soundsSent > g_hostEffects.soundsSent;
    else if (leg == kDelete)
        ok = d.game[kDeleteActive] == 1;
    else if (leg == kSend)  // the laptop took the saved row, not the deck's own selection
        ok = d.game[kLaptopAdd] == 1 && LaptopNewest() == BandHash();
    else if (leg == kExport)
        ok = d.game[kDeleteSignal] == 1;
    else if (leg == kImport)
        ok = d.game[kSaveSignal] == 1;
    else if (leg == kUpload)
        ok = d.game[kCompUpload] == 1;
    std::string row;
    if (IsRefinerLeg(leg)) ok = RefinerHostJudge(leg, d, e.glossesSent - g_hostEffects.glossesSent, row);
    g_hostPass = g_hostPass && ok;
    if (leg == kSave) {
        const std::vector<int32_t> rows = BandRows();
        RowSeen seen;
        if (!rows.empty() && ReadRowSeen(rows.back(), seen)) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "; saved row %016llx with a %zu-byte photo (digest %016llx), gloss '%ls'",
                          static_cast<unsigned long long>(seen.hash), seen.photo,
                          static_cast<unsigned long long>(seen.photoDigest), seen.signal.c_str());
            row = buf;
        }
    }
    UE_LOGI("[DESK-VERB-DRILL] host leg %s %s: the game's own entries here -- %s%s", kLegName[leg],
            ok ? "PASS" : "FAIL", CensusLine(d).c_str(), row.c_str());
}

bool HostLegDone(int leg) {
    switch (leg) {
    case kSave:   return BandRows().size() > g_hostBand;
    case kDelete: return CaughtX() != kBandX + static_cast<float>(kDelete);
    case kSend:   return ue_wrap::meadow_store::Count() > g_hostLaptop;
    case kExport: return BandRows().empty() && DriveRow(SlotDrive(DC::kRoleDeskPlay)) == g_hostRow;
    case kImport: return BandRows().size() == 1 && DriveRow(SlotDrive(DC::kRoleDeskPlay)) == 0;
    case kUpload: return CompRow() != 0 && CompRow() == g_hostRow;
    default:      return RefinerHostDone(leg);
    }
}

// join: the host's own decode, running as the client joins, so the joiner's save holds a decode its restore resumes.
void HostJoinDecode() {
    const char* why = nullptr;
    const int r = detail::HostJoinDecode(why);
    if (r < 0) HostAbandon(why);
    else if (r > 0) g_hostDecoding = true;
}

// join: the caught signal armed once the joiner's save snapshot is taken, so the row it saves reaches the joiner by
// the seed; no client is in the world yet to order the arm against.
void HostJoinArm() {
    if (!JoinSnapshotTaken()) return;
    const int purged = PurgeBand();
    if (!FindSource()) { HostAbandon("no sky signal to form a caught signal from"); return; }
    if (!WriteCaught(kSave) || !FormDownload()) { HostAbandon("the caught signal did not arm"); return; }
    UE_LOGI("[DESK-VERB-DRILL] host armed the join's caught signal from the sky's object '%ls' after the joiner's save "
            "snapshot (%d earlier band rows purged)", SourceObject().c_str(), purged);
    HostGo(HStep::JoinRender);
}

// join: the host's own SAVE once the download rendered.
void HostJoinSave(void* player) {
    if (!Detect()) { HostAbandon("the needle did not write"); return; }
    void* button = Button(kSave);
    if (!button || !DP::Press(CD::Instance(), player, button)) { HostAbandon("the host's SAVE press did not run"); return; }
    const std::vector<int32_t> rows = BandRows();
    RowSeen seen;
    if (rows.size() != 1 || !ReadRowSeen(rows.back(), seen)) {
        HostDone(false, "the host's SAVE left no one row in the band");
        return;
    }
    UE_LOGI("[DESK-VERB-DRILL] host saved row %016llx with a %zu-byte photo (digest %016llx) as the client joins",
            static_cast<unsigned long long>(seen.hash), seen.photo, static_cast<unsigned long long>(seen.photoDigest));
    HostDone(seen.photo > 0, seen.photo > 0 ? nullptr : "the saved row has no photo");
}

void HostTick(coop::net::Session* s, void* player) {
    const uint64_t now = ::GetTickCount64();
    switch (g_host) {
    case HStep::WaitClient:
        if (ModeOf() == Mode::Join) {
            if (!g_hostDecoding) {
                HostJoinDecode();
                return;
            }
            if (!s->AnyWorldReadyPeer()) HostJoinArm();
            else HostAbandon("the client's world was ready before the host could save as it joined");
            return;
        }
        if (!s->AnyWorldReadyPeer()) return;
        HostGo(HStep::Census);
        return;
    case HStep::JoinRender:
        if (!Rendered()) {
            if (now - g_hostMs > kArmBoundMs) HostAbandon("the download never rendered");
            return;
        }
        if (s->AnyWorldReadyPeer()) { HostAbandon("the client's world was ready before the host could save"); return; }
        HostJoinSave(player);
        return;
    case HStep::Census: {
        if (const wchar_t* fn = CensusNotLive()) {
            if (now - g_hostMs > kStartBoundMs) {
                UE_LOGW("[DESK-VERB-DRILL] ABANDONED on the host: the census watch on '%ls' never went live", fn);
                g_host = HStep::Done;
            }
            return;
        }
        const int purged = PurgeBand();
        if (!FindSource()) { HostAbandon("no sky signal to form a caught signal from"); return; }
        UE_LOGI("[DESK-VERB-DRILL] host: the census is live; %d earlier band rows purged; caught signals form from "
                "the sky's object '%ls'; %s", purged, SourceObject().c_str(), PowerLine().c_str());
        g_hostLeg = kSave;
        HostGo(HStep::Arm);
        return;
    }
    case HStep::Arm:
        if (g_hostLeg == kExport || g_hostLeg == kUpload) {
            const int r = ArmDrive(g_hostLeg == kExport ? DC::kRoleDeskPlay : DC::kRoleDeskComp, g_hostLeg == kUpload);
            if (r < 0) { HostAbandon("the leg's drive did not spawn, take its row or go in"); return; }
            if (r == 0) {
                if (now - g_hostMs > kArmBoundMs) HostAbandon("the leg's drive never went in");
                return;
            }
            g_hostRow = BandHash();
        }
        if (g_hostLeg == kSave || g_hostLeg == kDelete) {
            g_hostCatches = coop::signal_catch_sync::LocalCatchesRelayed();
            if (!WriteCaught(g_hostLeg)) { HostAbandon("the caught signal did not write"); return; }
        }
        if (IsRefinerLeg(g_hostLeg)) {
            if (const char* why = RefinerArm(g_hostLeg, player)) { HostAbandon(why); return; }
        }
        HostGo(HStep::ArmDownload);
        return;
    case HStep::ArmDownload:
        // DELETE's fixture is the caught signal alone: its branch deletes the signal and reads no download
        // (analogDScreenTest.cpp:3048-3064).
        if (g_hostLeg == kSave || g_hostLeg == kDelete) {
            if (coop::signal_catch_sync::LocalCatchesRelayed() == g_hostCatches) {
                if (now - g_hostMs > kArmBoundMs) HostAbandon("the caught signal was never relayed");
                return;
            }
            if (g_hostLeg == kSave && !FormDownload()) { HostAbandon("the download did not form"); return; }
        }
        HostGo(HStep::ArmRender);
        return;
    case HStep::ArmRender:
        if (g_hostLeg == kSave) {
            if (!Rendered()) {
                if (now - g_hostMs > kArmBoundMs) HostAbandon("the download never rendered");
                return;
            }
            if (!Detect()) { HostAbandon("the needle did not write"); return; }
        }
        g_hostBefore = CensusNow();
        g_hostEffects = coop::desk_verb_effects::CountsNow();
        g_hostBand = BandRows().size();
        g_hostLaptop = ue_wrap::meadow_store::Count();
        UE_LOGI("[DESK-VERB-DRILL] host armed leg %s", kLegName[g_hostLeg]);
        HostGo(HStep::WaitDone);
        return;
    case HStep::WaitDone:
        // A leg that does not come is ended by the client's own bounds: its walk, its wait for the fixture, its
        // aim and its wait for the result. A completing leg's start, once it latched this refiner, completes.
        if (IsRefinerLeg(g_hostLeg)) {
            if (const char* why = RefinerStep(g_hostLeg)) { HostAbandon(why); return; }
        }
        if (!HostLegDone(g_hostLeg)) return;
        HostJudge(g_hostLeg);
        if (++g_hostLeg < kLegs) {
            HostGo(HStep::Arm);
            return;
        }
        HostDone(g_hostPass, g_hostPass ? nullptr : "a leg's entries were not the host's");
        return;
    default:
        return;
    }
}

// ---- the client -----------------------------------------------------------------------------------------------

enum class CStep : uint8_t { Ready, JoinWait, Walk, WaitArmed, Aim, LidWait, Press, WaitDone, Done };
CStep    g_client = CStep::Ready;
int      g_leg = kSave;
int      g_stepTicks = 0;
uint64_t g_stepMs = 0;
int      g_aimPose = 0;
int      g_stand = 0;          // 0: the stop at the button; 1..g_ring.size(): a standpoint of the ring about it
std::vector<ue_wrap::FVector> g_ring;  // the ring's reachable standpoints about the leg's button, in walk order
uint64_t g_readySinceMs = 0;       // this client's world ready: the start's bound runs from here
uint64_t g_unresolvedSinceMs = 0;  // since when the desk, the saved signals or the player have not resolved
std::wstring g_aimTook;        // what the trace took instead during the fan, the distinct ones in order
bool     g_clientPass = true;
Census   g_before;
coop::desk_verb_effects::Counts g_effectsBefore;
size_t   g_bandBefore = 0;
int32_t  g_laptopBefore = 0;
uint64_t g_bandRow = 0;       // the band row the drive legs move, by identity, as this client saw it
bool     g_loadArmed = false; // join: this session's census is live ahead of the load, or will never be
std::shared_ptr<coop::director::BackgroundWalk> g_walk;

void Go(CStep s) {
    g_client = s;
    g_stepTicks = 0;
    g_stepMs = ::GetTickCount64();
}

void Abandon(const char* why) {
    UE_LOGW("[DESK-VERB-DRILL] ABANDONED on the client: %s", why);
    g_client = CStep::Done;
}

const std::vector<std::pair<int, int>>& AimFan() {
    static const std::vector<std::pair<int, int>> fan = [] {
        std::vector<std::pair<int, int>> v;
        for (int p = -kAimFanHalf; p <= kAimFanHalf; ++p)
            for (int y = -kAimFanHalf; y <= kAimFanHalf; ++y) v.emplace_back(p, y);
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
            return a.first * a.first + a.second * a.second < b.first * b.first + b.second * b.second;
        });
        return v;
    }();
    return fan;
}

// The fixture of leg `leg` is on this copy of the desk: the host's caught signal, and for SAVE the download formed
// from it and its needle; for send and export, this deck pointed at the saved row, a scroll this client makes
// itself, and for export an empty drive in the deck; for import that drive holding the row; for upload the
// refiner's drive holding it.
bool Armed(int leg) {
    if (leg == kSave) return CaughtX() == kBandX && CD::DownloadMeshValid() && Needle() >= 1.f;
    if (leg == kDelete) return CaughtX() == kBandX + static_cast<float>(kDelete);
    if (leg == kImport) return g_bandRow != 0 && DriveRow(SlotDrive(DC::kRoleDeskPlay)) == g_bandRow;
    if (IsRefinerLeg(leg)) return RefinerArmed(leg);
    if (leg == kUpload) {
        g_bandRow = BandHash();
        return g_bandRow != 0 && DriveRow(SlotDrive(DC::kRoleDeskComp)) == g_bandRow;
    }
    if (leg == kExport) {
        void* drive = SlotDrive(DC::kRoleDeskPlay);
        if (!drive || DriveRow(drive) != 0 ||
            coop::element::Registry::Get().EidForActor(drive) == coop::element::kInvalidId)
            return false;
    }
    const std::vector<int32_t> rows = BandRows();
    if (rows.empty()) return false;
    void* desk = CD::Instance();
    if (desk && DP::SelectedRow(desk) != rows.back()) DP::SelectRow(desk, rows.back());
    g_bandRow = BandHash();
    return desk && DP::SelectedRow(desk) == rows.back();
}

bool ClientLegDone(int leg) {
    switch (leg) {
    case kSave:   return BandRows().size() > g_bandBefore;
    case kDelete: return CaughtX() != kBandX + static_cast<float>(kDelete);
    case kSend:   return ue_wrap::meadow_store::Count() > g_laptopBefore;
    case kExport: return BandRows().empty() && DriveRow(SlotDrive(DC::kRoleDeskPlay)) == g_bandRow;
    case kImport: return BandRows().size() == 1 && DriveRow(SlotDrive(DC::kRoleDeskPlay)) == 0;
    case kUpload: return CompRow() == g_bandRow;
    default:      return RefinerClientDone(leg);
    }
}

void ClientJudge(int leg) {
    const Census d = Since(g_before);
    bool ok = true;
    for (int i = 0; i < kFns; ++i) ok = ok && d.game[i] == 0;
    std::string extra;
    if (leg == kSend) ok = ok && LaptopNewest() == BandHash();  // the saved row, not the deck's own selection
    if (IsRefinerLeg(leg)) {
        const coop::desk_verb_effects::Counts e = coop::desk_verb_effects::CountsNow();
        ok = RefinerClientJudge(leg, d, e.glossesMade - g_effectsBefore.glossesMade, extra) && ok;
    }
    if (leg == kSave) {
        // The host's gloss made here once, its clicks played here: counted by the effects, since the profile may
        // hold the name from an earlier run already.
        const coop::desk_verb_effects::Counts e = coop::desk_verb_effects::CountsNow();
        const std::vector<int32_t> rows = BandRows();
        RowSeen seen;
        const bool read = !rows.empty() && ReadRowSeen(rows.back(), seen);
        const int gloss = read ? GlossaryHas(seen.signal) : -1;
        ok = ok && read && seen.photo > 0 && gloss == 1 && d.other[kAddGloss] == 1 &&
             e.glossesMade - g_effectsBefore.glossesMade == 1 && e.soundsMade > g_effectsBefore.soundsMade;
        char buf[240];
        std::snprintf(buf, sizeof(buf), "; saved row %016llx with a %zu-byte photo (digest %016llx); gloss '%ls' in "
                      "this profile: %s", static_cast<unsigned long long>(seen.hash), seen.photo,
                      static_cast<unsigned long long>(seen.photoDigest), seen.signal.c_str(),
                      gloss == 1 ? "yes" : gloss == 0 ? "NO" : "unread");
        extra = buf;
    }
    g_clientPass = g_clientPass && ok;
    UE_LOGI("[DESK-VERB-DRILL] client leg %s %s: the game's own entries here -- %s%s", kLegName[leg],
            ok ? "PASS" : "FAIL", CensusLine(d).c_str(), extra.c_str());
}

// join: the row the host saved as this client joined, with its photo, once the seed brought it; and the host's
// decode, running as this client loaded its save: the restore refused, this refiner never latched, mirroring it.
void ClientJoinCheck() {
    const std::vector<int32_t> rows = BandRows();
    RowSeen seen;
    const bool read = rows.size() == 1 && ReadRowSeen(rows.back(), seen);
    std::string refiner;
    const bool refinerOk = RefinerJoinCheck(refiner);
    UE_LOGI("[DESK-VERB-DRILL] client joined: %zu band rows; the saved row %016llx with a %zu-byte photo (digest "
            "%016llx)%s", rows.size(), static_cast<unsigned long long>(seen.hash), seen.photo,
            static_cast<unsigned long long>(seen.photoDigest), refiner.c_str());
    UE_LOGI("[DESK-VERB-DRILL] client DONE %s", read && seen.photo > 0 && refinerOk ? "PASS" : "FAIL");
    g_client = CStep::Done;
}

void NextStandpoint(void* player, const char* why) {
    if (g_stand == 0) {
        void* button = Button(g_leg);
        g_ring = button ? coop::director::ReachableStandpoints(player, E::GetComponentLocation(button), kStandRingCm,
                                                               kStandpoints, kStandReachCm)
                        : std::vector<ue_wrap::FVector>{};
        UE_LOGI("[DESK-VERB-DRILL] client: a route reaches %zu of the %d standpoints about %s", g_ring.size(),
                kStandpoints, kLegName[g_leg]);
    }
    if (++g_stand > static_cast<int>(g_ring.size())) {
        Abandon(why);
        return;
    }
    UE_LOGI("[DESK-VERB-DRILL] client: %s -- stepping to standpoint %d of %zu about %s", why, g_stand, g_ring.size(),
            kLegName[g_leg]);
    g_aimTook.clear();
    g_walk.reset();
    Go(CStep::Walk);
}

void ClientTick(void* player) {
    ++g_stepTicks;
    const uint64_t now = ::GetTickCount64();
    switch (g_client) {
    case CStep::Ready: {
        if (!coop::net_pump::HasAnnouncedWorldReady()) return;
        // From the world's readiness on, the start is bounded: the join settling, then the census going live.
        if (!g_readySinceMs) g_readySinceMs = now;
        const bool settled = coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
        const wchar_t* notLive = settled && ModeOf() == Mode::Run ? CensusNotLive() : nullptr;
        if (!settled || notLive) {
            if (now - g_readySinceMs <= kStartBoundMs) return;
            if (!settled) {
                Abandon("the join never settled");
                return;
            }
            UE_LOGW("[DESK-VERB-DRILL] ABANDONED on the client: the census watch on '%ls' never went live", notLive);
            g_client = CStep::Done;
            return;
        }
        if (ModeOf() == Mode::Join) {
            Go(CStep::JoinWait);
            return;
        }
        UE_LOGI("[DESK-VERB-DRILL] client: the census is live; %s", PowerLine().c_str());
        g_leg = kSave;
        Go(CStep::Walk);
        return;
    }
    case CStep::JoinWait:
        // The seed follows the world's readiness: its row arrives a moment after (signal_sync's seed at the ready
        // edge).
        if ((BandRows().empty() || !RefinerJoinSeeded()) && now - g_stepMs <= kDoneBoundMs) return;
        ClientJoinCheck();
        return;
    case CStep::Walk: {
        void* button = Button(g_leg);
        if (!button) { Abandon("the leg's button does not read on this desk"); return; }
        if (!g_walk) {
            g_walk = g_stand ? coop::director::StartBackgroundWalk(g_ring[g_stand - 1], kStandReachCm,
                                                                   kStandWalkDeadlineS)
                             : coop::director::StartBackgroundWalk(E::GetComponentLocation(button), kReachCm,
                                                                   kWalkDeadlineS);
            return;
        }
        const int w = g_walk->state.load();
        if (w == 0) return;
        g_walk.reset();
        if (w == 2) {
            if (g_stand == 0) { Abandon("the walk to the desk did not arrive"); return; }
            NextStandpoint(player, "that standpoint could not be walked to");
            return;
        }
        g_aimPose = 0;
        Go(g_stand ? CStep::Aim : CStep::WaitArmed);
        return;
    }
    case CStep::WaitArmed:
        if (!Armed(g_leg)) {
            if (now - g_stepMs > kArmBoundMs) Abandon("the leg's fixture never reached this desk");
            return;
        }
        g_aimPose = 0;
        Go(CStep::Aim);
        return;
    case CStep::Aim: {
        void* button = Button(g_leg);
        if (!button) { Abandon("the leg's button does not read on this desk"); return; }
        void* took = E::ReadMainPlayerHitComponent(player);
        if (took == button) {
            UE_LOGI("[DESK-VERB-DRILL] client: the trace took %s at fan pose %d from standpoint %d", kLegName[g_leg],
                    g_aimPose, g_stand);
            g_aimTook.clear();
            Go(CStep::Press);
            return;
        }
        // The desk's own cap in the way, closed: a player opens it first.
        void* desk = CD::Instance();
        if (took && desk && took == DP::Member(desk, L"cap") && !CapOpened(desk)) {
            if (!E::CallMainPlayerUseSelectedAction(player)) { Abandon("the press on the cap did not dispatch"); return; }
            UE_LOGI("[DESK-VERB-DRILL] client opened the desk's cap in the way of %s", kLegName[g_leg]);
            Go(CStep::LidWait);
            return;
        }
        if (took && R::IsLive(took)) {
            const std::wstring name = R::ToString(R::NameOf(took));
            if (g_aimTook.find(name) == std::wstring::npos && g_aimTook.size() < 400) g_aimTook += name + L" ";
        }
        if (g_stepTicks % kAimTicksPerPose != 1) return;
        const auto& fan = AimFan();
        if (g_aimPose >= static_cast<int>(fan.size())) {
            const ue_wrap::FVector cam = E::GetCameraLocation(), at = E::GetComponentLocation(button);
            UE_LOGW("[DESK-VERB-DRILL] client: no heading put the trace on %s at (%.0f,%.0f,%.0f) from the camera at "
                    "(%.0f,%.0f,%.0f); it took: %ls", kLegName[g_leg], at.X, at.Y, at.Z, cam.X, cam.Y, cam.Z,
                    g_aimTook.empty() ? L"nothing" : g_aimTook.c_str());
            NextStandpoint(player, "no heading of the fan put the trace on the button");
            return;
        }
        ue_wrap::FRotator r = coop::director::LookAt(E::GetCameraLocation(), E::GetComponentLocation(button));
        r.Pitch += kAimFanStepDeg * static_cast<float>(fan[g_aimPose].first);
        r.Yaw += kAimFanStepDeg * static_cast<float>(fan[g_aimPose].second);
        E::SetControlRotation(E::GetController(player), r);
        ++g_aimPose;
        return;
    }
    case CStep::LidWait:
        if (!CapOpened(CD::Instance())) {
            if (now - g_stepMs > kArmBoundMs) Abandon("the desk's cap never opened");
            return;
        }
        g_aimPose = 0;
        Go(CStep::Aim);
        return;
    case CStep::Press:
        g_before = CensusNow();
        g_effectsBefore = coop::desk_verb_effects::CountsNow();
        g_bandBefore = BandRows().size();
        g_laptopBefore = ue_wrap::meadow_store::Count();
        RefinerBeforePress();
        if (!E::CallMainPlayerUseSelectedAction(player)) { Abandon("useSelectedAction did not dispatch"); return; }
        UE_LOGI("[DESK-VERB-DRILL] client pressed %s", kLegName[g_leg]);
        Go(CStep::WaitDone);
        return;
    case CStep::WaitDone:
        if (!ClientLegDone(g_leg)) {
            if (now - g_stepMs > kDoneBoundMs) {
                UE_LOGW("[DESK-VERB-DRILL] client leg %s FAIL: its result never reached this desk", kLegName[g_leg]);
                g_clientPass = false;
                g_client = CStep::Done;
                UE_LOGI("[DESK-VERB-DRILL] client DONE FAIL");
            }
            return;
        }
        ClientJudge(g_leg);
        g_stand = 0;
        g_ring.clear();
        if (++g_leg < kLegs) {
            Go(CStep::Walk);
            return;
        }
        UE_LOGI("[DESK-VERB-DRILL] client DONE %s", g_clientPass ? "PASS" : "FAIL");
        g_client = CStep::Done;
        return;
    default:
        return;
    }
}

// A ready world gives a peer its desk, its saved signals and its player. A world that never does ends the drill on
// the start's bound, counted from the client's readiness on either peer; a moment without them only pauses it.
void Unresolved(coop::net::Session* s, bool host) {
    if (host ? !s->AnyWorldReadyPeer() : !coop::net_pump::HasAnnouncedWorldReady()) return;
    const uint64_t now = ::GetTickCount64();
    if (!g_unresolvedSinceMs) {
        g_unresolvedSinceMs = now;
        return;
    }
    if (now - g_unresolvedSinceMs <= kStartBoundMs) return;
    if (host) HostAbandon("the desk, the saved signals or the host's player did not resolve");
    else Abandon("the desk, the saved signals or this client's player did not resolve");
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (ModeOf() == Mode::Off || !session || !session->running()) return;
    const bool host = session->role() == coop::net::Role::Host;
    // The host runs from its hosting on: the join's host decode is armed before any client connects.
    if (!host && !session->connected()) return;
    if (host ? g_host == HStep::Done : g_client == CStep::Done) return;
    void* player = coop::players::Registry::Get().Local();
    if (!SS::EnsureResolved() || !CD::Instance() || !player) {
        Unresolved(session, host);
        return;
    }
    g_unresolvedSinceMs = 0;
    if (host) HostTick(session, player);
    else ClientTick(player);
}

void TickLoad(coop::net::Session* session) {
    if (g_loadArmed || ModeOf() != Mode::Join || !session || !session->running() ||
        session->role() == coop::net::Role::Host)
        return;
    if (const wchar_t* dead = CensusDead()) {
        g_loadArmed = true;
        UE_LOGW("[DESK-VERB-DRILL] ABANDONED on the client: the census watch on '%ls' is dead, so the join's restore "
                "cannot be counted", dead);
        g_client = CStep::Done;
        return;
    }
    if (CensusNotLive()) return;
    g_loadArmed = true;
    MarkJoinCensus();
    UE_LOGI("[DESK-VERB-DRILL] client: the census is live ahead of the world load");
}

void OnDisconnect() {
    ResetFixtures();
    RefinerReset();
    g_hostRow = 0;
    g_hostDecoding = false;
    g_loadArmed = false;
    g_bandRow = 0;
    g_host = HStep::WaitClient;
    g_hostLeg = kSave;
    g_hostMs = 0;
    g_hostPass = true;
    g_client = CStep::Ready;
    g_leg = kSave;
    g_stepTicks = 0;
    g_stepMs = 0;
    g_aimPose = 0;
    g_stand = 0;
    g_ring.clear();
    g_readySinceMs = 0;
    g_unresolvedSinceMs = 0;
    g_aimTook.clear();
    g_clientPass = true;
    g_walk.reset();
}

}  // namespace coop::dev::desk_verb_drill
