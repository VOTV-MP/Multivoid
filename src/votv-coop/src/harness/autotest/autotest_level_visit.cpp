// harness/autotest/autotest_level_visit.cpp -- TEST-ONLY: a clip's trip to another map is the clipping player's alone,
// and comes back as a death does (coop/player/level_visit). The level_visit_drill row names the arm and its sibling rows
// shape the visit; the red arms are level_visit_hide_off and backrooms_close_off.
// client / host: the visitor takes the trip, stays until the watcher has judged its copy of the visitor hidden by the
// poses it applies, ends the visit and comes back; the watcher judges the copy hidden at the first pose below the visit
// floor and shown at the first pose back above it. close: the host closes the backrooms, the client's clip check is answered no, and after
// the host opens them again it takes a trip. join: the host takes the trip and stays; a client that joins later must
// find the host's copy hidden at the first pose it applies.
// Every step ends on the other peer's signal (peer_signal) or on the state it waits for, never a clock. A check's
// outcome is said where it is decided (`level-visit: ARM <name> <role> PASS|FAIL|INVALID`, a failure also
// `rig: VERDICT level-visit`), no later leg runs, and a bound that runs out says INVALID only. The host says the PASS.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/player/level_visit.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_events.h"
#include "coop/player/remote_player.h"
#include "coop/session/rig_verdict.h"
#include "harness/screenshot.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_fingerprint.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/screen_state.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>

namespace harness::autotest {
namespace {

namespace PS   = harness::autotest::peer_signal;
namespace R    = ue_wrap::reflection;
namespace E    = ue_wrap::engine;
namespace OI   = ue_wrap::object_index;
namespace LV   = coop::level_visit;
namespace RV   = coop::rig_verdict;
using disc_drill::OnGameThread;

constexpr const char* kTag = "level-visit";
constexpr float kFloorZ = LV::kVisitFloorZ;
constexpr uint32_t kSettlePoses = 3;  // poses applied after the first of a state, by which its copy must follow
constexpr int kStepSec = 240;         // the bound on a step of the other peer's
constexpr int kLoadSec = 120;         // the bound on a trip reaching its map
constexpr int kSettleSec = 30;        // the bound on a state following an event
constexpr int kBackSec = 60;          // the bound on the return after a map's end the drill asks for

const char* Who() { return IsClientRole() ? "client" : "host"; }
void* Me() { return coop::players::Registry::Get().Local(); }

// ---- outcomes, said where they are decided ---------------------------------------------------------------------------
enum class Res { Fail = 0, Pass = 1, Invalid = 2 };
std::string Result(Res r, const std::string& why) { return std::to_string(static_cast<int>(r)) + " " + why; }
Res ResultOf(const std::string& t) {
    return t.empty() ? Res::Invalid : t[0] == '1' ? Res::Pass : t[0] == '0' ? Res::Fail : Res::Invalid;
}
std::string ResultWhy(const std::string& t) { return t.size() > 2 ? t.substr(2) : std::string("no reason given"); }

// This peer's check, left for the other peer under `name`. A failure or an unmeasurable check is said here at once and
// ends the drill (false); a pass goes on.
bool Emit(const char* name, Res r, const std::string& why) {
    PS::Mark(name, Result(r, why));
    if (r == Res::Pass) {
        UE_LOGI("level-visit: ARM %hs %hs PASS %hs", name, Who(), why.c_str());
        return true;
    }
    UE_LOGW("level-visit: ARM %hs %hs %hs %hs", name, Who(), r == Res::Fail ? "FAIL" : "INVALID", why.c_str());
    RV::Say(kTag, r == Res::Fail ? RV::Outcome::Fail : RV::Outcome::Invalid, "%s: %s", name, why.c_str());
    return false;
}

// The other peer's check `name`, waited for. Its failure was said by that peer; it is said here too (once per process).
bool Heard(const char* name, int seconds = kStepSec) {
    std::string t;
    if (!PS::AwaitText(name, t, seconds)) return false;
    const Res r = ResultOf(t);
    if (r == Res::Pass) return true;
    RV::Say(kTag, r == Res::Fail ? RV::Outcome::Fail : RV::Outcome::Invalid, "%s (the other peer): %s", name,
            ResultWhy(t).c_str());
    return false;
}

// Whether the other peer has left a check that did not pass: a wait on a pose or a state ends at it.
bool OtherStopped() {
    static const char* const kNames[] = {"arrived", "back", "hid", "shown", "held", "reopened_clip", "joined_hid"};
    std::string t;
    for (const char* n : kNames)
        if (PS::Peek(n, t) && ResultOf(t) != Res::Pass) return true;
    return false;
}

// ---- the poses this peer applies (the drill's own listener; the pump's records are not readable from here) ----------
struct PoseRec {
    std::atomic<uint32_t> seq{0};       // poses applied for the slot
    std::atomic<uint32_t> stateSeq{0};  // the pose that began the current state (below the floor, or not)
    std::atomic<uint32_t> firstSeq{0};  // the slot's first pose heard
    std::atomic<bool> have{false}, below{false}, sawBelow{false}, firstBelow{false};
};
PoseRec g_rec[coop::players::kMaxPeers];

void OnPose(uint8_t slot, const coop::net::PoseSnapshot& pose) {
    if (slot >= coop::players::kMaxPeers) return;
    PoseRec& r = g_rec[slot];
    const bool below = pose.z < kFloorZ;
    const uint32_t seq = r.seq.load() + 1;
    if (!r.have.load()) {
        r.firstBelow = below;
        r.firstSeq = seq;
        r.stateSeq = seq;
        r.have = true;
    } else if (below != r.below.load()) {
        r.stateSeq = seq;
    }
    r.below = below;
    if (below) r.sawBelow = true;
    r.seq = seq;
}

// AActor::bHidden of the slot's copy here: 1, 0, or -1 with no copy or an unread flag.
int CopyHidden(int slot) {
    int out = -1;
    OnGameThread([&out, slot] {
        coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
        void* actor = p && p->valid() ? p->actor() : nullptr;
        int32_t off = -1;
        uint8_t mask = 0;
        if (actor && R::FindBoolProperty(R::ClassOf(actor), L"bHidden", off, mask))
            out = (static_cast<const uint8_t*>(actor)[off] & mask) ? 1 : 0;
    });
    return out;
}

// The copy of `slot` follows its pose: hidden == `want` once kSettlePoses more poses have been applied after the pose
// `from` that began the state, or sooner. Decided at that event; a stream that stops first is Invalid.
bool Follows(const char* name, int slot, int want, uint32_t from, const char* hiddenWhat, const char* shownWhat) {
    PoseRec& r = g_rec[slot];
    const bool settled = PS::WaitFor("the copy following its pose", kSettleSec, [&] {
        return CopyHidden(slot) == want || r.seq.load() >= from + kSettlePoses;
    }, false);
    if (!settled) return Emit(name, Res::Invalid, "the pose stream stopped before the copy could be judged");
    const int hidden = CopyHidden(slot);
    if (hidden < 0) return Emit(name, Res::Invalid, "no copy of that player stands here");
    if (hidden != want)
        return Emit(name, Res::Fail, want ? hiddenWhat : shownWhat);
    return Emit(name, Res::Pass, want ? "its copy is hidden here" : "its copy is shown again");
}

// The watcher's check: the visitor's pose `below` the visit floor (or back above it, once it was below) is the event; its
// copy must follow it.
bool JudgeCopy(const char* name, int slot, bool below, int boundSec) {
    PoseRec& r = g_rec[slot];
    const bool entered = PS::WaitFor(below ? "the visitor's pose below the floor" : "the visitor's pose back above it",
                                     boundSec, [&] {
        return OtherStopped() || (below ? r.below.load() : (r.sawBelow.load() && !r.below.load()));
    }, false);
    if (OtherStopped()) return false;
    if (!entered) return Emit(name, Res::Invalid, "no such pose reached this peer");
    return Follows(name, slot, below ? 1 : 0, r.stateSeq.load(), "the visitor is in the map and its copy is not hidden here",
                   "the visitor is back and its copy stays hidden here");
}

// ---- the visitor's state ----------------------------------------------------------------------------------------------
struct Now {
    ue_wrap::FVector at{};
    float health = 0.f;
    void* held = nullptr;
    std::wstring heldName = L"none";  // read on the game thread: the pointer may be gone by the time it is logged
    int inventory = -1, hold = -1;
    bool visiting = false, inMap = false;
    uint32_t trips = 0;      // visits begun, counted on the game thread beside `visiting` (one moment, one answer)
    bool possessed = false;  // the controller holds this player
    float camZ = 0.f;
    int black = 0;           // black screens on the viewport
};

// A TArray's length on the game mode's save slot.
int ArrayNum(const wchar_t* field) {
    void* gm = ue_wrap::world_singleton::Gamemode();
    const int32_t offSlot = gm ? R::FindPropertyOffset(R::ClassOf(gm), L"saveSlot") : -1;
    void* slot = offSlot >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(gm) + offSlot) : nullptr;
    const int32_t off = slot ? R::FindPropertyOffset(R::ClassOf(slot), field) : -1;
    return off >= 0 ? *reinterpret_cast<int32_t*>(static_cast<char*>(slot) + off + 8) : -1;
}

Now Read() {
    Now n;
    OnGameThread([&n] {
        if (Me()) E::TryGetActorLocation(Me(), n.at);
        ue_wrap::vitals::Read(ue_wrap::vitals::Field::Health, &n.health);
        const int32_t off = Me() ? R::FindPropertyOffset(R::ClassOf(Me()), L"holding_actor") : -1;
        n.held = off >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(Me()) + off) : nullptr;
        if (n.held && R::IsLive(n.held)) n.heldName = R::ClassNameOf(n.held);
        else n.held = nullptr;
        n.inventory = ArrayNum(L"inventoryData");
        n.hold = ArrayNum(L"hold");
        n.visiting = LV::Visiting();
        n.trips = LV::Stats().tripsStarted;
        n.inMap = LV::CurrentLevel() != nullptr;
        n.possessed = Me() && E::GetController(Me()) != nullptr;
        n.camZ = E::GetCameraLocation().Z;
        n.black = ue_wrap::engine::ReadScreenState().blackOnScreen;
    });
    return n;
}

void Log(const char* what, const Now& n) {
    UE_LOGI("level-visit: STATE %hs %hs at=(%.0f,%.0f,%.0f) health=%.0f visiting=%d map=%ls held=%ls inventory=%d hold=%d "
            "possessed=%d camZ=%.0f black=%d", what, Who(), n.at.X, n.at.Y, n.at.Z, n.health, n.visiting ? 1 : 0,
            LV::CurrentMap().c_str(), n.heldName.c_str(), n.inventory, n.hold, n.possessed ? 1 : 0, n.camZ, n.black);
}

void PlaceMe(const ue_wrap::FVector& at) {
    void* f = R::FindFunction(R::ClassOf(Me()), L"teleportWObackrooms");
    ue_wrap::ParamFrame pf(f);
    ue_wrap::FTransform t{};
    t.TX = at.X; t.TY = at.Y; t.TZ = at.Z;
    if (f && pf.valid() && pf.Set<ue_wrap::FTransform>(L"newTransform", t)) ue_wrap::Call(Me(), pf);
}

// The level script actor of the visited map: what its own End and QuitGame calls name as their context.
void* LevelScriptActor() {
    void* level = LV::CurrentLevel();
    if (!level) return nullptr;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(level), L"LevelScriptActor");
    return off >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(level) + off) : nullptr;
}

void DumpLevelClasses(const std::string& dir) {
    OnGameThread([&dir] {
        void* level = LV::CurrentLevel();
        if (!level) return;
        struct Ctx { void* level; std::map<std::wstring, void*> cls; } c{level, {}};
        OI::ForEachClass([](void* ctx, void* cls, void*) {
            OI::ForEachInstance(cls, [](void* ctx2, void* o, int32_t) {
                auto* k = static_cast<Ctx*>(ctx2);
                if (R::IsLive(o) && R::OuterOf(o) == k->level) k->cls[R::ClassNameOf(o)] = R::ClassOf(o);
            }, ctx);
        }, &c);
        std::wstring names;
        for (auto& [n, cls] : c.cls) {
            names += L" " + n;
            if (n.size() < 3 || n.substr(n.size() - 2) != L"_C") continue;
            const std::string file = dir + "/" + std::string(n.begin(), n.end()) + "_script.txt";
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            for (const R::ObjectRef& o : R::ChildObjectsOf(cls)) {
                if (o.className != L"Function") continue;
                out << "=== " << std::string(o.name.begin(), o.name.end()) << '\n'
                    << ue_wrap::script_fingerprint::Of(o.object).text << '\n';
            }
        }
        UE_LOGI("level-visit: the map's classes:%ls", names.c_str());
    });
}

// ---- what the visit's rows ask for ---------------------------------------------------------------------------------
struct Plan {
    std::string start, look, map, end, dump;
    bool camera = false;  // the visitor takes the camera the host spawns into its hand
};

Plan PlanFromRows() {
    Plan p;
    p.start = coop::config::ResolveEnum(coop::config_registry::rows::level_visit_start);
    p.look = coop::config::ResolveEnum(coop::config_registry::rows::level_visit_look);
    p.map = coop::config::ResolveString(coop::config_registry::rows::level_visit_map);
    if (p.map.empty()) p.map = "sl_backroomsObsolete";
    p.end = coop::config::ResolveEnum(coop::config_registry::rows::level_visit_end);
    p.dump = coop::config::ResolveString(coop::config_registry::rows::level_visit_dump_dir);
    p.camera = p.start != "clip" && p.look != "lamp";
    return p;
}

void* FindCamera() {
    void* cls = OI::ClassByName(L"prop_vidcam_low_C");
    void* cam = nullptr;
    if (cls)
        OI::ForEachInstance(cls, [](void* ctx, void* o, int32_t) {
            if (!*static_cast<void**>(ctx) && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__"))
                *static_cast<void**>(ctx) = o;
        }, &cam);
    return cam;
}

// HOST: a camera beside it, for the visitor to take (a run that takes none carries what its profile holds).
void SpawnCamera() {
    OnGameThread([] {
        void* cls = OI::ClassByName(L"prop_vidcam_low_C");
        if (!cls) cls = R::FindClass(L"prop_vidcam_low_C");
        ue_wrap::FVector at{};
        E::TryGetActorLocation(Me(), at);
        void* cam = cls ? E::SpawnActor(cls, {at.X + 150.f, at.Y, at.Z}) : nullptr;
        UE_LOGI("level-visit: host spawned a camera for the visitor (%d)", cam ? 1 : 0);
    });
}

// VISITOR: the camera, placed beside and taken in hand.
bool TakeCamera() {
    if (!PS::WaitFor("the camera the host spawned", 60, [] {
            void* found = nullptr;
            OnGameThread([&found] { found = FindCamera(); });
            return found != nullptr;
        }))
        return false;
    OnGameThread([] {
        void* cam = FindCamera();
        ue_wrap::FVector at{};
        if (cam && E::TryGetActorLocation(cam, at)) PlaceMe({at.X - 120.f, at.Y, at.Z + 60.f});
        bool collected = false;
        const bool took = cam && E::CallMainPlayerHoldObject(Me(), cam, collected);
        UE_LOGI("level-visit: %hs took the camera ran=%d", Who(), took ? 1 : 0);
    });
    return PS::WaitFor("the camera in hand", 15, [] { return Read().held != nullptr; });
}

// The game's own clip: its tick traces from lastLoc to the camera, and a hit on static geometry is the trip. lastLoc
// 4 m under the floor puts the floor between them.
void WriteLastLoc(bool say) {
    OnGameThread([say] {
        const int32_t off = R::FindPropertyOffset(R::ClassOf(Me()), L"lastLoc");
        const ue_wrap::FVector cam = E::GetCameraLocation();
        if (off >= 0)
            *reinterpret_cast<ue_wrap::FVector*>(static_cast<char*>(Me()) + off) = {cam.X, cam.Y, cam.Z - 400.f};
        if (say) UE_LOGI("level-visit: %hs lastLoc set under the floor for the game's own clip (%d)", Who(), off >= 0 ? 1 : 0);
    });
}

// The clip driven until its event: the game's tick overwrites lastLoc with the camera on every frame its condition
// fails (the check, the trace, the view target), so one write survives only when the very next tick passes it. lastLoc is
// written again at each poll, on the game thread, until `done` or the bound; false when the bound ran out. One chain at a
// time: the chain's black screen is its first visible event and its trip comes a second later, so once a black screen
// beyond the one already up is seen no more is written (a write would start a second chain).
template <class Pred>
bool ClipUntil(const char* what, int boundSec, Pred done) {
    bool first = true, underway = false;
    const int blackBefore = Read().black;
    return PS::WaitFor(what, boundSec, [&] {
        if (done()) return true;
        underway = underway || Read().black > blackBefore;
        if (underway) return false;
        WriteLastLoc(first);
        first = false;
        return false;
    }, false);
}

// What the game's clip check answered on this machine, for an INVALID that names the term the clip never passed.
std::string ClipDiag() {
    const LV::DrillStats s = LV::Stats();
    const char* by = s.lastClipWhy == 1 ? "the mod, the host closed the backrooms"
                   : s.lastClipWhy == 2 ? "the mod, a visit runs"
                   : s.lastClipWhy == 3 ? "the mod, the quiet window after a visit"
                   : s.lastClipWhy == 0 ? "the game's own logic (its answer is not read)" : "nobody";
    return "the clip check was asked " + std::to_string(s.clipChecksAsked) + " times for this player, last answered by " + by;
}

// The clip's own travel call, lib_C::loadLevel with the map.
void TryCall(const std::string& map) {
    OnGameThread([&map] {
        void* lib = R::FindClassDefaultObject(L"lib_C");
        void* fn = lib ? R::FindFunction(R::ClassOf(lib), L"loadLevel") : nullptr;
        ue_wrap::ParamFrame pf(fn);
        const std::wstring w(map.begin(), map.end());
        const bool ran = fn && pf.valid() && pf.Set<R::FName>(L"level", ue_wrap::fname_utils::StringToFName(w)) &&
                         pf.Set<bool>(L"clearSubArea", true) && pf.Set<void*>(L"__WorldContext", Me()) &&
                         ue_wrap::Call(lib, pf);
        UE_LOGI("level-visit: %hs called lib_C::loadLevel('%hs') ran=%d", Who(), map.c_str(), ran ? 1 : 0);
    });
}

// ---- the map's lantern (level_visit_look = lamp) --------------------------------------------------------------------
void* FindLamp() {
    void* cls = OI::ClassByName(L"prop_lamp_C");
    void* lamp = nullptr;
    if (cls)
        OI::ForEachInstance(cls, [](void* ctx, void* o, int32_t) {
            ue_wrap::FVector at{};
            if (!*static_cast<void**>(ctx) && R::IsLive(o) && E::TryGetActorLocation(o, at) && at.Z < kFloorZ)
                *static_cast<void**>(ctx) = o;
        }, &lamp);
    return lamp;
}

bool LampLit() {
    bool lit = false;
    OnGameThread([&lit] {
        void* lamp = FindLamp();
        int32_t off = -1;
        uint8_t mask = 0;
        lit = lamp && R::FindBoolProperty(R::ClassOf(lamp), L"active", off, mask) && off >= 0 &&
              (*(static_cast<uint8_t*>(lamp) + off) & mask) != 0;
    });
    return lit;
}

// A step: 0 face it, 1 take it in hand, 2 light it with its own RMB use, 3 put it down with the player's own forceDrop.
void LampStep(int step) {
    OnGameThread([step] {
        void* lamp = FindLamp();
        ue_wrap::FVector at{}, me{};
        if (!lamp || !E::TryGetActorLocation(lamp, at) || !E::TryGetActorLocation(Me(), me)) {
            UE_LOGI("level-visit: %hs LAMP none in the map", Who());
            return;
        }
        const ue_wrap::FVector cam = E::GetCameraLocation();
        const float dx = at.X - cam.X, dy = at.Y - cam.Y, dz = at.Z - cam.Z;
        bool ok = false, collected = false;
        if (step == 1) ok = E::CallMainPlayerHoldObject(Me(), lamp, collected);
        else if (step == 2 || step == 3) {
            void* who = step == 2 ? lamp : Me();
            void* fn = step == 2 ? R::FindFunction(R::ClassOf(lamp), L"playerHandUse_RMB")
                                 : R::FindDispatchFunctionCached(R::ClassOf(Me()), L"forceDrop");
            ue_wrap::ParamFrame pf(fn);
            ok = fn && pf.valid() && (step == 3 || pf.Set<void*>(L"player", Me())) && ue_wrap::Call(who, pf);
        } else if (void* pc = E::GetController(Me())) {
            E::SetControlRotation(pc, {static_cast<float>(std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 57.2958),
                                       static_cast<float>(std::atan2(dy, dx) * 57.2958), 0.f});
            ok = true;
        }
        UE_LOGI("level-visit: %hs LAMP at (%.0f,%.0f,%.0f) step %d %hs", Who(), at.X, at.Y, at.Z, step,
                ok ? "ran" : "failed");
    });
}

// Face the lantern, take it in hand, light it, put it down: each step on the state the one before left.
void LampSequence() {
    LampStep(0);
    harness::screenshot::Capture(L"visit-lamp-faced");
    LampStep(1);
    PS::WaitFor("the lantern in hand", 15, [] { return Read().held != nullptr; }, false);
    LampStep(2);
    PS::WaitFor("the lantern lit", 15, [] { return LampLit(); }, false);
    harness::screenshot::Capture(L"visit-lamp-lit");
    LampStep(3);
    PS::WaitFor("the lantern put down", 15, [] { return Read().held == nullptr; }, false);
}

// ---- the visitor ---------------------------------------------------------------------------------------------------
// The control (level_visit_start = none): no visit, a lethal hit in the world, and what the player holds after the usual
// revive.
bool Control(const Now& before) {
    OnGameThread([] { E::InvokeAddPlayerDamage(Me(), 1000.f, /*blood=*/true); });
    bool died = false;
    const bool revived = PS::WaitFor("the death and its revive", 90, [&died] {
        const Now n = Read();
        died = died || n.health <= 0.f;
        return died && n.health > 0.f;
    }, false);
    const Now n = Read();
    Log("control", n);
    UE_LOGI("level-visit: %hs a death in the world: held before=%ls after=%ls hold %d->%d", Who(), before.heldName.c_str(),
            n.heldName.c_str(), before.hold, n.hold);
    return Emit("back", revived ? Res::Pass : Res::Invalid, revived ? "a death in the world, revived (the control)"
                                                                     : "the death's revive did not come");
}

// The visit's end the drill asks for (`wait` leaves it to the map's own clock).
void EndVisit(const std::string& end) {
    OnGameThread([&end] {
        void* script = LevelScriptActor();
        bool ran = false;
        if (end == "wait") {
            ran = true;
        } else if (end == "death") {
            ran = E::InvokeAddPlayerDamage(Me(), 1000.f, /*blood=*/true);  // the game's own lethal hit
        } else if (end == "quit") {
            void* cdo = R::FindClassDefaultObject(L"KismetSystemLibrary");
            void* fn = cdo ? R::FindFunction(R::ClassOf(cdo), L"QuitGame") : nullptr;
            ue_wrap::ParamFrame pf(fn);
            ran = fn && pf.valid() && pf.Set<void*>(L"WorldContextObject", script) && ue_wrap::Call(cdo, pf);
        } else {
            void* lib = R::FindClassDefaultObject(L"lib_C");
            void* fn = lib ? R::FindFunction(R::ClassOf(lib), L"End") : nullptr;
            ue_wrap::ParamFrame pf(fn);
            ran = fn && pf.valid() && pf.Set<void*>(L"__WorldContext", script) && ue_wrap::Call(lib, pf);
        }
        UE_LOGI("level-visit: %hs ended the visit by '%hs' (level script %ls) ran=%d", Who(), end.c_str(),
                script ? R::ClassNameOf(script).c_str() : L"none", ran ? 1 : 0);
    });
}

// The trip, and the player standing in the loaded map: its own to move (its controller holds it), its view in the map,
// uncovered. The visit ending before the player arrives is the event that fails it; no trip at all is Invalid.
bool Arrive(const Plan& p) {
    const uint32_t trips = LV::Stats().tripsStarted;
    if (p.start == "clip") {
        const bool began = ClipUntil("the game's own clip taking a trip", kLoadSec,
                                     [trips] { return LV::Stats().tripsStarted > trips || OtherStopped(); });
        if (OtherStopped()) return false;
        if (!began)
            return Emit("arrived", Res::Invalid, "no clip took a trip in " + std::to_string(kLoadSec) + " s: " + ClipDiag());
    } else {
        TryCall(p.map);
    }
    const bool reached = PS::WaitFor("the visitor in the map", kLoadSec, [&] {
        const Now n = Read();
        return n.inMap || (n.trips > trips && !n.visiting) || OtherStopped();
    }, false);
    if (OtherStopped()) return false;
    const Now n = Read();
    Log("arrived", n);
    if (!n.inMap) {
        // The bound running out says INVALID, also while the map is still loading: the visit has not ended.
        if (!reached)
            return Emit("arrived", Res::Invalid, n.visiting ? "the map was still loading when the bound ran out"
                                                              : "no trip reached the map");
        if (n.trips > trips) return Emit("arrived", Res::Fail, "the visit ended before the player arrived in the map");
        return Emit("arrived", Res::Invalid, "no trip started");
    }
    if (!PS::WaitFor("the view in the map", kSettleSec, [] { return Read().camZ < kFloorZ; }, false))
        return Emit("arrived", Res::Invalid, "the view did not follow the player into the map");
    const Now in = Read();
    const bool inOk = in.possessed && in.camZ < kFloorZ && in.black == 0;
    UE_LOGI("level-visit: %hs in the map possessed=%d camZ=%.0f black=%d", Who(), in.possessed ? 1 : 0, in.camZ, in.black);
    return Emit("arrived", inOk ? Res::Pass : Res::Fail,
                inOk ? "in the map the player is its own to move, its view uncovered"
                     : "in the map the player was not its own to move, or its view was covered");
}

// Back in the world with what it carried: out of the map, alive, the dropped item given back, its view the player's.
bool Return(const Plan& p, const Now& before) {
    const int bound = p.end == "wait" ? 720 : kBackSec;
    const bool back = PS::WaitFor("the visitor back in the world", bound, [] {
        const Now n = Read();
        return !n.visiting && n.at.Z > kFloorZ && n.health > 0.f;
    }, false);
    if (!back) return Emit("back", Res::Invalid, "the visitor did not come back in the time asked");
    PS::WaitFor("the dropped item given back", kSettleSec, [] {
        bool pending = true;
        OnGameThread([&pending] { pending = LV::ReturnPending(); });
        return !pending;
    }, false);
    const bool viewed = PS::WaitFor("the view the player's", kSettleSec, [] {
        const Now n = Read();
        return n.possessed && std::fabs(n.camZ - n.at.Z) < 300.f;
    }, false);
    const Now n = Read();
    Log("back", n);
    UE_LOGI("level-visit: %hs back (last end '%ls') held before=%ls after=%ls hold %d->%d inventory %d->%d", Who(),
            LV::LastEnd().c_str(), before.heldName.c_str(), n.heldName.c_str(), before.hold, n.hold, before.inventory,
            n.inventory);
    const bool kept = n.held == nullptr ? before.held == nullptr : (before.held && n.hold == before.hold);
    const bool pockets = n.inventory == before.inventory;
    if (!kept) return Emit("back", Res::Fail, "the hand item did not come back");
    if (!pockets) return Emit("back", Res::Fail, "the pockets changed");
    if (!viewed) return Emit("back", Res::Invalid, "back in the world the view did not settle on the player");
    return Emit("back", Res::Pass, "back in the world with what it carried, its view its own throughout");
}

// The visitor's half of a pair: the trip, the stay until the watcher has judged its copy hidden, the end, the return.
bool Visit(const Plan& p) {
    if (p.camera && !TakeCamera()) return false;
    const Now before = Read();
    Log("before", before);
    if (p.start == "none") return Control(before);
    if (!Arrive(p)) return false;
    if (!p.dump.empty()) DumpLevelClasses(p.dump);
    harness::screenshot::Capture(L"visit-in");
    if (p.look == "lamp") LampSequence();
    if (!Heard("hid")) return false;
    EndVisit(p.end);
    return Return(p, before);
}

// ---- the watcher ---------------------------------------------------------------------------------------------------
// The watcher's half: its copy of the visitor hidden at the pose below the floor, shown at the pose back above it.
bool Watch(int slot, const Plan& p) {
    if (!JudgeCopy("hid", slot, true, kStepSec)) return false;
    return JudgeCopy("shown", slot, false, p.end == "wait" ? 780 : kStepSec);
}

// ---- the pair, the close and the join ---------------------------------------------------------------------------------
// Both peers are in the world: the client says so, the host waits for it (and spawns the camera for a run that takes one).
bool Ready(const Plan& p) {
    if (IsClientRole()) {
        if (!disc_drill::ClientJoined()) return false;
        PS::Mark("client_ready");
        return !p.camera || PS::Await("spawned");
    }
    if (!disc_drill::HostWorldUp() || !PS::Await("client_ready", 480)) return false;
    if (p.camera) {
        SpawnCamera();
        PS::Mark("spawned");
    }
    return true;
}

void RunPair(const std::string& arm) {
    const Plan p = PlanFromRows();
    if (!Ready(p)) return;
    const bool hostVisits = arm == "host";
    const bool visitor = IsClientRole() != hostVisits;
    const int visitorSlot = hostVisits ? 0 : 1;
    if (visitor) {
        if (!Visit(p)) return;
    } else if (p.start != "none") {
        if (!Watch(visitorSlot, p)) return;
    }
    if (IsClientRole()) {
        UE_LOGI("level-visit: DONE client");
        return;
    }
    // The host judges: what the client alone could see reaches it as its signals.
    const bool ok = visitor ? (p.start == "none" || Heard("shown"))
                            : ((p.start == "none" || Heard("arrived")) && Heard("back"));
    if (!ok) return;
    UE_LOGI("level-visit: DONE host");
    RV::Say(kTag, RV::Outcome::Pass,
            p.start == "none" ? "a death in the world, revived (the control)"
                              : "the visitor's copy hidden by its pose while it was away and shown after, the visitor back "
                                "with what it carried, its view its own throughout");
}

// A clip while the host has the backrooms closed is answered no, and after the host opens them it takes a trip. The
// host closes and opens; the client clips.
bool BackroomsAllowed() {
    bool on = true;
    OnGameThread([&on] { on = coop::config::ResolveFlag(coop::config_registry::rows::world_backrooms); });
    return on;
}

void RunCloseHost() {
    if (!disc_drill::HostWorldUp() || !PS::Await("client_ready", 480)) return;
    OnGameThread([] {
        const auto r = coop::config::SetValue(coop::config_registry::rows::world_backrooms, "0");
        UE_LOGI("level-visit: host closed the backrooms (set result %d)", static_cast<int>(r));
    });
    PS::Mark("closed");
    if (!Heard("held")) return;
    OnGameThread([] {
        const auto r = coop::config::ResetValue(coop::config_registry::rows::world_backrooms);
        UE_LOGI("level-visit: host opened the backrooms again (reset result %d)", static_cast<int>(r));
    });
    PS::Mark("reopened");
    if (!Heard("reopened_clip")) return;
    UE_LOGI("level-visit: DONE host");
    RV::Say(kTag, RV::Outcome::Pass, "a clip was answered no while the host had the backrooms closed, and took a trip after "
                                     "the host opened them again");
}

void RunCloseClient() {
    if (!disc_drill::ClientJoined()) return;
    PS::Mark("client_ready");
    if (!PS::Await("closed")) return;
    if (!PS::WaitFor("the host's closed setting", kSettleSec, [] { return !BackroomsAllowed(); }, false))
        { Emit("held", Res::Invalid, "the host's closed setting never reached this client"); return; }
    // The clip's event: the game asks its check in the tick that traces, and the check is answered no (a count) or the
    // trip starts. Whichever comes first decides; neither is Invalid. The green is the first answer of no (the game asks
    // every tick, so it is decided at a clip's answer and not at a trace hit); the red's FAIL is the trip.
    const LV::DrillStats before = LV::Stats();
    const bool decided = ClipUntil("the clip check answered or a trip taken", kSettleSec, [&before] {
        const LV::DrillStats s = LV::Stats();
        return s.clipsHeldClosed > before.clipsHeldClosed || s.tripsStarted > before.tripsStarted;
    });
    if (LV::Stats().tripsStarted > before.tripsStarted)
        { Emit("held", Res::Fail, "a clip took a trip while the host had the backrooms closed"); return; }
    if (!decided) { Emit("held", Res::Invalid, "no clip check was answered no in the time asked: " + ClipDiag()); return; }
    // A refused clip leaves the player as before: no black screen of the clip's chain stands. A chain past its check when
    // the setting arrived puts one up and the lane takes it down on its next turn, so the read waits for it to be gone.
    if (!PS::WaitFor("the clip's black screen gone", kSettleSec, [] { return Read().black == 0; }, false))
        { Emit("held", Res::Fail, "a black screen still stands after the refused clip"); return; }
    if (!Emit("held", Res::Pass, "the clip check was answered no while the host had the backrooms closed, and no black "
                                 "screen stands")) return;
    if (!PS::Await("reopened")) return;
    if (!PS::WaitFor("the host's open setting", kSettleSec, [] { return BackroomsAllowed(); }, false))
        { Emit("reopened_clip", Res::Invalid, "the host's open setting never reached this client"); return; }
    const uint32_t trips = LV::Stats().tripsStarted;
    const bool went = ClipUntil("a clip taking a trip", kLoadSec, [trips] { return LV::Stats().tripsStarted > trips; });
    if (!Emit("reopened_clip", went ? Res::Pass : Res::Invalid,
              went ? "after the host opened the backrooms again a clip took a trip"
                   : "no trip started after the host opened the backrooms again: " + ClipDiag()))
        return;
    UE_LOGI("level-visit: DONE client");
}

// The host takes the trip and stays; a client that joins later must find the host's copy hidden at the first pose it
// applies.
void RunJoinHost() {
    const Plan p = PlanFromRows();
    if (!disc_drill::HostWorldUp()) return;
    if (p.camera && !TakeCamera()) return;
    Log("before", Read());
    if (!Arrive(p)) return;
    if (!Heard("joined_hid", 480)) return;
    UE_LOGI("level-visit: DONE host");
    RV::Say(kTag, RV::Outcome::Pass, "a client that joined while the host was in the map found the host's copy hidden at "
                                     "the first pose it applied");
}

void RunJoinClient() {
    if (!disc_drill::ClientJoined()) return;
    PoseRec& r = g_rec[0];
    if (!PS::WaitFor("the host's first pose", kSettleSec, [&r] { return r.have.load(); }, false)) {
        Emit("joined_hid", Res::Invalid, "no pose of the host reached this joiner");
        return;
    }
    if (!r.firstBelow.load()) {
        Emit("joined_hid", Res::Invalid, "the host was not in the map when its first pose reached this joiner");
        return;
    }
    if (Follows("joined_hid", 0, 1, r.firstSeq.load(), "the host is in the map and its copy is not hidden here at the first "
                "pose this joiner applied", ""))
        UE_LOGI("level-visit: DONE client");
}

}  // namespace

DWORD WINAPI LevelVisitThread(LPVOID /*arg*/) {
    const std::string arm = coop::config::ResolveEnum(coop::config_registry::rows::level_visit_drill);
    if (!PS::Begin(kTag)) return 0;
    if (arm != "close")
        OnGameThread([] {
            if (!coop::puppet_events::AddPoseListener(&OnPose)) UE_LOGW("level-visit: the pose listener list is full");
        });
    if (arm == "close") {
        if (IsClientRole()) RunCloseClient();
        else RunCloseHost();
    } else if (arm == "join") {
        if (IsClientRole()) RunJoinClient();
        else RunJoinHost();
    } else {
        RunPair(arm);
    }
    return 0;
}

}  // namespace harness::autotest
