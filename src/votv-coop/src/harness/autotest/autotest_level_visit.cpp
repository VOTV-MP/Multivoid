// harness/autotest/autotest_level_visit.cpp -- TEST-ONLY: a clip's trip to another map is the clipping player's alone,
// and comes back as a death does (coop/player/level_visit).

// The visitor (VOTVCOOP_LEVEL_VISIT_DOER, client by default) takes a camera the host spawns into its hand, then calls
// the game's own lib_C::loadLevel for VOTVCOOP_LEVEL_VISIT_MAP (sl_backroomsObsolete by default) as the clip does.
// With VOTVCOOP_LEVEL_VISIT_START=clip the trip is the game's own clip instead (lastLoc under the floor, no camera
// taken), and the map is the clip's pick.

// It waits to be in the map, stays 20 s, and ends the visit by VOTVCOOP_LEVEL_VISIT_END: `end` calls lib_C::End from
// the map's own level script, `quit` calls QuitGame with it, `wait` leaves it to the map's own clock (the backrooms'
// 600 s), `death` deals a lethal hit through the game's own Add Player Damage. It then waits to be back in the world.
// The other peer reads the visitor's copy each second.

// Each peer logs `level-visit: STATE` once a second; verdicts are `level-visit: VERDICT <role> PASS|FAIL <why>`: the
// visitor back in the world, out of the map, with the same hand item and pocket count; the other peer saw the copy
// hidden while it was away and shown again after. With VOTVCOOP_LEVEL_VISIT_DUMP_DIR the visitor writes the map's own
// Blueprint classes there as script text. Gated by VOTVCOOP_RUN_LEVEL_VISIT; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/player/level_visit.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
#include "harness/screenshot.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
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
#include <fstream>
#include <map>
#include <string>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace LV = coop::level_visit;

std::string g_dir;
bool g_hostVisits = false;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

const char* Who() { return IsClientRole() ? "client" : "host"; }
bool IVisit() { return IsClientRole() != g_hostVisits; }
void* Me() { return coop::players::Registry::Get().Local(); }

void Write(const std::string& name) { std::ofstream(g_dir + "\\" + name, std::ios::binary | std::ios::trunc) << "x"; }
bool Seen(const std::string& name) { return static_cast<bool>(std::ifstream(g_dir + "\\" + name)); }
bool Await(const std::string& name, int seconds = 300) {
    for (int i = 0; i < seconds * 10; ++i) {
        if (Seen(name)) return true;
        ::Sleep(100);
    }
    UE_LOGW("level-visit: ABORT -- no '%hs' from the other peer", name.c_str());
    return false;
}

void Verdict(bool ok, const char* why) {
    UE_LOGI("level-visit: VERDICT %hs %hs %hs", Who(), ok ? "PASS" : "FAIL", why);
}

void PlaceMe(const ue_wrap::FVector& at) {
    void* f = R::FindFunction(R::ClassOf(Me()), L"teleportWObackrooms");
    ue_wrap::ParamFrame pf(f);
    ue_wrap::FTransform t{};
    t.TX = at.X; t.TY = at.Y; t.TZ = at.Z;
    if (f && pf.valid() && pf.Set<ue_wrap::FTransform>(L"newTransform", t)) ue_wrap::Call(Me(), pf);
}

// A TArray's length on the game mode's save slot.
int ArrayNum(const wchar_t* field) {
    void* gm = ue_wrap::world_singleton::Gamemode();
    const int32_t offSlot = gm ? R::FindPropertyOffset(R::ClassOf(gm), L"saveSlot") : -1;
    void* slot = offSlot >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(gm) + offSlot) : nullptr;
    const int32_t off = slot ? R::FindPropertyOffset(R::ClassOf(slot), field) : -1;
    return off >= 0 ? *reinterpret_cast<int32_t*>(static_cast<char*>(slot) + off + 8) : -1;
}

struct Now {
    ue_wrap::FVector at{};
    float health = 0.f;
    void* held = nullptr;
    std::wstring heldName = L"none";  // read on the game thread: the pointer may be gone by the time it is logged
    int inventory = -1, hold = -1;
    bool visiting = false;
    bool possessed = false;  // the controller holds this player
    float camZ = 0.f;
    int black = 0;           // black screens on the viewport
};

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
        n.possessed = Me() && E::GetController(Me()) != nullptr;
        n.camZ = E::GetCameraLocation().Z;
        n.black = ue_wrap::engine::ReadScreenState().blackOnScreen;
    });
    return n;
}

void Log(const Now& n) {
    UE_LOGI("level-visit: STATE %hs at=(%.0f,%.0f,%.0f) health=%.0f visiting=%d map=%ls held=%ls inventory=%d hold=%d "
            "possessed=%d camZ=%.0f black=%d", Who(), n.at.X, n.at.Y, n.at.Z, n.health, n.visiting ? 1 : 0,
            LV::CurrentMap().c_str(), n.heldName.c_str(), n.inventory, n.hold,
            n.possessed ? 1 : 0, n.camZ, n.black);
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

// The map's lantern: where it is and whether it is lit; then a step: 0 face it, 1 take it in hand, 2 light it with its
// own RMB use, 3 put it down with the player's own forceDrop.
void LampStep(int step) {
    void* cls = OI::ClassByName(L"prop_lamp_C");
    void* lamp = nullptr;
    if (cls)
        OI::ForEachInstance(cls, [](void* ctx, void* o, int32_t) {
            ue_wrap::FVector at{};
            if (!*static_cast<void**>(ctx) && R::IsLive(o) && E::TryGetActorLocation(o, at) && at.Z < -30000.f)
                *static_cast<void**>(ctx) = o;
        }, &lamp);
    ue_wrap::FVector at{}, me{};
    if (!lamp || !E::TryGetActorLocation(lamp, at) || !E::TryGetActorLocation(Me(), me)) {
        UE_LOGI("level-visit: %hs LAMP none in the map", Who());
        return;
    }
    int32_t off = -1;
    uint8_t mask = 0;
    const bool lit = R::FindBoolProperty(R::ClassOf(lamp), L"active", off, mask) && off >= 0 &&
                     (*(static_cast<uint8_t*>(lamp) + off) & mask) != 0;
    const ue_wrap::FVector cam = E::GetCameraLocation();
    const float dx = at.X - cam.X, dy = at.Y - cam.Y, dz = at.Z - cam.Z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    bool took = false, collected = false;
    float fuel = -1.f;
    if (const int32_t fo = R::FindPropertyOffset(R::ClassOf(lamp), L"fuel"); fo >= 0)
        fuel = *reinterpret_cast<float*>(static_cast<char*>(lamp) + fo);
    if (step == 1) took = E::CallMainPlayerHoldObject(Me(), lamp, collected);
    else if (step == 2 || step == 3) {
        void* who = step == 2 ? lamp : Me();
        void* fn = step == 2 ? R::FindFunction(R::ClassOf(lamp), L"playerHandUse_RMB")
                             : R::FindDispatchFunctionCached(R::ClassOf(Me()), L"forceDrop");
        ue_wrap::ParamFrame pf(fn);
        took = fn && pf.valid() && (step == 3 || pf.Set<void*>(L"player", Me())) && ue_wrap::Call(who, pf);
    } else if (void* pc = E::GetController(Me()))
        E::SetControlRotation(pc, {static_cast<float>(std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 57.2958),
                                   static_cast<float>(std::atan2(dy, dx) * 57.2958), 0.f});
    UE_LOGI("level-visit: %hs LAMP at (%.0f,%.0f,%.0f) %.0f cm from the camera lit=%d fuel=%.1f %hs", Who(), at.X, at.Y,
            at.Z, dist, lit ? 1 : 0, fuel,
            step == 0 ? "faced" : step == 1 ? (took ? "taken in hand" : "take failed")
                        : step == 2 ? (took ? "lit with its RMB use" : "RMB use failed")
                                    : (took ? "put down" : "put down failed"));
}

// The game's own clip: its tick traces from lastLoc to the camera, and a hit on static geometry is the trip. lastLoc
// 4 m under the floor puts the floor between them.
void TryClip() {
    OnGameThread([] {
        const int32_t off = R::FindPropertyOffset(R::ClassOf(Me()), L"lastLoc");
        const ue_wrap::FVector cam = E::GetCameraLocation();
        if (off >= 0)
            *reinterpret_cast<ue_wrap::FVector*>(static_cast<char*>(Me()) + off) = {cam.X, cam.Y, cam.Z - 400.f};
        UE_LOGI("level-visit: %hs lastLoc set under the floor for the game's own clip (%d)", Who(), off >= 0 ? 1 : 0);
    });
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

// The game mode's backroomsEnabled, which the game's clip check reads; -1 unreadable.
int GameBackrooms() {
    int v = -1;
    OnGameThread([&v] {
        void* gm = ue_wrap::world_singleton::Gamemode();
        int32_t off = -1;
        uint8_t mask = 0;
        if (gm && R::FindBoolProperty(R::ClassOf(gm), L"backroomsEnabled", off, mask))
            v = (*(static_cast<uint8_t*>(gm) + off) & mask) ? 1 : 0;
    });
    return v;
}

bool BackroomsAllowed() {
    bool on = true;
    OnGameThread([&on] { on = coop::config::ResolveFlag(coop::config_registry::rows::world_backrooms); });
    return on;
}

// VOTVCOOP_LEVEL_VISIT_CLOSED=1: the host closes the backrooms (world.backrooms 0) before the visit. The visitor sees
// the session's value arrive and its game mode's backroomsEnabled held off; the game's own clip and the clip's travel
// call each leave it in the world. The host opens them again with a reset; the visitor sees backroomsEnabled given
// back, then visits as usual. A closed phase that fails fails the visitor's verdict.
std::string g_closedFail;

void ClosedPhaseHost() {
    if (!Await("client_closed_ready")) return;
    OnGameThread([] {
        const auto r = coop::config::SetValue(coop::config_registry::rows::world_backrooms, "0");
        UE_LOGI("level-visit: host closed the backrooms (set result %d)", static_cast<int>(r));
    });
    Write("closed");
    if (!Await("closed_done")) return;
    OnGameThread([] {
        const auto r = coop::config::ResetValue(coop::config_registry::rows::world_backrooms);
        UE_LOGI("level-visit: host opened the backrooms again (reset result %d)", static_cast<int>(r));
    });
    Write("reopened");
}

void ClosedPhaseVisitor(const std::string& map) {
    const int before = GameBackrooms();
    Write("client_closed_ready");
    if (!Await("closed")) {
        g_closedFail = "the host never closed the backrooms";
        return;
    }
    bool seen = false;
    int held = -1;
    for (int t = 0; t < 20 && !seen; ++t) {
        ::Sleep(1000);
        held = GameBackrooms();
        seen = !BackroomsAllowed() && held == 0;
    }
    UE_LOGI("level-visit: %hs CLOSED seen=%d backroomsEnabled %d->%d", Who(), seen ? 1 : 0, before, held);
    if (!seen) g_closedFail = "the closed setting never reached the visitor, or the clip switch was not held off";
    TryClip();
    bool went = false;
    for (int t = 0; t < 8; ++t) {
        ::Sleep(1000);
        went = went || Read().visiting;
    }
    UE_LOGI("level-visit: %hs CLOSED the game's own clip -> visiting=%d", Who(), went ? 1 : 0);
    if (went && g_closedFail.empty()) g_closedFail = "the clip sent the visitor while the backrooms were closed";
    TryCall(map);
    bool sent = false;
    for (int t = 0; t < 8; ++t) {
        ::Sleep(1000);
        sent = sent || Read().visiting;
    }
    UE_LOGI("level-visit: %hs CLOSED the travel call -> visiting=%d", Who(), sent ? 1 : 0);
    if (sent && g_closedFail.empty()) g_closedFail = "the travel call sent the visitor while the backrooms were closed";
    Write("closed_done");
    if (!Await("reopened")) {
        g_closedFail = "the host never opened the backrooms again";
        return;
    }
    bool open = false;
    int after = -1;
    for (int t = 0; t < 20 && !open; ++t) {
        ::Sleep(1000);
        after = GameBackrooms();
        open = BackroomsAllowed() && after == before;
    }
    UE_LOGI("level-visit: %hs OPENED seen=%d backroomsEnabled back to %d (was %d)", Who(), open ? 1 : 0, after, before);
    if (!open && g_closedFail.empty()) g_closedFail = "reopened, the clip switch did not get its own value back";
    ::Sleep(6000);  // past the visit's quiet window, as a player's clip would be
}

void Visitor() {
    const bool clip = coop::config::ReadEnv("VOTVCOOP_LEVEL_VISIT_START") == "clip";
    // The camera the host spawned, in hand (a clip run takes none, carrying what its profile holds, and neither does a
    // run that takes the map's lantern in hand).
    const bool bare = clip || coop::config::ReadEnv("VOTVCOOP_LEVEL_VISIT_LOOK") == "lamp";
    if (!bare) OnGameThread([] {
        void* cls = OI::ClassByName(L"prop_vidcam_low_C");
        void* cam = nullptr;
        if (cls)
            OI::ForEachInstance(cls, [](void* ctx, void* o, int32_t) {
                if (!*static_cast<void**>(ctx) && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__"))
                    *static_cast<void**>(ctx) = o;
            }, &cam);
        ue_wrap::FVector at{};
        if (cam && E::TryGetActorLocation(cam, at)) PlaceMe({at.X - 120.f, at.Y, at.Z + 60.f});
    });
    ::Sleep(1500);
    if (!bare) OnGameThread([] {
        void* cls = OI::ClassByName(L"prop_vidcam_low_C");
        void* cam = nullptr;
        if (cls)
            OI::ForEachInstance(cls, [](void* ctx, void* o, int32_t) {
                if (!*static_cast<void**>(ctx) && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__"))
                    *static_cast<void**>(ctx) = o;
            }, &cam);
        bool collected = false;
        const bool took = cam && E::CallMainPlayerHoldObject(Me(), cam, collected);
        UE_LOGI("level-visit: %hs took the camera ran=%d", Who(), took ? 1 : 0);
    });
    ::Sleep(3000);
    const Now before = Read();
    Log(before);
    std::string map = coop::config::ReadEnv("VOTVCOOP_LEVEL_VISIT_MAP");
    if (map.empty()) map = "sl_backroomsObsolete";
    if (coop::config::ReadEnv("VOTVCOOP_LEVEL_VISIT_START") == "none") {
        // The control: no visit, a lethal hit in the world, and what the player holds after the usual revive.
        OnGameThread([] { E::InvokeAddPlayerDamage(Me(), 1000.f, /*blood=*/true); });
        Write("visit_in");
        Now n;
        bool died = false;
        for (int t = 0; t < 60; ++t) {
            ::Sleep(1000);
            n = Read();
            Log(n);
            died = died || n.health <= 0.f;
            if (died && n.health > 0.f) break;
        }
        ::Sleep(3000);
        n = Read();
        Write("visit_over");
        UE_LOGI("level-visit: %hs a death in the world: held before=%ls after=%ls hold %d->%d", Who(),
                before.heldName.c_str(), n.heldName.c_str(), before.hold, n.hold);
        return Verdict(died && n.health > 0.f, "a death in the world, revived (the control)");
    }
    if (coop::config::ReadEnv("VOTVCOOP_LEVEL_VISIT_CLOSED") == "1") ClosedPhaseVisitor(map);
    if (clip) TryClip();
    else TryCall(map);
    Now n;
    int t = 0;
    for (; t < 60; ++t) {
        ::Sleep(1000);
        n = Read();
        Log(n);
        if (n.visiting && n.at.Z < -30000.f) break;
    }
    if (!(n.visiting && n.at.Z < -30000.f)) { Write("visit_over"); return Verdict(false, "never arrived in the map"); }
    Write("visit_in");
    ::Sleep(2000);
    const Now inMap = Read();
    // In the map the player is this machine's to move: its controller holds it, the view is in the map, uncovered.
    const bool inOk = inMap.possessed && inMap.camZ < -30000.f && inMap.black == 0;
    UE_LOGI("level-visit: %hs in the map possessed=%d camZ=%.0f black=%d", Who(), inMap.possessed ? 1 : 0,
            inMap.camZ, inMap.black);
    const std::string dump = coop::config::ReadEnv("VOTVCOOP_LEVEL_VISIT_DUMP_DIR");
    if (!dump.empty()) DumpLevelClasses(dump);
    // The view in the map, as the visitor sees it: once just in, once the eye has adapted. With
    // VOTVCOOP_LEVEL_VISIT_LOOK=lamp the visitor turns to the map's lantern (prop_lamp_C), takes it in hand, lights it
    // with the lantern's own RMB use, and puts it down before the end, so it comes back with what it took in.
    const bool lamp = coop::config::ReadEnv("VOTVCOOP_LEVEL_VISIT_LOOK") == "lamp";
    for (int i = 0; i < 20; ++i) {
        ::Sleep(1000);
        Log(Read());
        if (i == 1 || i == 15) harness::screenshot::Capture(i == 1 ? L"visit-in-a" : L"visit-in-b");
        if (lamp && (i == 3 || i == 8 || i == 10)) OnGameThread([i] { LampStep(i == 3 ? 0 : i == 8 ? 1 : 2); });
        if (lamp && (i == 6 || i == 13)) harness::screenshot::Capture(i == 6 ? L"visit-lamp-faced" : L"visit-lamp-lit");
        if (lamp && i == 17) OnGameThread([] { LampStep(3); });
    }
    std::string end = coop::config::ReadEnv("VOTVCOOP_LEVEL_VISIT_END");
    if (end.empty()) end = "end";
    OnGameThread([&end] {
        void* script = LevelScriptActor();
        bool ran = false;
        if (end == "wait") {
            ran = true;  // the map's own end, on its own clock
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
    const int bound = end == "wait" ? 700 : 60;
    for (t = 0; t < bound; ++t) {
        ::Sleep(1000);
        n = Read();
        if (end != "wait" || t % 30 == 0 || !n.visiting) Log(n);
        if (!n.visiting && n.at.Z > -30000.f && n.health > 0.f && (end == "wait" || t >= 15)) break;
    }
    Write("visit_over");
    const bool back = !n.visiting && n.at.Z > -30000.f;
    const bool kept = n.held == nullptr ? before.held == nullptr : (before.held && n.hold == before.hold);
    const bool pockets = n.inventory == before.inventory;
    const bool view = n.possessed && std::fabs(n.camZ - n.at.Z) < 300.f;
    UE_LOGI("level-visit: %hs back=%d (last end '%ls') held before=%ls after=%ls hold %d->%d inventory %d->%d", Who(),
            back ? 1 : 0, LV::LastEnd().c_str(), before.heldName.c_str(),
            n.heldName.c_str(), before.hold, n.hold, before.inventory, n.inventory);
    Verdict(back && kept && pockets && inOk && view && g_closedFail.empty(),
            !g_closedFail.empty() ? g_closedFail.c_str()
            : !back    ? "not back in the world"
            : !kept    ? "the hand item did not come back"
            : !pockets ? "the pockets changed"
            : !inOk    ? "in the map the player was not its own to move, or its view was covered"
            : !view    ? "back in the world the view is not the player's"
                       : "back in the world with what it carried, its view its own throughout");
}

void Watcher(int visitorSlot) {
    bool sawHidden = false, shownAfter = false;
    if (!Await("visit_in", 400)) return Verdict(false, "the visitor never arrived");
    for (int t = 0; t < 900 && !shownAfter; ++t) {
        ::Sleep(1000);
        bool hidden = false;
        ue_wrap::FVector at{};
        OnGameThread([&] {
            coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(visitorSlot));
            void* a = p ? p->actor() : nullptr;
            if (a) E::TryGetActorLocation(a, at);
            int32_t off = -1;
            uint8_t mask = 0;
            hidden = a && R::FindBoolProperty(R::ClassOf(a), L"bHidden", off, mask) &&
                     (*(static_cast<uint8_t*>(a) + off) & mask) != 0;
        });
        UE_LOGI("level-visit: STATE %hs sees the visitor's copy at=(%.0f,%.0f,%.0f) hidden=%d", Who(), at.X, at.Y,
                at.Z, hidden ? 1 : 0);
        if (hidden) sawHidden = true;
        if (sawHidden && !hidden && Seen("visit_over")) shownAfter = true;
    }
    Verdict(sawHidden && shownAfter, !sawHidden    ? "never saw the copy hidden"
                                     : !shownAfter ? "the copy was not shown again"
                                                   : "the copy was hidden while away");
}

void Run() {
    if (IsClientRole()) {
        bool ready = false;
        for (int i = 0; i < 480 && !ready; ++i) {
            OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
            if (!ready) ::Sleep(500);
        }
        ::Sleep(10000);
        Write("client_ready");
    } else {
        ::Sleep(20000);
        if (!Await("client_ready")) return;
        OnGameThread([] {
            void* cls = OI::ClassByName(L"prop_vidcam_low_C");
            if (!cls) cls = R::FindClass(L"prop_vidcam_low_C");
            ue_wrap::FVector at{};
            E::TryGetActorLocation(Me(), at);
            void* cam = cls ? E::SpawnActor(cls, {at.X + 150.f, at.Y, at.Z}) : nullptr;
            UE_LOGI("level-visit: host spawned a camera for the visitor (%d)", cam ? 1 : 0);
        });
        ::Sleep(4000);
        Write("spawned");
    }
    if (IsClientRole() && !Await("spawned")) return;
    ::Sleep(3000);
    if (!IsClientRole() && coop::config::ReadEnv("VOTVCOOP_LEVEL_VISIT_CLOSED") == "1") ClosedPhaseHost();
    if (IVisit()) Visitor();
    else Watcher(g_hostVisits ? 0 : 1);
    UE_LOGI("level-visit: DONE %hs", Who());
}

}  // namespace

DWORD WINAPI LevelVisitThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("level-visit: ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    g_hostVisits = coop::config::ReadEnv("VOTVCOOP_LEVEL_VISIT_DOER") == "host";
    Run();
    return 0;
}

}  // namespace harness::autotest
