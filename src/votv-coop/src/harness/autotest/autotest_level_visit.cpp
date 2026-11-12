// harness/autotest/autotest_level_visit.cpp -- TEST-ONLY: a clip's trip to another map is the clipping player's alone,
// and comes back as a death does (coop/player/level_visit).

// The visitor (VOTVCOOP_LEVEL_VISIT_DOER, client by default) takes a camera the host spawns into its hand, then calls
// the game's own lib_C::loadLevel for VOTVCOOP_LEVEL_VISIT_MAP (sl_backroomsObsolete by default) as the clip does.
// With VOTVCOOP_LEVEL_VISIT_START=clip the trip is the game's own clip instead: no hand item (the game sends no player
// holding something) and lastLoc under the floor, and the map is the clip's pick.

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
#include "coop/player/level_visit.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
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
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <atomic>
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
    int inventory = -1, hold = -1;
    bool visiting = false;
};

Now Read() {
    Now n;
    OnGameThread([&n] {
        if (Me()) E::TryGetActorLocation(Me(), n.at);
        ue_wrap::vitals::Read(ue_wrap::vitals::Field::Health, &n.health);
        const int32_t off = Me() ? R::FindPropertyOffset(R::ClassOf(Me()), L"holding_actor") : -1;
        n.held = off >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(Me()) + off) : nullptr;
        n.inventory = ArrayNum(L"inventoryData");
        n.hold = ArrayNum(L"hold");
        n.visiting = LV::Visiting();
    });
    return n;
}

void Log(const Now& n) {
    UE_LOGI("level-visit: STATE %hs at=(%.0f,%.0f,%.0f) health=%.0f visiting=%d map=%ls held=%ls inventory=%d hold=%d",
            Who(), n.at.X, n.at.Y, n.at.Z, n.health, n.visiting ? 1 : 0, LV::CurrentMap().c_str(),
            n.held ? R::ClassNameOf(n.held).c_str() : L"none", n.inventory, n.hold);
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

void Visitor() {
    const bool clip = coop::config::ReadEnv("VOTVCOOP_LEVEL_VISIT_START") == "clip";
    // The camera the host spawned, in hand (not for a clip: the game sends no player holding something).
    if (!clip) OnGameThread([] {
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
    if (!clip) OnGameThread([] {
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
    if (clip) OnGameThread([] {
        // The game's own clip: its tick traces from lastLoc to the camera, and a hit on static geometry is the trip.
        // lastLoc 4 m under the floor puts the floor between them.
        const int32_t off = R::FindPropertyOffset(R::ClassOf(Me()), L"lastLoc");
        const ue_wrap::FVector cam = E::GetCameraLocation();
        if (off >= 0)
            *reinterpret_cast<ue_wrap::FVector*>(static_cast<char*>(Me()) + off) = {cam.X, cam.Y, cam.Z - 400.f};
        UE_LOGI("level-visit: %hs lastLoc set under the floor for the game's own clip (%d)", Who(), off >= 0 ? 1 : 0);
    });
    else OnGameThread([&map] {
        void* lib = R::FindClassDefaultObject(L"lib_C");
        void* fn = lib ? R::FindFunction(R::ClassOf(lib), L"loadLevel") : nullptr;
        ue_wrap::ParamFrame pf(fn);
        const std::wstring w(map.begin(), map.end());
        const bool ran = fn && pf.valid() && pf.Set<R::FName>(L"level", ue_wrap::fname_utils::StringToFName(w)) &&
                         pf.Set<bool>(L"clearSubArea", true) && pf.Set<void*>(L"__WorldContext", Me()) &&
                         ue_wrap::Call(lib, pf);
        UE_LOGI("level-visit: %hs called lib_C::loadLevel('%hs') ran=%d", Who(), map.c_str(), ran ? 1 : 0);
    });
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
    const std::string dump = coop::config::ReadEnv("VOTVCOOP_LEVEL_VISIT_DUMP_DIR");
    if (!dump.empty()) DumpLevelClasses(dump);
    for (int i = 0; i < 20; ++i) { ::Sleep(1000); Log(Read()); }
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
    UE_LOGI("level-visit: %hs back=%d (last end '%ls') held before=%ls after=%ls hold %d->%d inventory %d->%d", Who(),
            back ? 1 : 0, LV::LastEnd().c_str(), before.held ? R::ClassNameOf(before.held).c_str() : L"none",
            n.held ? R::ClassNameOf(n.held).c_str() : L"none", before.hold, n.hold, before.inventory, n.inventory);
    Verdict(back && kept && pockets, !back ? "not back in the world" : !kept ? "the hand item did not come back"
                                     : !pockets ? "the pockets changed" : "back in the world with what it carried");
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
