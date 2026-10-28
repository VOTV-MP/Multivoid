// coop/dev/pr64_drill_lid.cpp -- see coop/dev/pr64_drill.h: the lid arm. The host spawns a drive box in front of its own
// player with one drive in slot 0 (the box's own putDriveIn, called through reflection: a call of ours, so it publishes
// nothing) and changes it as a player does, twice, back to back with the use key held (a tap does nothing on a loose
// box): A opens the lid, B takes the drive out of the aimed slot. The box's record publishes after each change; the
// host marks them in its log and the publishes between the marks are counted from the log, since the publishing code
// keeps its counter to itself. The host says the pair it ended with, (opened, drive0); the client reads its own copy for
// up to kNearBoundMs and says whether the two agree.
//
// The host prints PASS when it hears the client's line and FAIL when it polls the client's FAIL line first, so on a red
// the two chat lines race and the host's verdict is not a marker; the rig judges the arm by the client's line.

#include "pr64_drill_detail.h"
#include "pr64_drill_lid.h"

#include "coffee_drill_detail.h"  // co-located private header: KeyRides

#include "coop/dev/director/routes.h"
#include "coop/dev/drill_props.h"
#include "coop/dev/game_window.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/desk/signal_dynamic.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::pr64_drill::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace DR = coop::director;
namespace DP = coop::dev::drill_props;
namespace DC = ue_wrap::drive_chain;
namespace EL = coop::element;
namespace CD = coop::dev::coffee_drill::detail;
using ue_wrap::FVector;

const wchar_t* const kBoxPath = L"/Game/objects/prop_box.prop_box_C";

// The box's state the two peers compare: its lid, and whether slot 0 holds a drive (-1: does not read).
struct Pair {
    int opened = -1, drive0 = -1;
    bool operator==(const Pair&) const = default;
};

// One process is one role, so one step and one clock serve both: the clock is stamped when a step begins.
enum class Step : uint8_t {
    Idle,
    Spawn, Fixture, Key, Found, DownA, HoldA, DownB, HoldB, WaitFinal,   // the host
    Hear, Find, Watch, After                                             // the client
};
struct State {
    Step step = Step::Idle;
    uint64_t stepMs = 0, nextFindMs = 0;
    uint64_t downMs = 0, aMs = 0;   // the host's marks; downMs is the time base of its lines
    Pair seen{-2, -2};              // the last pair printed
    Pair host;                      // the pair the host said
    std::string key;
    ue_wrap::CachedObjRef box, drive;
};
State g_l;

void Go(Step s) {
    g_l.step = s;
    g_l.stepMs = NowMs();
}
unsigned long long Since() { return static_cast<unsigned long long>(NowMs() - g_l.stepMs); }
// Milliseconds on the host's lines count from the first key down; the client has no such mark and counts from its step.
unsigned long long Lap() { return static_cast<unsigned long long>(NowMs() - (g_l.downMs ? g_l.downMs : g_l.stepMs)); }

Pair ReadPair(void* box) { return {ReadOpened(box), ReadDrive0(box)}; }

// Prints the box's pair when it differs from the last one printed.
void Note(const char* role, void* box) {
    const Pair p = ReadPair(box);
    if (p == g_l.seen) return;
    g_l.seen = p;
    UE_LOGI("[PR64-DRILL] %s lid +%llums opened=%d drive0=%d", role, Lap(), p.opened, p.drive0);
}

std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }

// ---- the host ----

void HostSpawn() {
    void* me = coop::players::Registry::Get().Local();
    FVector mePos{}, at{};
    float fz = 0.f, len = 0.f;
    if (!me || !E::TryGetActorLocation(me, mePos)) {
        Fail("the host's player cannot be read");
        return;
    }
    const FVector fwd = E::GetActorForwardVector(me);
    at = {mePos.X + fwd.X * kBoxAheadCm, mePos.Y + fwd.Y * kBoxAheadCm, mePos.Z};
    if (!DP::FloorAt(me, at.X, at.Y, mePos.Z + kFromUpCm, kDownCm, kMinColumnCm, &fz) ||
        !DR::RouteInLegs(me, mePos, {at.X, at.Y, fz}, kStandCm, &len)) {
        Fail("no reachable floor %.0f cm in front of the host's player", static_cast<double>(kBoxAheadCm));
        return;
    }
    void* cls = ue_wrap::asset_load::LoadObjectByPath(kBoxPath);
    void* box = cls ? E::SpawnActor(cls, {at.X, at.Y, fz + kBoxRiseCm}) : nullptr;
    if (!box) {
        Fail("the drive box did not spawn (class %p)", cls);
        return;
    }
    g_l.box.Set(box);
    UE_LOGI("[PR64-DRILL] host box spawned at (%.0f,%.0f,%.0f), a route of %.0f cm", at.X, at.Y, fz + kBoxRiseCm,
            static_cast<double>(len));
    Go(Step::Fixture);
}

// One drive into slot 0, by the box's own putDriveIn. The call is the drill's own, so the box's record watch skips its
// upd(); the far peer learns the drive from the first publish. It waits for the box's arrays, then spawns the drive and
// waits for the element lane to name it (a drive written before it is named is not the drive the lanes will see).
void HostFixture() {
    void* box = g_l.box.Get();
    if (!box) {
        Fail("the box was gone before its drive went in");
        return;
    }
    if (!g_l.drive.Get()) {
        void* slot = SlotComp(box, 0);
        FVector at{};
        if (!DC::EnsureResolved() || !slot || Count(box, L"drives_in") < 1 || !E::TryGetActorLocation(box, at)) {
            if (Late(kFixtureBoundMs))
                Fail("the box is not ready for its drive after %llu ms (drive chain %d, boxesTr %d, drives_in %d)", Since(),
                     DC::EnsureResolved() ? 1 : 0, Count(box, L"boxesTr"), Count(box, L"drives_in"));
            return;
        }
        void* drive = E::SpawnActor(DC::DriveClass(), {at.X, at.Y, at.Z + kDriveRiseCm});
        if (!drive) {
            Fail("the fixture drive did not spawn");
            return;
        }
        g_l.drive.Set(drive);
        return;
    }
    void* drive = g_l.drive.Get();
    const EL::ElementId eid = EL::Registry::Get().EidForActor(drive);
    if (eid == EL::kInvalidId || eid == 0u) {
        if (Late(kFixtureBoundMs)) Fail("the element lane never named the fixture drive");
        return;
    }
    ue_wrap::signal_dynamic::Row row;
    if (!DC::ReadDriveRow(drive, row)) {
        Fail("the fixture drive's row cannot be read");
        return;
    }
    row.name = L"pr64";
    row.id = L"pr64-ID";
    row.size = 10.f;
    row.decoded = 10.f;
    row.level = 2;
    void* fn = R::FindFunction(R::ClassOf(box), L"putDriveIn");
    ue_wrap::ParamFrame f(fn);
    if (!DC::WriteDriveRow(drive, row) || !DC::CallDriveUpd(drive) || !fn || !f.valid() || !f.Set<void*>(L"Object", drive) ||
        !f.Set<void*>(L"lookAt", SlotComp(box, 0)) || !f.Set<bool>(L"ignoreNotif", false) || !ue_wrap::Call(box, f)) {
        Fail("the fixture drive's row or the box's putDriveIn did not run (putDriveIn %p)", fn);
        return;
    }
    if (ReadDrive0(box) != 1) {
        Fail("the box did not take the drive into slot 0 (drives_in[0]=%d, boxesTr %d, drives_in %d)", ReadDrive0(box),
             Count(box, L"boxesTr"), Count(box, L"drives_in"));
        return;
    }
    UE_LOGI("[PR64-DRILL] host fixture: the drive (eid=%u) is in slot 0, opened=%d", static_cast<unsigned>(eid), ReadOpened(box));
    Go(Step::Key);
}

// The box's interactable key, minted in its construction; the client finds the box by it.
void HostKey() {
    const std::wstring k = ue_wrap::prop::GetInteractableKeyString(g_l.box.Get());
    if (!CD::KeyRides(k)) {
        if (Late(kKeyBoundMs)) Fail("the box got no key that rides a chat line");
        return;
    }
    g_l.key = Narrow(k);
    LogSlotState(g_l.box.Get());
    Say(kLidReady + g_l.key);
    UE_LOGI("[PR64-DRILL] host lid-ready key=%s opened=%d", g_l.key.c_str(), ReadOpened(g_l.box.Get()));
    Go(Step::Found);
}

// The first press waits for the client to hold the box, so its changes are all read.
void HostFound() {
    std::string rest;
    if (Rest(kLidFound, &rest)) {
        UE_LOGI("[PR64-DRILL] host: the client holds the box, opened=%s", rest.c_str());
        Go(Step::DownA);
    } else if (Late(kFoundBoundMs)) {
        Fail("the client never found the box");
    }
}

// Change A, the open: aims at the box and puts the use key down (a tap on a loose box does nothing: the game starts a
// hold on the press and acts on it only while the key is still down).
void HostDownA() {
    void* box = g_l.box.Get();
    FVector at{};
    if (!box || !E::TryGetActorLocation(box, at)) {
        Fail("the box is gone");
        return;
    }
    if (!Aimed(at, box, nullptr, "box for the open")) return;
    g_l.seen = {-2, -2};
    g_l.downMs = NowMs();
    const Pair p = ReadPair(box);
    UE_LOGI("[PR64-DRILL] host lid mark A key down: opened=%d drive0=%d", p.opened, p.drive0);
    if (p.opened != 0 || p.drive0 != 1) {
        Fail("the box did not start closed with its drive in (opened=%d, drive0=%d)", p.opened, p.drive0);
        return;
    }
    if (!coop::dev::game_window::PostKeyDown(kUseKey)) {
        Fail("the key down could not be posted to the game window");
        return;
    }
    Go(Step::HoldA);
}

// The key stays down, the box kept under the crosshair, until the box reads open; then it is released.
void HostHoldA() {
    void* box = g_l.box.Get();
    FVector at{};
    if (box && E::TryGetActorLocation(box, at)) Aim(coop::players::Registry::Get().Local(), at);
    Note("host", box);
    if (ReadOpened(box) == 1) {
        g_l.aMs = NowMs();
        coop::dev::game_window::PostKeyUp(kUseKey);
        UE_LOGI("[PR64-DRILL] host lid mark A landed: opened=1 at +%llums, key released", Lap());
        Go(Step::DownB);
    } else if (Late(kMutateBoundMs)) {
        coop::dev::game_window::PostKeyUp(kUseKey);
        Fail("the box never opened in %llu ms of held E (opened=%d)", Since(), ReadOpened(box));
    }
}

// Change B, the drive out: aims at slot 0's component (sweeping the aim when the trace will not take it), and puts the
// use key down once the trace takes it.
void HostDownB() {
    void* box = g_l.box.Get();
    void* slot = SlotComp(box, 0);
    if (!slot) {
        Fail("slot 0's component cannot be read");
        return;
    }
    Note("host", box);
    if (!SlotAimed(box, slot)) return;
    UE_LOGI("[PR64-DRILL] host lid mark B key down: opened=%d drive0=%d, %llu ms after A landed", ReadOpened(box),
            ReadDrive0(box), static_cast<unsigned long long>(NowMs() - g_l.aMs));
    if (!coop::dev::game_window::PostKeyDown(kUseKey)) {
        Fail("the key down could not be posted to the game window");
        return;
    }
    Go(Step::HoldB);
}

// The key stays down, slot 0 kept under the crosshair, until the slot reads empty; then it is released and the host
// says the pair it ended with.
void HostHoldB() {
    void* box = g_l.box.Get();
    HoldSlotAim(box, SlotComp(box, 0));
    Note("host", box);
    if (ReadDrive0(box) == 0) {
        coop::dev::game_window::PostKeyUp(kUseKey);
        const Pair p = ReadPair(box);
        UE_LOGI("[PR64-DRILL] host lid mark B landed: drive0=0 at +%llums, %llu ms after A landed", Lap(),
                static_cast<unsigned long long>(NowMs() - g_l.aMs));
        UE_LOGI("[PR64-DRILL] host lid mark end: opened=%d drive0=%d", p.opened, p.drive0);
        if (p.opened != 1) {
            Fail("change B landed but the lid is not open (opened=%d)", p.opened);
            return;
        }
        char line[48];
        std::snprintf(line, sizeof(line), "%s%d %d", kLidHost, p.opened, p.drive0);
        Say(line);
        Go(Step::WaitFinal);
    } else if (Late(kMutateBoundMs)) {
        coop::dev::game_window::PostKeyUp(kUseKey);
        Fail("the drive never left slot 0 in %llu ms of held E (drive0=%d)", Since(), ReadDrive0(box));
    }
}

void HostWaitFinal() {
    std::string rest;
    if (Rest(kLidClient, &rest)) {
        UE_LOGI("[PR64-DRILL] host client's final pair: %s", rest.c_str());
        Pass();
    } else if (Late(kFinalBoundMs + kNearBoundMs)) {
        Fail("the client never said its final numbers");
    }
}

// ---- the client ----

void ClientHear() {
    std::string rest;
    if (Rest(kLidReady, &rest)) {
        g_l.key = rest;
        Go(Step::Find);
    } else if (Late(kHearBoundMs)) {
        Fail("the host never said lid-ready");
    }
}

// The box by its key; a miss catches the object index up, so it is asked a few times a second.
void ClientFind() {
    const uint64_t now = NowMs();
    if (now < g_l.nextFindMs) return;
    g_l.nextFindMs = now + 250;
    void* box = coop::prop_key_index::Find(std::wstring(g_l.key.begin(), g_l.key.end()));
    if (!box) {
        if (Late(kFindBoundMs)) Fail("no box with key %s here after %llu ms", g_l.key.c_str(), Since());
        return;
    }
    g_l.box.Set(box);
    g_l.seen = {-2, -2};
    UE_LOGI("[PR64-DRILL] client holds the box key=%s after %llu ms: class=%ls opened=%d drive0=%d", g_l.key.c_str(), Since(),
            R::ClassNameOf(box).c_str(), ReadOpened(box), ReadDrive0(box));
    char line[32];
    std::snprintf(line, sizeof(line), "%s%d", kLidFound, ReadOpened(box));
    Say(line);
    Go(Step::Watch);
}

// Every change of the pair, from the box found, until the host's word.
void ClientWatch() {
    Note("client", g_l.box.Get());
    std::string rest;
    double n[2];
    if (Rest(kLidHost, &rest) && Numbers(rest, 0, n, 2)) {
        g_l.host = {static_cast<int>(n[0]), static_cast<int>(n[1])};
        UE_LOGI("[PR64-DRILL] client heard the host's pair: opened=%d drive0=%d", g_l.host.opened, g_l.host.drive0);
        Go(Step::After);
    }
}

// The far box has up to kNearBoundMs to read the host's pair; its own pair and the wait are said either way.
void ClientAfter() {
    void* box = g_l.box.Get();
    Note("client", box);
    const Pair mine = ReadPair(box);
    const bool same = mine == g_l.host;
    if (!same && !Late(kNearBoundMs)) return;
    char line[64];
    std::snprintf(line, sizeof(line), "%s%d %d %llu", kLidClient, mine.opened, mine.drive0, Since());
    Say(line);
    UE_LOGI("[PR64-DRILL] client final pair: opened=%d drive0=%d after %llu ms", mine.opened, mine.drive0, Since());
    if (same) Pass();
    else Fail("the far box ended (opened=%d, drive0=%d), the host's is (opened=%d, drive0=%d)", mine.opened, mine.drive0,
              g_l.host.opened, g_l.host.drive0);
}

}  // namespace

void LidHostTick(coop::net::Session* s) {
    if (!s->AnyWorldReadyPeer()) return;
    if (g_l.step == Step::Idle) Go(Step::Spawn);
    if (Heard(kClientFail)) {
        Fail("the client failed");
        return;
    }
    switch (g_l.step) {
    case Step::Spawn:     HostSpawn(); return;
    case Step::Fixture:   HostFixture(); return;
    case Step::Key:       HostKey(); return;
    case Step::Found:     HostFound(); return;
    case Step::DownA:     HostDownA(); return;
    case Step::HoldA:     HostHoldA(); return;
    case Step::DownB:     HostDownB(); return;
    case Step::HoldB:     HostHoldB(); return;
    case Step::WaitFinal: HostWaitFinal(); return;
    default:              return;
    }
}

void LidClientTick() {
    if (!ClientReady()) return;
    if (g_l.step == Step::Idle) Go(Step::Hear);
    if (Heard(kHostFail)) {
        Fail("the host failed");
        return;
    }
    switch (g_l.step) {
    case Step::Hear:  ClientHear(); return;
    case Step::Find:  ClientFind(); return;
    case Step::Watch: ClientWatch(); return;
    case Step::After: ClientAfter(); return;
    default:          return;
    }
}

void LidReset() {
    g_l = State{};
    AimReset();
}

bool Late(uint64_t boundMs) { return Expired(g_l.stepMs, boundMs); }

}  // namespace coop::dev::pr64_drill::detail
