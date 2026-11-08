// harness/autotest/autotest_mushroom.cpp -- TEST-ONLY: the mushrooms on two peers, the host's alone.
//
// The game makes mushrooms three ways, each driven here through the game's own classes:
//   - a wild mushroom: a mushroomSpawner_C named by a list_shrooms row makes the prop at its own transform. The
//     host's stands on the client once; the client picks it up and it leaves the host's world. A client's own
//     spawner makes nothing on either peer (coop/world/spawn_authority).
//   - the growing stage, prop_food_mushroom7_C: the host's never crosses; its Transform makes a mature puffball,
//     which stands on the client once.
//   - a mushroom plant, growingPlant_shroom_7_C: a puffball's explosion throws 0 to 3 spores, and a spore's hit on
//     grass, dirt or gravel plants one. Thrown spores can rest without planting, so each peer also runs a spore's
//     hit event on traced ground with its physical material. The host's plants stand on the client under their
//     keys; a client's explosions and spore hit plant nothing on either peer.
// Each check logs `mushroom: PASS|FAIL <what>` or `mushroom: SKIP <why>`, ending with `mushroom: DONE fails=N`.
// Gated by VOTVCOOP_RUN_MUSHROOM; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"
#include "harness/autotest/peer_signal.h"

#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/growing_plant.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/data_table.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/core/call.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GP = ue_wrap::growing_plant;
namespace PS = harness::autotest::peer_signal;
using PS::Await;
using PS::AwaitText;
using PS::Mark;
using PS::WaitFor;

int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Check(bool ok, const std::string& what) {
    if (ok) {
        UE_LOGI("mushroom: PASS %hs", what.c_str());
    } else {
        ++g_fails;
        UE_LOGW("mushroom: FAIL %hs", what.c_str());
    }
}

void Skip(const std::string& why) { UE_LOGW("mushroom: SKIP %hs", why.c_str()); }

std::string Narrow(const std::wstring& w) {
    std::string out;
    for (wchar_t c : w) out += (c > 0 && c < 128) ? static_cast<char>(c) : '?';
    return out;
}

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::string FormatPoint(const ue_wrap::FVector& p) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%.1f %.1f %.1f", p.X, p.Y, p.Z);
    return buf;
}

ue_wrap::FVector ParsePoint(const std::string& s) {
    ue_wrap::FVector p{};
    std::istringstream in(s);
    in >> p.X >> p.Y >> p.Z;
    return p;
}

// A point beside this peer's player: `ahead` cm along X and `up` cm above its centre.
bool Beside(float ahead, float up, ue_wrap::FVector& out) {
    bool ok = false;
    OnGameThread([&] {
        void* pawn = coop::players::Registry::Get().Local();
        ok = pawn && E::TryGetActorLocation(pawn, out);
    });
    out.X += ahead;
    out.Z += up;
    return ok;
}

void* ClassOf(const wchar_t* name, const wchar_t* path) {
    void* cls = R::FindClass(name);
    if (!cls && path) cls = ue_wrap::asset_load::LoadObjectByPath(path);
    return cls;
}

// ---- props near a point ----

struct NearProp {
    void* actor;
    std::string key, cls;
    ue_wrap::FVector at;
};

struct NearScan {
    ue_wrap::FVector at;
    float radius;
    const wchar_t* onlyClass;  // null: every keyed prop class
    std::vector<NearProp>* out;
};

// Every keyed prop within `radius` of `at`, of `onlyClass` or of any prop class. Game thread.
std::vector<NearProp> PropsNear(const ue_wrap::FVector& at, float radius, const wchar_t* onlyClass = nullptr) {
    std::vector<NearProp> out;
    NearScan scan{at, radius, onlyClass, &out};
    std::vector<void*> classes;
    ue_wrap::object_index::ForEachClass([](void* c, void* cls, void*) {
        static_cast<std::vector<void*>*>(c)->push_back(cls);
    }, &classes);
    for (void* cls : classes) {
        if (ue_wrap::prop::KeyFieldOf(cls) != ue_wrap::prop::KeyField::Prop) continue;
        if (onlyClass && R::ToString(R::NameOf(cls)) != onlyClass) continue;
        ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
            auto* s = static_cast<NearScan*>(c);
            if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            ue_wrap::FVector p{};
            if (!E::TryGetActorLocation(obj, p) || Dist(p, s->at) > s->radius) return;
            s->out->push_back({obj, Narrow(ue_wrap::prop::GetKeyString(obj)), Narrow(R::ClassNameOf(obj)), p});
        }, &scan);
    }
    return out;
}

std::set<std::string> KeysOf(const std::vector<NearProp>& props) {
    std::set<std::string> keys;
    for (const NearProp& p : props) keys.insert(p.key);
    return keys;
}

// The props near `at` that were not there before (by key).
std::vector<NearProp> NewSince(const std::set<std::string>& before, const ue_wrap::FVector& at, float radius) {
    std::vector<NearProp> out;
    OnGameThread([&] {
        for (NearProp& p : PropsNear(at, radius))
            if (!before.count(p.key)) out.push_back(p);
    });
    return out;
}

std::set<std::string> KeysNear(const ue_wrap::FVector& at, float radius) {
    std::set<std::string> keys;
    OnGameThread([&] { keys = KeysOf(PropsNear(at, radius)); });
    return keys;
}

// How many copies of `key` stand within `radius` of `at`, of the same class: one, or a duplicate.
int CopiesNear(const std::string& key, const std::string& cls, const ue_wrap::FVector& at, float radius) {
    int n = 0;
    OnGameThread([&] {
        const std::wstring wc = Widen(cls);
        for (const NearProp& p : PropsNear(at, radius, wc.c_str())) n += (p.cls == cls) ? 1 : 0;
    });
    (void)key;
    return n;
}

bool HasKey(const std::string& key) {
    bool there = false;
    OnGameThread([&] { there = coop::prop_key_index::Find(Widen(key)) != nullptr; });
    return there;
}

// ---- the game's own mushroom makers ----

// The list_shrooms row's prop name the master hands its spawners (struct_shroom.prop); "" when it does not read.
std::wstring FirstShroomProp() {
    std::wstring out;
    OnGameThread([&] {
        void* table = R::FindObject(L"list_shrooms", L"DataTable");
        void* rs = table ? ue_wrap::data_table::RowStruct(table) : nullptr;
        const int32_t off = rs ? R::FindPropertyOffsetByPrefix(rs, L"prop_") : -1;
        std::vector<ue_wrap::data_table::RowRef> rows;
        if (off < 0 || !ue_wrap::data_table::Rows(table, rows, "mushroom")) return;
        for (const auto& r : rows) {
            const auto* name = reinterpret_cast<const R::FName*>(r.row + off);
            const std::wstring s = R::ToString(*name);
            if (!s.empty() && s != L"None") {
                out = s;
                return;
            }
        }
    });
    return out;
}

// A mushroomSpawner_C at `at`, named `prop` and not waiting to be unseen, and its Spawn: what the master's children
// run when their timer fires. True when the call dispatched.
bool RunSpawner(const ue_wrap::FVector& at, const std::wstring& prop) {
    bool ok = false;
    OnGameThread([&] {
        void* cls = ClassOf(L"mushroomSpawner_C", L"/Game/objects/mushroomSpawner.mushroomSpawner_C");
        void* sp = cls ? E::SpawnActor(cls, at) : nullptr;
        if (!sp) return;
        const int32_t offName = R::FindPropertyOffset(cls, L"Name");
        int32_t offRendered = -1;
        uint8_t mask = 0;
        if (offName < 0 || !R::FindBoolProperty(cls, L"useRendered", offRendered, mask)) return;
        *reinterpret_cast<R::FName*>(static_cast<uint8_t*>(sp) + offName) = ue_wrap::fname_utils::StringToFName(prop);
        static_cast<uint8_t*>(sp)[offRendered] &= static_cast<uint8_t>(~mask);
        ok = ue_wrap::component_calls::CallParamlessNamed(sp, L"Spawn");
    });
    return ok;
}

void* SpawnAt(const wchar_t* name, const wchar_t* path, const ue_wrap::FVector& at) {
    void* out = nullptr;
    OnGameThread([&] {
        void* cls = ClassOf(name, path);
        out = cls ? E::SpawnActor(cls, at) : nullptr;
    });
    return out;
}

// ---- mushroom plants ----

// Ground a spore can plant on, near this peer's player: the first point, out from it in rings of 5 m to 60 m, whose
// downward trace lands on grass, dirt or gravel (mushroomSpore_C's own list). False when none is in reach.
bool PlantableGround(ue_wrap::FVector& out, ue_wrap::trace::Hit* ground = nullptr) {
    bool found = false;
    OnGameThread([&] {
        void* pawn = coop::players::Registry::Get().Local();
        ue_wrap::FVector me{};
        if (!pawn || !E::TryGetActorLocation(pawn, me)) return;
        for (int ring = 1; ring <= 12 && !found; ++ring) {
            for (int dir = 0; dir < 16 && !found; ++dir) {
                const float a = 6.2831853f * static_cast<float>(dir) / 16.f;
                const float r = 500.f * static_cast<float>(ring);
                const ue_wrap::FVector top{me.X + r * std::cos(a), me.Y + r * std::sin(a), me.Z + 3000.f};
                const ue_wrap::FVector bottom{top.X, top.Y, me.Z - 3000.f};
                ue_wrap::trace::Hit hit;
                if (!ue_wrap::trace::LineHitStatDyn(pawn, top, bottom, &hit) || !hit.blocked || !hit.physMaterial) continue;
                const std::wstring mat = R::ToString(R::NameOf(hit.physMaterial));
                if (mat == L"grass" || mat == L"dirt" || mat == L"gravel") {
                    out = hit.point;
                    if (ground) *ground = hit;
                    found = true;
                    UE_LOGI("mushroom: plantable ground (%ls) at %.0f %.0f %.0f, %.0f m out", mat.c_str(), out.X, out.Y,
                            out.Z, r / 100.f);
                }
            }
        }
    });
    return found;
}

std::set<std::string> ShroomPlants() {
    std::set<std::string> keys;
    OnGameThread([&] {
        std::vector<void*> plants;
        GP::List(plants);
        for (void* p : plants)
            if (R::ClassNameOf(p) == L"growingPlant_shroom_7_C") keys.insert(Narrow(GP::Key(p)));
    });
    return keys;
}

std::string Join(const std::set<std::string>& keys) {
    std::string out;
    for (const std::string& k : keys) out += (out.empty() ? "" : ",") + k;
    return out.empty() ? "none" : out;
}

std::set<std::string> Split(const std::string& s) {
    std::set<std::string> out;
    if (s == "none") return out;
    std::stringstream in(s);
    std::string k;
    while (std::getline(in, k, ',')) out.insert(k);
    return out;
}

// The plants that appeared since `before`, read after `seconds` more for the spores in flight to land.
std::set<std::string> PlantsSince(const std::set<std::string>& before, int seconds) {
    ::Sleep(static_cast<DWORD>(seconds) * 1000);
    std::set<std::string> fresh;
    for (const std::string& k : ShroomPlants())
        if (!before.count(k)) fresh.insert(k);
    return fresh;
}

// A mushroomSpore_C over the traced ground, and its own hit event with a hit on that ground's component and
// physical material: the body a spore's landing runs. True when the call dispatched (a client's is refused inside).
bool SporeHit() {
    ue_wrap::FVector at{};
    ue_wrap::trace::Hit ground;
    if (!PlantableGround(at, &ground) || !ground.actor || !ground.component) return false;
    bool ok = false;
    OnGameThread([&] {
        void* cls = R::FindClass(L"mushroomSpore_C");
        void* spore = cls ? E::SpawnActor(cls, {at.X, at.Y, at.Z + 30.f}) : nullptr;
        void* fn = spore ? R::FindDispatchFunctionCached(cls,
                               L"BndEvt__mushroomSpore_Sphere_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature")
                         : nullptr;
        if (!fn) return;
        ue_wrap::ParamFrame f(fn);
        if (!f.valid() || !f.Set<void*>(L"OtherActor", ground.actor) || !f.Set<void*>(L"OtherComp", ground.component))
            return;
        f.Set<ue_wrap::FVector>(L"NormalImpulse", ue_wrap::FVector{0.f, 0.f, 1.f});
        if (!ue_wrap::hit_result::Write(f, L"Hit", ground.actor, ground.component, at, ground.physMaterial)) return;
        ok = ue_wrap::Call(spore, f);
    });
    UE_LOGI("mushroom: a spore's own hit on the traced ground %hs", ok ? "dispatched" : "NOT dispatched");
    return ok;
}

// The live spores and where they are, logged: what an explosion threw and where it went.
void LogSpores(const char* when) {
    OnGameThread([&] {
        void* cls = R::FindClass(L"mushroomSpore_C");
        if (!cls) {
            UE_LOGI("mushroom: spores %hs: the class is not loaded", when);
            return;
        }
        struct Acc { int n = 0; std::string where; } acc;
        ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
            auto* a = static_cast<Acc*>(c);
            if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            ue_wrap::FVector p{};
            if (!E::TryGetActorLocation(obj, p)) return;
            ++a->n;
            if (a->n <= 4) a->where += " (" + FormatPoint(p) + ")";
        }, &acc);
        UE_LOGI("mushroom: spores %hs: %d live%hs", when, acc.n, acc.where.c_str());
    });
}

// Puffball explosions 150 cm over plantable ground, `count` of them 5 s apart, or until one plants when
// `untilPlanted`. Returns how many were set off; 0 when no ground or class was found.
int Explode(int count, bool untilPlanted, const std::set<std::string>& before) {
    ue_wrap::FVector at{};
    if (!PlantableGround(at)) return 0;
    at.Z += 150.f;
    int n = 0;
    for (; n < count; ++n) {
        if (!SpawnAt(L"puffballExplosion_C", nullptr, at)) break;
        ::Sleep(300);
        LogSpores("0.3 s after an explosion");
        ::Sleep(2000);
        LogSpores("2.3 s after it");
        if (untilPlanted && !PlantsSince(before, 5).empty()) return n + 1;
        if (!untilPlanted) ::Sleep(5000);
    }
    return n;
}

// ---- the picker ----

bool PickUp(const std::string& key) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        OnGameThread([&] {
            void* player = coop::players::Registry::Get().Local();
            void* prop = coop::prop_key_index::Find(Widen(key));
            bool collected = false;
            if (player && prop) E::CallMainPlayerHoldObject(player, prop, collected);
        });
        if (WaitFor("the mushroom in the hand", 3, [&] {
                bool held = false;
                OnGameThread([&] { held = coop::hand_item::LocalHandActor() != nullptr; });
                return held;
            }, false))
            return true;
    }
    return false;
}

// ---- the two roles ----

void RunHost() {
    if (!Await("client_ready")) return;

    // 1. The host's wild mushroom.
    const std::wstring prop = FirstShroomProp();
    UE_LOGI("mushroom: the first list_shrooms prop is '%ls'", prop.c_str());
    ue_wrap::FVector at{};
    std::string wildKey = "none", wildCls, wildAt;
    if (prop.empty() || !Beside(150.f, 0.f, at)) {
        Skip("1: no list_shrooms prop or no host player");
    } else {
        const std::set<std::string> before = KeysNear(at, 150.f);
        Check(RunSpawner(at, prop), "1: the host's mushroom spawner ran its Spawn");
        std::vector<NearProp> fresh;
        WaitFor("the host's wild mushroom", 10, [&] { return !(fresh = NewSince(before, at, 150.f)).empty(); }, false);
        Check(fresh.size() == 1, "1: the host's spawner made one mushroom (" + std::to_string(fresh.size()) + ")");
        if (!fresh.empty()) {
            wildKey = fresh[0].key;
            wildCls = fresh[0].cls;
            wildAt = FormatPoint(fresh[0].at);
            UE_LOGI("mushroom: the host's wild mushroom %hs %hs at %hs", wildCls.c_str(), wildKey.c_str(),
                    wildAt.c_str());
        }
    }
    Mark("wild", wildKey + "|" + wildCls + "|" + wildAt);
    if (!Await("wild_seen")) return;

    // 2. The client picks it up: it leaves the host's world.
    if (!Await("wild_picked")) return;
    if (wildKey != "none") {
        const bool gone = WaitFor("the picked mushroom gone from the host", 15, [&] { return !HasKey(wildKey); }, false);
        Check(gone, "2: the client's pickup took the host's mushroom out of the world");
    }
    Mark("wild_gone_seen");

    // 3. The client's own spawner: nothing on the host where it stood.
    std::string clientSpot;
    if (!AwaitText("client_spawner", clientSpot)) return;
    if (clientSpot != "none") {
        const ue_wrap::FVector cAt = ParsePoint(clientSpot);
        const std::set<std::string> before = KeysNear(cAt, 150.f);
        ::Sleep(3000);
        Check(NewSince(before, cAt, 150.f).empty(), "3: the client's own spawner made nothing on the host");
    }
    Mark("client_spawner_seen");

    // 4. The growing stage stays the host's; its Transform makes a mature puffball the client sees.
    std::string puffKey = "none", puffCls, puffAt;
    ue_wrap::FVector gAt{};
    if (Beside(-150.f, 0.f, gAt)) {
        void* young = SpawnAt(L"prop_food_mushroom7_C", L"/Game/objects/prop_food_mushroom7.prop_food_mushroom7_C", gAt);
        Check(young != nullptr, "4: the host's growing stage stands");
        Mark("young", FormatPoint(gAt));
        if (!Await("young_seen")) return;
        constexpr float kAnywhere = 1.0e7f;  // the stage falls before it turns: the puffball is found by class
        std::set<std::string> before;
        OnGameThread([&] { before = KeysOf(PropsNear(gAt, kAnywhere, L"prop_puffballMature_C")); });
        bool turned = false;
        for (int i = 0; i < 400 && young && !turned; ++i) {
            OnGameThread([&] {
                if (R::IsLive(young)) ue_wrap::component_calls::CallParamlessNamed(young, L"Transform");
                turned = !R::IsLive(young);
            });
        }
        Check(turned, "4: the growing stage's Transform turned it");
        std::vector<NearProp> fresh;
        WaitFor("the host's mature puffball", 10, [&] {
            fresh.clear();
            OnGameThread([&] {
                for (NearProp& p : PropsNear(gAt, kAnywhere, L"prop_puffballMature_C"))
                    if (!before.count(p.key)) fresh.push_back(p);
            });
            return !fresh.empty();
        }, false);
        Check(fresh.size() == 1, "4: the Transform made one mature puffball (" + std::to_string(fresh.size()) + ")");
        if (!fresh.empty()) {
            puffKey = fresh[0].key;
            puffCls = fresh[0].cls;
            puffAt = FormatPoint(fresh[0].at);
        }
    } else {
        Mark("young", "none");
        if (!Await("young_seen")) return;
    }
    Mark("puffball", puffKey + "|" + puffCls + "|" + puffAt);
    if (!Await("puffball_seen")) return;

    // 5. The host's puffball explosion: the plants its spores make stand on the client.
    std::set<std::string> plantsBefore = ShroomPlants();
    std::set<std::string> fresh;
    const int set = Explode(10, true, plantsBefore);
    if (set == 0) {
        Skip("5: no grass, dirt or gravel within 60 m of the host, or no puffball explosion class");
    } else {
        fresh = PlantsSince(plantsBefore, 10);
        UE_LOGI("mushroom: the host's %d explosion(s) planted %d mushroom plant(s): %hs", set,
                static_cast<int>(fresh.size()), Join(fresh).c_str());
        if (fresh.empty()) Skip("5: the host's thrown spores came to rest without planting (the game's surface test)");
    }
    {
        const std::set<std::string> beforeHit = ShroomPlants();
        Check(SporeHit(), "5: the host's spore hit ran");
        const std::set<std::string> hit = PlantsSince(beforeHit, 3);
        Check(hit.size() == 1, "5: the host's spore hit planted one mushroom plant (" + Join(hit) + ")");
        fresh.insert(hit.begin(), hit.end());
    }
    Mark("host_plants", Join(fresh));
    if (!Await("host_plants_seen")) return;

    // 6. The client's own explosion: no plant on the host.
    plantsBefore = ShroomPlants();
    Mark("client_throw_go");
    if (!Await("client_threw")) return;
    const std::set<std::string> after = PlantsSince(plantsBefore, 10);
    Check(after.empty(), "6: the client's own explosions and spore hit planted nothing on the host (" + Join(after) + ")");
    Mark("host_done");
    if (!Await("client_done")) return;
    UE_LOGI("mushroom: DONE fails=%d", g_fails);
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    if (!WaitFor("the host's puppet", 60, [&] {
            bool have = false;
            OnGameThread([&] {
                coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
                have = p && p->valid();
            });
            return have;
        }))
        return;
    ::Sleep(10000);  // the join settles before the props are read (local/rig notes)
    Mark("client_ready");

    // 1. The host's wild mushroom stands here, once.
    std::string wild;
    if (!AwaitText("wild", wild)) return;
    std::string wildKey, wildCls, wildAt;
    {
        std::stringstream in(wild);
        std::getline(in, wildKey, '|');
        std::getline(in, wildCls, '|');
        std::getline(in, wildAt, '|');
    }
    if (wildKey != "none") {
        const ue_wrap::FVector at = ParsePoint(wildAt);
        const bool there = WaitFor("the host's wild mushroom here", 15, [&] { return HasKey(wildKey); }, false);
        Check(there, "1: the host's wild mushroom stands on the client under its key");
        Check(CopiesNear(wildKey, wildCls, at, 100.f) == 1,
              "1: one copy of it here (" + std::to_string(CopiesNear(wildKey, wildCls, at, 100.f)) + ")");
    }
    Mark("wild_seen");

    // 2. This player picks it up.
    if (wildKey != "none") Check(PickUp(wildKey), "2: the client's player picked the host's mushroom up");
    Mark("wild_picked");
    if (!Await("wild_gone_seen")) return;

    // 3. This peer's own spawner makes nothing here.
    const std::wstring prop = FirstShroomProp();
    ue_wrap::FVector at{};
    if (prop.empty() || !Beside(150.f, 0.f, at)) {
        Skip("3: no list_shrooms prop or no client player");
        Mark("client_spawner", "none");
    } else {
        const std::set<std::string> before = KeysNear(at, 150.f);
        RunSpawner(at, prop);
        ::Sleep(3000);
        const std::vector<NearProp> fresh = NewSince(before, at, 150.f);
        Check(fresh.empty(), "3: the client's own spawner made nothing here (" + std::to_string(fresh.size()) + ")");
        Mark("client_spawner", FormatPoint(at));
    }
    if (!Await("client_spawner_seen")) return;

    // 4. The host's growing stage is not here; its mature puffball is, once.
    std::string young;
    if (!AwaitText("young", young)) return;
    if (young != "none") {
        ::Sleep(3000);
        int n = 0;
        const ue_wrap::FVector gAt = ParsePoint(young);
        OnGameThread([&] { n = static_cast<int>(PropsNear(gAt, 300.f, L"prop_food_mushroom7_C").size()); });
        Check(n == 0, "4: the host's growing stage did not cross (" + std::to_string(n) + " here)");
    }
    Mark("young_seen");
    std::string puff;
    if (!AwaitText("puffball", puff)) return;
    std::string puffKey, puffCls, puffAt;
    {
        std::stringstream in(puff);
        std::getline(in, puffKey, '|');
        std::getline(in, puffCls, '|');
        std::getline(in, puffAt, '|');
    }
    if (puffKey != "none") {
        const bool there = WaitFor("the host's mature puffball here", 15, [&] { return HasKey(puffKey); }, false);
        Check(there, "4: the host's mature puffball stands on the client under its key");
        Check(CopiesNear(puffKey, puffCls, ParsePoint(puffAt), 100.f) == 1, "4: one copy of it here");
    }
    Mark("puffball_seen");

    // 5. The host's spores' plants stand here.
    std::string hostPlants;
    if (!AwaitText("host_plants", hostPlants)) return;
    const std::set<std::string> want = Split(hostPlants);
    if (!want.empty()) {
        std::set<std::string> have;
        const bool all = WaitFor("the host's mushroom plants here", 20, [&] {
            have = ShroomPlants();
            for (const std::string& k : want)
                if (!have.count(k)) return false;
            return true;
        }, false);
        Check(all, "5: every mushroom plant the host's spores made stands on the client (" + hostPlants + ")");
    }
    Mark("host_plants_seen");

    // 6. This peer's own explosion plants nothing here.
    if (!Await("client_throw_go")) return;
    const std::set<std::string> before = ShroomPlants();
    const int set = Explode(8, false, before);
    UE_LOGI("mushroom: the client set off %d explosion(s)", set);
    const bool hitRan = SporeHit();  // dispatched or not, its body is refused here: the check is the plant count
    if (set == 0) Skip("6: no plantable ground near the client, or no puffball explosion class");
    const std::set<std::string> after = PlantsSince(before, 10);
    Mark("client_threw");
    if (set > 0 || hitRan)
        Check(after.empty(), "6: the client's own explosions and spore hit planted nothing here (" + Join(after) + ")");
    if (!Await("host_done")) return;
    Mark("client_done");
    UE_LOGI("mushroom: DONE fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI MushroomThread(LPVOID /*arg*/) {
    if (!PS::Begin("mushroom")) return 0;
    if (IsClientRole()) RunClient();
    else                RunHost();
    return 0;
}

}  // namespace harness::autotest
