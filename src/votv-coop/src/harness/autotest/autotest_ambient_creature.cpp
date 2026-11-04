// harness/autotest/autotest_ambient_creature.cpp -- TEST-ONLY: one set of deer and walking trees across two peers.
//
// The host runs the spawners' own ReceiveTick, so each creature comes through the game's roll and the lane's catch. The client
// checks its mirror, its parked spawners, a tree's size and landing, a dissolve of its own and one of the host's, and that a
// roll of its own does not stand. Gated by VOTVCOOP_RUN_AMBIENT_CREATURE; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/creatures/npc_sync.h"
#include "coop/element/mirror_managers.h"
#include "coop/element/registry.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/world_identity.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace AC = ue_wrap::ambient_creature;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;

std::string g_dir;
int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Mark(const char* name, const std::string& body = "x") {
    std::string tmp = g_dir + "\\" + name + ".tmp";
    { std::ofstream(tmp, std::ios::binary | std::ios::trunc) << body; }
    ::MoveFileExA(tmp.c_str(), (g_dir + "\\" + name).c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool Await(const char* name, std::string* body = nullptr, int seconds = 240) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty()) {
            if (body) *body = s;
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("ambient-creature: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void Check(bool ok, const char* what) {
    UE_LOGI("ambient-creature: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

void* FirstOf(const wchar_t* cls) {
    void* found = nullptr;
    if (void* c = OI::ClassByName(cls))
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) *static_cast<void**>(ctx) = obj;
        }, &found);
    return found;
}

// One run of a spawner's own body.
bool RunSpawner(const wchar_t* cls) {
    void* ticker = FirstOf(cls);
    void* fn = ticker ? R::FindDispatchFunctionCached(R::ClassOf(ticker), L"ReceiveTick") : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<float>(L"DeltaSeconds", 0.016f);
    return ue_wrap::Call(ticker, f);
}

// The deer's roll wants night hours: the clock's hour, written where the body reads it.
void MakeNight() {
    void* cycle = FirstOf(L"dayNightCycle_C");
    const int32_t off = cycle ? R::FindPropertyOffset(R::ClassOf(cycle), L"timeZ") : -1;
    if (off >= 0) *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(cycle) + off) = 22;
}

// The deer the spawner's roll just added.
void* NewDeer(const std::vector<void*>& before) {
    std::vector<void*> v;
    AC::ListDeer(v);
    for (void* a : v)
        if (std::find(before.begin(), before.end(), a) == before.end()) return a;
    return nullptr;
}

size_t Count(bool deer) {
    std::vector<void*> v;
    if (deer) AC::ListDeer(v);
    else AC::ListTrees(v);
    return v.size();
}

void LogPose(const char* who, const char* what, void* a, float t) {
    ue_wrap::FVector at{};
    if (a && E::TryGetActorLocation(a, at)) UE_LOGI("ambient-creature: %hs %hs t=%.0f at=(%.0f,%.0f,%.0f)", who, what, t, at.X, at.Y, at.Z);
}

void RunHost() {
    if (!Await("client_ready")) return;
    // The tree: one real roll, then its fall, landing and wander in both logs.
    size_t before = 0;
    OnGameThread([&] { before = Count(false); RunSpawner(L"ticker_treeSpawner_C"); });
    void* tree = nullptr;
    for (int i = 0; i < 100 && !tree; ++i) {
        ::Sleep(100);
        OnGameThread([&] { std::vector<void*> v; AC::ListTrees(v); if (v.size() > before) tree = v.back(); });
    }
    Check(tree != nullptr, "the spawner's own body rolled a tree");
    if (!tree) return;
    std::string body;
    uint32_t eid = 0;
    float scale = 0.f;
    bool landed = false;
    for (int i = 0; i < 50 && !eid; ++i) {
        ::Sleep(100);
        OnGameThread([&] { eid = coop::npc_sync::GetNpcIdForActor(tree); AC::ReadTree(tree, scale, landed); });
    }
    Check(eid != 0, "the tree has an element");
    Mark("tree", std::to_string(eid) + " " + std::to_string(scale));
    for (int t = 0; t < 180 && !landed; ++t) {
        ::Sleep(1000);
        OnGameThread([&] { if (R::IsLive(tree)) { AC::ReadTree(tree, scale, landed); if (t % 10 == 0) LogPose("host", "tree", tree, float(t)); } });
    }
    Check(landed, "the host's tree landed");
    OnGameThread([&] { LogPose("host", "tree landed", tree, 0.f); });
    Mark("tree_landed", "x");
    if (!Await("tree_done")) return;

    // The deer: its roll is one in fifty, so the body runs until one stands.
    void* deer = nullptr;
    std::vector<void*> had;
    OnGameThread([&] { AC::ListDeer(had); });
    for (int i = 0; i < 2000 && !deer; ++i) {
        OnGameThread([&] {
            MakeNight();
            RunSpawner(L"ticker_deerSpawner_C");
            deer = NewDeer(had);
        });
        ::Sleep(5);
    }
    Check(deer != nullptr, "the spawner's own body rolled a deer");
    if (!deer) return;
    ::Sleep(3000);
    OnGameThread([&] { eid = coop::npc_sync::GetNpcIdForActor(deer); });
    Mark("deer", std::to_string(eid));
    for (int t = 0; t < 30; t += 5) {
        ::Sleep(5000);
        OnGameThread([&] { LogPose("host", "deer", deer, float(t)); });
    }
    // Beside both players at base, with the host's own test off: only the client's can dissolve it.
    OnGameThread([&] {
        ue_wrap::FVector me{};
        void* pawn = coop::players::Registry::Get().Local();
        E::SetActorTickEnabled(deer, false);
        if (pawn && E::TryGetActorLocation(pawn, me)) E::TeleportTo(deer, {me.X + 2000.f, me.Y, me.Z}, {});
    });
    if (!Await("deer_near_done")) return;
    bool alive = false;
    OnGameThread([&] { alive = R::IsLive(deer); });
    Check(alive, "the client's nearness left the host's deer standing");
    // The host's own dissolve reaches the client as its end, for a second deer the client stands far from.
    void* second = nullptr;
    had.clear();
    OnGameThread([&] { AC::ListDeer(had); });
    for (int i = 0; i < 2000 && !second; ++i) {
        OnGameThread([&] {
            MakeNight();
            RunSpawner(L"ticker_deerSpawner_C");
            second = NewDeer(had);
        });
        ::Sleep(5);
    }
    Check(second != nullptr, "a second deer stands");
    if (!second) return;
    ::Sleep(3000);
    uint32_t secondEid = 0;
    OnGameThread([&] { secondEid = coop::npc_sync::GetNpcIdForActor(second); });
    Mark("deer2", std::to_string(secondEid));
    if (!Await("deer2_seen")) return;
    OnGameThread([&] { AC::Dissolve(second); });
    UE_LOGI("ambient-creature: the host's second deer dissolves");
    Mark("deer2_gone_host", "x");
    Await("client_done");
    UE_LOGI("ambient-creature: DONE on host fails=%d", g_fails);
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(8000);
    OnGameThread([&] {
        void* deerTicker = FirstOf(L"ticker_deerSpawner_C");
        void* treeTicker = FirstOf(L"ticker_treeSpawner_C");
        UE_LOGI("ambient-creature: client spawners deer=%d tree=%d", deerTicker ? 1 : 0, treeTicker ? 1 : 0);
        Check(deerTicker && !E::IsActorTickEnabled(deerTicker), "this client's deer spawner is parked");
        Check(treeTicker && !E::IsActorTickEnabled(treeTicker), "this client's tree spawner is parked");
    });

    // A roll of this client's own does not stand.
    OnGameThread([] { RunSpawner(L"ticker_treeSpawner_C"); });
    ::Sleep(2500);
    size_t own = 99;
    OnGameThread([&] { own = Count(false); });
    Check(own == 0, "a tree this client rolled itself was swept");
    Mark("client_ready");

    std::string body;
    if (!Await("tree", &body)) return;
    const uint32_t treeEid = static_cast<uint32_t>(std::strtoul(body.c_str(), nullptr, 10));
    const float hostScale = std::strtof(body.c_str() + body.find(' '), nullptr);
    void* tree = nullptr;
    for (int i = 0; i < 100 && !tree; ++i) {
        ::Sleep(100);
        OnGameThread([&] { tree = coop::element::LiveActorOfType(treeEid, coop::element::ElementType::Npc); });
    }
    Check(tree != nullptr, "the host's tree has its mirror here");
    if (!tree) return;
    ::Sleep(1000);
    float scale = 0.f;
    bool landed = true;
    OnGameThread([&] { AC::ReadTree(tree, scale, landed); });
    UE_LOGI("ambient-creature: tree scale here=%.2f host=%.2f", scale, hostScale);
    Check(scale > hostScale - 0.01f && scale < hostScale + 0.01f, "the tree's size is the host's");
    Check(!landed, "a falling tree stays hidden here");
    if (!Await("tree_landed")) return;
    ::Sleep(2000);
    OnGameThread([&] { if (R::IsLive(tree)) { AC::ReadTree(tree, scale, landed); LogPose("client", "tree landed", tree, 0.f); } });
    Check(landed, "the landed tree's mesh shows here");
    Mark("tree_done");

    if (!Await("deer", &body)) return;
    const uint32_t deerEid = static_cast<uint32_t>(std::strtoul(body.c_str(), nullptr, 10));
    void* deer = nullptr;
    for (int i = 0; i < 100 && !deer; ++i) {
        ::Sleep(100);
        OnGameThread([&] { deer = coop::element::LiveActorOfType(deerEid, coop::element::ElementType::Npc); });
    }
    Check(deer != nullptr, "the host's deer has its mirror here");
    if (!deer) return;
    for (int t = 0; t < 25; t += 5) {
        ::Sleep(5000);
        OnGameThread([&] { LogPose("client", "deer", deer, float(t)); });
    }
    // The host brings it near: it dissolves for this viewer alone, in this same world.
    void* const world = ue_wrap::world_identity::CurrentWorld();
    bool gone = false;
    for (int i = 0; i < 200 && !gone; ++i) {
        ::Sleep(100);
        OnGameThread([&] { gone = coop::element::LiveActorOfType(deerEid, coop::element::ElementType::Npc) == nullptr; });
    }
    Check(gone && ue_wrap::world_identity::CurrentWorld() == world, "the near deer dissolved for this viewer, in the same world");
    Mark("deer_near_done");

    if (!Await("deer2", &body)) return;
    const uint32_t secondEid = static_cast<uint32_t>(std::strtoul(body.c_str(), nullptr, 10));
    void* second = nullptr;
    for (int i = 0; i < 100 && !second; ++i) {
        ::Sleep(100);
        OnGameThread([&] { second = coop::element::LiveActorOfType(secondEid, coop::element::ElementType::Npc); });
    }
    Check(second != nullptr, "the second deer has its mirror here");
    Mark("deer2_seen");
    if (!Await("deer2_gone_host")) return;
    bool secondGone = false;
    for (int i = 0; i < 200 && !secondGone; ++i) {
        ::Sleep(100);
        OnGameThread([&] { secondGone = coop::element::LiveActorOfType(secondEid, coop::element::ElementType::Npc) == nullptr; });
    }
    Check(second && secondGone, "the host's dissolved deer ended here too");
    Mark("client_done");
    UE_LOGI("ambient-creature: DONE on client fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI AmbientCreatureThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("ambient-creature: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest
