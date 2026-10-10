// coop/dev/drive_drill.cpp -- see coop/dev/drive_drill.h.

#include "coop/dev/drive_drill.h"

#include "drive_drill_verbs.h"  // co-located private header (src tree, not include/)

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/element/registry.h"
#include "coop/interactables/drive_payload_sync.h"
#include "coop/interactables/drive_sync.h"
#include "coop/interactables/eraser_press_intent.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/save/save_transfer.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/desk/drive_eraser.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::drive_drill {
namespace {

namespace DC = ue_wrap::drive_chain;
namespace DE = ue_wrap::drive_eraser;
namespace DP = coop::drive_payload_sync;
namespace DV = coop::dev::drive_drill_verbs;
namespace E  = ue_wrap::engine;
namespace PT = coop::prop_element_tracker;
namespace R  = ue_wrap::reflection;
namespace SD = ue_wrap::signal_dynamic;

const wchar_t* const kFixture   = L"drill-fixture";
const wchar_t* const kDeviation = L"drill-deviation";
const wchar_t* const kBorn      = L"drill-newborn";
const wchar_t* const kForged    = L"drill-forged";
const wchar_t* const kDriveCls  = L"prop_drive_C";
constexpr uint64_t kArmBoundMs   = 60000;  // CLIENT: world-ready to the fixture's row
constexpr uint64_t kLegBoundMs   = 30000;  // a leg's own event, from the step that started it
constexpr uint64_t kWipeBoundMs  = 10000;  // the press to the host's wipe here (the press's own 3 s, and the wire)
constexpr uint64_t kHoldMs       = 3000;
constexpr uint64_t kCheckEveryMs = 250;
// The press needs this client's body within the host's 400 uu of the eraser, so it walks to a standpoint on a ring
// about the eraser that a NavMesh route reaches, shortest route first, and steps to the next when a walk does not
// arrive: a point inside the machine has a route that ends short.
constexpr int      kStandpoints        = 8;
constexpr float    kStandRingCm        = 150.f;
constexpr float    kStandReachCm       = 60.f;
constexpr int      kFirstWalkDeadlineS = 300;  // from wherever this client stands in its world
constexpr int      kNextWalkDeadlineS  = 30;   // a step round the eraser
// The client's drives that must reach the host with their row: its birth, its take-out and its throw.
constexpr int kOwnDrives = 3;

enum class Step : uint8_t { Arm, PutBack, Walk, Seat, SeatLine, Wipe, Show, Born, TakeOut, HoldIt, Thrown, Forge,
                            Hold, Done };
constexpr int kPocketTries = 8;  // candidates the pocket may refuse (asleep, static, a full inventory)
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
uint64_t g_nextCheckMs = 0;
int      g_session = 1;
bool     g_hostArmed = false;
bool     g_hostDone = false;
void*    g_fixture = nullptr;   // this peer's copy of the fixture
void*    g_own = nullptr;       // CLIENT: the drive of its current leg
std::wstring g_ownKey;          // CLIENT: the carried drive's key, its inventory record's
std::wstring g_ownName;         // CLIENT: the carried drive's row name
uint64_t g_mark = 0;            // CLIENT: a counter's value when the step began
std::shared_ptr<coop::director::BackgroundWalk> g_walk;  // CLIENT: the walk to the eraser, on a worker
std::vector<ue_wrap::FVector> g_ring;
size_t   g_stand = 0;
// HOST: after its wipe, when the last of the client's rows came (taken or refused).
uint64_t g_progressMs = 0;
uint64_t g_progressCount = 0;

const std::string& Mode() {
    static const std::string s = coop::config::ResolveString(::coop::config_registry::rows::drive_drill);
    return s;
}
bool Join() { return Mode() == "join"; }
bool Obs() { return Mode() == "obs"; }  // run's steps, begun once a second client, the watcher, stands too
bool Enabled() { return Mode() == "run" || Join() || Obs(); }
bool Expired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }
void Next(Step s) { g_step = s; g_stepMs = ::GetTickCount64(); }

void Fail(const char* what) {
    UE_LOGW("[DRIVE-DRILL] FAIL in session %d: %s", g_session, what);
    g_step = Step::Done;
    g_hostDone = true;
}
void Abandon(const char* why) {
    UE_LOGW("[DRIVE-DRILL] ABANDONED in session %d: %s", g_session, why);
    g_step = Step::Done;
    g_hostDone = true;
}

std::wstring NameOf(void* drive) {
    SD::Row row;
    return drive && R::IsLive(drive) && DC::ReadDriveRow(drive, row) ? row.name : std::wstring(L"<unread>");
}
float SizeOf(void* drive) {
    SD::Row row;
    return drive && R::IsLive(drive) && DC::ReadDriveRow(drive, row) ? row.size : -1.f;
}

// The live drives whose row carries `name`, among the registry's props, with their eids.
std::vector<coop::element::Registry::ActorIdPair> DrivesNamed(const wchar_t* name) {
    std::vector<coop::element::Registry::ActorIdPair> pairs, out;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    for (const auto& p : pairs) {
        if (!p.actor || !R::IsLiveByIndex(p.actor, p.internalIdx) || !DC::IsDriveClass(R::ClassOf(p.actor))) continue;
        if (NameOf(p.actor) == name) out.push_back(p);
    }
    return out;
}

// HOST (join): a live drive in no slot, to carry the fixture row: one the joiner's world holds, since it came with the
// host's world, so only the row is new to it.
void* UnseatedDrive() {
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    for (const auto& p : pairs) {
        if (!p.actor || !R::IsLiveByIndex(p.actor, p.internalIdx) || !DC::IsDriveClass(R::ClassOf(p.actor))) continue;
        bool seated = false;
        for (int r = 0; r < DC::kRoleCount && !seated; ++r)
            if (void* slot = DC::SlotActor(r)) seated = DC::SlotDrive(slot) == p.actor;
        if (!seated) return p.actor;
    }
    return nullptr;
}

// Writes `name` and `size` into a drive and refreshes it as a game writer does.
bool WriteRow(void* d, const wchar_t* name, float size) {
    SD::Row row;
    if (!d || !DC::ReadDriveRow(d, row)) return false;
    row.name = name;
    row.id = std::wstring(name) + L"-ID";
    row.size = size;
    row.decoded = size;
    row.level = 2;
    if (!DC::WriteDriveRow(d, row)) return false;
    DC::CallDriveUpd(d);
    return true;
}

// Writes `name` into a drive's row without refreshing it: no game writer's upd, so nothing puts it back.
bool WriteRowRaw(void* d, const wchar_t* name) {
    SD::Row row;
    if (!d || !DC::ReadDriveRow(d, row)) return false;
    row.name = name;
    row.size = 1.f;
    row.decoded = 1.f;
    return DC::WriteDriveRow(d, row);
}

// A drive at the local player, raised by `dz`, written with `name` and `size` and refreshed as a game writer does.
void* SpawnDrive(float dz, const wchar_t* name, float size) {
    void* player = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    if (!player || !E::TryGetActorLocation(player, at) || !DC::DriveClass()) return nullptr;
    void* d = E::SpawnActor(DC::DriveClass(), {at.X, at.Y, at.Z + dz});
    return WriteRow(d, name, size) ? d : nullptr;
}

// CLIENT: its own drive's row left for the host since the step began (the lane counts each).
bool OwnRowSent() { return DP::LaneCounts().ownSent > g_mark; }

// CLIENT: pockets a world drive with a row of its own, as a player gathers one: the pocket refuses a drive asleep or
// static, so candidates are tried in order. True with the carried drive's key and row name.
bool PocketAWorldDrive(void* player) {
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    int tries = 0;
    for (const auto& p : pairs) {
        if (tries >= kPocketTries) break;
        if (!p.actor || p.actor == g_fixture || p.actor == g_own || !R::IsLiveByIndex(p.actor, p.internalIdx) ||
            !DC::IsDriveClass(R::ClassOf(p.actor)))
            continue;
        SD::Row row;
        if (!DC::ReadDriveRow(p.actor, row) || row.size <= 0.f || row.name.empty()) continue;
        bool seated = false;
        for (int r = 0; r < DC::kRoleCount && !seated; ++r)
            if (void* slot = DC::SlotActor(r)) seated = DC::SlotDrive(slot) == p.actor;
        if (seated) continue;
        ++tries;
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(p.actor);
        if (!DV::Pocket(player, p.actor)) continue;
        g_ownKey = key;
        g_ownName = row.name;
        return true;
    }
    return false;
}

// The first client runs the steps; a second, the rig's observer, only watches: the lanes it hears the first client's
// edits through are the host's relays, which its own drive lane's log names by their source slot.
bool g_observerSaid = false;

void ClientTick() {
    const uint64_t now = ::GetTickCount64();
    if (g_step == Step::Done || now < g_nextCheckMs) return;
    g_nextCheckMs = now + kCheckEveryMs;
    const uint8_t mine = coop::players::Registry::Get().LocalPeerId();
    if (mine > 1) {
        if (!g_observerSaid) {
            g_observerSaid = true;
            UE_LOGI("[DRIVE-DRILL] client in slot %u: an observer, it runs no step and watches the drive lane", mine);
        }
        return;
    }
    void* player = coop::players::Registry::Get().Local();
    switch (g_step) {
    case Step::Arm: {
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle) {
            g_stepMs = now;
            return;
        }
        const auto found = DrivesNamed(kFixture);
        if (found.empty()) {
            if (Expired(kArmBoundMs)) Fail("no drive here reads the host's fixture row 60 s after this world was ready");
            return;
        }
        if (DP::LaneCounts().parkedExpired > 0) {
            Fail("a drive row the host sent expired here before its drive bound");
            return;
        }
        g_fixture = found.front().actor;
        UE_LOGI("[DRIVE-DRILL] client: the fixture reads '%ls' here (a row parked for at most %llu ms)", kFixture,
                static_cast<unsigned long long>(DP::LaneCounts().maxParkedMs));
        Next(Step::PutBack);
        return;
    }
    case Step::PutBack: {
        const uint64_t before = DP::LaneCounts().putBack;
        SD::Row row;
        if (!DC::ReadDriveRow(g_fixture, row)) {
            Abandon("the fixture's row could not be read");
            return;
        }
        row.name = kDeviation;
        row.size = 3.f;
        if (!DC::WriteDriveRow(g_fixture, row)) {
            Abandon("the deviation could not be written into the fixture");
            return;
        }
        DC::CallDriveUpd(g_fixture);  // as a game writer does; the lane's POST puts it back inside this call
        if (NameOf(g_fixture) != kFixture || DP::LaneCounts().putBack <= before) {
            Fail("the client's deviation on the fixture stood after its upd() -- not put back");
            return;
        }
        UE_LOGI("[DRIVE-DRILL] client: the deviation was put back inside its own upd()");
        void* eraser = DE::Instance();
        if (!eraser || !player) {
            Abandon("this client's world has no eraser, or no player, to walk with");
            return;
        }
        ue_wrap::FVector at{};
        g_ring = E::TryGetActorLocation(eraser, at)
                     ? coop::director::ReachableStandpoints(player, at, kStandRingCm, kStandpoints, kStandReachCm)
                     : std::vector<ue_wrap::FVector>();
        UE_LOGI("[DRIVE-DRILL] client: a route reaches %zu of the %d standpoints about the eraser", g_ring.size(),
                kStandpoints);
        if (g_ring.empty()) {
            Abandon("no NavMesh route reaches a standpoint about the eraser");
            return;
        }
        g_stand = 0;
        g_walk = coop::director::StartBackgroundWalk(g_ring[0], kStandReachCm, kFirstWalkDeadlineS);
        Next(Step::Walk);
        return;
    }
    case Step::Walk: {
        const int state = g_walk ? g_walk->state.load() : 2;
        if (state == 0) return;
        if (state == 2) {
            if (++g_stand >= g_ring.size()) {
                Abandon("no standpoint about the eraser was reached");
                return;
            }
            UE_LOGI("[DRIVE-DRILL] client: the walk did not arrive -- stepping to standpoint %zu of %zu", g_stand + 1,
                    g_ring.size());
            g_walk = coop::director::StartBackgroundWalk(g_ring[g_stand], kStandReachCm, kNextWalkDeadlineS);
            return;
        }
        UE_LOGI("[DRIVE-DRILL] client: walked to standpoint %zu beside the eraser in %llu ms", g_stand + 1,
                static_cast<unsigned long long>(now - g_stepMs));
        Next(Step::Seat);
        return;
    }
    case Step::Seat: {
        void* slot = DC::SlotActor(DC::kRoleEraser);
        g_mark = coop::drive_sync::AnnouncedCount();
        if (!slot || !DC::CallPutDriveIn(slot, g_fixture)) {
            Abandon("the fixture could not be seated in this client's eraser");
            return;
        }
        Next(Step::SeatLine);
        return;
    }
    case Step::SeatLine: {
        // A player presses after its insert has gone out: the press follows the slot line on their lane.
        if (coop::drive_sync::AnnouncedCount() <= g_mark) {
            if (Expired(kLegBoundMs)) Abandon("the eraser's slot line never went out from this client");
            return;
        }
        const uint64_t sent = coop::eraser_press_intent::SentCount();
        g_mark = coop::eraser_press_intent::ShownCount();
        if (!DE::PressDelete(DE::Instance(), player)) {
            Abandon("this client's eraser press could not be called");
            return;
        }
        if (coop::eraser_press_intent::SentCount() <= sent) {
            Fail("the eraser press ran here instead of reaching the host");
            return;
        }
        UE_LOGI("[DRIVE-DRILL] client: the fixture seated and the delete pressed; the host must wipe it");
        Next(Step::Wipe);
        return;
    }
    case Step::Wipe:
        if (SizeOf(g_fixture) == 0.f && NameOf(g_fixture).empty()) {
            UE_LOGI("[DRIVE-DRILL] client: the host's wipe reached the fixture after %llu ms",
                    static_cast<unsigned long long>(now - g_stepMs));
            Next(Step::Show);
        } else if (Expired(kWipeBoundMs)) {
            Fail("the fixture was not wiped within 10 s of the delete press");
        }
        return;
    case Step::Show:
        // The host's press and its resume, shown by this client's own eraser: a start and a done.
        if (coop::eraser_press_intent::ShownCount() < g_mark + 2) {
            if (Expired(kLegBoundMs)) Fail("the host's eraser press and its done never showed on this client's eraser");
            return;
        }
        g_mark = DP::LaneCounts().ownSent;
        g_own = SpawnDrive(90.f, kBorn, 5.f);
        if (!g_own) {
            Abandon("a drive could not be spawned at this client's player");
            return;
        }
        UE_LOGI("[DRIVE-DRILL] client: the eraser showed the host's press; spawned a drive with the row '%ls'", kBorn);
        Next(Step::Born);
        return;
    case Step::Born:
        if (!OwnRowSent()) {
            if (Expired(kLegBoundMs)) Fail("the spawned drive's row never went to the host");
            return;
        }
        // The re-placement: a world drive with a row, gathered into the inventory and taken back out, as the
        // inventory screen does.
        if (!PocketAWorldDrive(player)) {
            Abandon("no world drive with a row could be pocketed");
            return;
        }
        UE_LOGI("[DRIVE-DRILL] client: the spawned drive's row went to the host; a world drive reading '%ls' pocketed",
                g_ownName.c_str());
        Next(Step::TakeOut);
        return;
    case Step::TakeOut:
        g_mark = DP::LaneCounts().ownSent;
        g_own = DV::TakeOut(kDriveCls, g_ownKey);
        if (!g_own || NameOf(g_own) != g_ownName) {
            Abandon("the pocketed drive did not come back out of the inventory with its row");
            return;
        }
        UE_LOGI("[DRIVE-DRILL] client: the pocketed drive taken back out into the world, reading '%ls'",
                g_ownName.c_str());
        Next(Step::HoldIt);
        return;
    case Step::HoldIt:
        if (!OwnRowSent()) {
            if (Expired(kLegBoundMs)) Fail("the taken-out drive's row never went to the host");
            return;
        }
        if (!DV::Hold(player, g_own)) {
            Abandon("the taken-out drive could not be taken into the hand");
            return;
        }
        g_mark = DP::LaneCounts().ownSent;
        if (!DV::Throw(player)) {
            Abandon("the hand's drive could not be thrown");
            return;
        }
        UE_LOGI("[DRIVE-DRILL] client: the taken-out drive's row went to the host; the drive taken into the hand and "
                "thrown, which the game does by a new actor's birth");
        Next(Step::Thrown);
        return;
    case Step::Thrown:
        if (!OwnRowSent()) {
            if (Expired(kLegBoundMs)) Fail("the thrown drive's row never went to the host");
            return;
        }
        UE_LOGI("[DRIVE-DRILL] client: the thrown drive's row went to the host");
        // A modified client's claim on a drive it did not bring: the wiped fixture, forged here and sent as its own.
        if (!WriteRowRaw(g_fixture, kForged) || !DP::DevClaimRow(g_fixture)) {
            Abandon("the forged row could not be written or sent");
            return;
        }
        Next(Step::Forge);
        return;
    case Step::Forge:
        // The host refuses it and answers with its own row, which this copy takes back.
        if (NameOf(g_fixture) == kForged) {
            if (Expired(kLegBoundMs)) Fail("the host did not answer the forged row with its own");
            return;
        }
        UE_LOGI("[DRIVE-DRILL] client: the host answered the forged row with its own");
        Next(Step::Hold);
        return;
    case Step::Hold:
        if (SizeOf(g_fixture) != 0.f) {
            Fail("the wiped fixture took a row again");
            return;
        }
        if (!Expired(kHoldMs)) return;
        UE_LOGI("[DRIVE-DRILL] client DONE in session %d (%s): the deviation went back, the eraser press was the host's "
                "and showed here, the drive's row went to the host at its birth, its take-out and its throw, and "
                "a forged row came back as the host's -- PASS", g_session, Mode().c_str());
        g_step = Step::Done;
        return;
    default:
        return;
    }
}

void HostTick(coop::net::Session* s) {
    const uint64_t now = ::GetTickCount64();
    if (g_hostDone || now < g_nextCheckMs) return;
    g_nextCheckMs = now + kCheckEveryMs;
    if (!g_hostArmed) {
        if (!s->running() || !DC::EnsureResolved()) return;
        if (Join()) {
            // The row is written in a joiner's window: its world taken (the live capture carries no later write) and
            // not yet ready (a broadcast skips such a slot), so the lane's seed at its world-ready is its one way in.
            int joiner = -1;
            for (int i = 1; i < coop::net::kMaxPeers && joiner < 0; ++i)
                if (coop::save_transfer::WorldTakenFor(i) && !s->IsSlotWorldReady(i)) joiner = i;
            if (joiner < 0) {
                if (s->AnyWorldReadyPeer()) Abandon("a joiner's world was ready before the host saw its window");
                return;
            }
            if (!PT::HasSeededOnce() || !PT::IsRegistrySeededForCurrentWorld()) return;
            g_fixture = UnseatedDrive();
            if (!WriteRow(g_fixture, kFixture, 7.f)) {
                Abandon("no drive in the host's world could carry the fixture row");
                return;
            }
        } else {
            int ready = 0;
            for (int i = 1; i < coop::net::kMaxPeers; ++i) ready += s->IsSlotWorldReady(i) ? 1 : 0;
            if (ready < (Obs() ? 2 : 1)) return;
            g_fixture = SpawnDrive(60.f, kFixture, 7.f);
        }
        if (!g_fixture) {
            Abandon("the host's fixture drive could not be spawned");
            return;
        }
        g_hostArmed = true;
        UE_LOGI("[DRIVE-DRILL] host (%s): the fixture drive carries the row '%ls' (%s)", Mode().c_str(), kFixture,
                Join() ? "a drive of the host's world, written while the joiner loads it" : "spawned");
        return;
    }
    if (NameOf(g_fixture) == kDeviation) {
        Fail("the host's fixture took the client's deviation");
        return;
    }
    if (NameOf(g_fixture) == kForged) {
        Fail("the host took a client's row for a drive that client did not bring");
        return;
    }
    // The legs before the wipe are bounded on the client (the walk's deadline, the wipe's 10 s).
    const bool wiped = SizeOf(g_fixture) == 0.f && coop::eraser_press_intent::PressedCount() >= 1;
    if (!wiped) return;
    // The client's drives reach the host with their rows, each taken once from it; its forged row is refused.
    const DP::Counts c = DP::LaneCounts();
    const uint64_t progress = c.accepted + c.refused;
    if (g_progressMs == 0 || progress != g_progressCount) {
        g_progressMs = now;
        g_progressCount = progress;
    }
    const bool refused = c.refused >= 1;
    if (static_cast<int>(c.accepted) >= kOwnDrives && refused) {
        g_hostDone = true;
        UE_LOGI("[DRIVE-DRILL] host DONE in session %d (%s): the client's eraser press wiped the fixture here, its "
                "drive's row came with its birth, its take-out and its throw, and its forged row was refused",
                g_session, Mode().c_str());
        return;
    }
    if (now - g_progressMs > kLegBoundMs) {
        static const char* const kLeg[] = {"its birth", "its take-out", "its throw"};
        char why[160];
        if (static_cast<int>(c.accepted) < kOwnDrives)
            std::snprintf(why, sizeof(why), "the client's drive did not reach the host with its row at %s within 30 s",
                          kLeg[c.accepted]);
        else
            std::snprintf(why, sizeof(why), "the client's forged row did not reach the host within 30 s of its throw");
        Fail(why);
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (!Enabled() || !s) return;
    if (s->role() == coop::net::Role::Host) HostTick(s);
    else if (s->connected()) ClientTick();
}

void OnDisconnect() {
    if (!Enabled()) return;
    ++g_session;
    g_step = Step::Arm;
    g_stepMs = g_nextCheckMs = 0;
    g_hostArmed = g_hostDone = false;
    g_fixture = g_own = nullptr;
    g_ownKey.clear();
    g_ownName.clear();
    g_mark = 0;
    g_walk.reset();
    g_ring.clear();
    g_stand = 0;
    g_progressMs = 0;
    g_progressCount = 0;
    g_observerSaid = false;
}

}  // namespace coop::dev::drive_drill
