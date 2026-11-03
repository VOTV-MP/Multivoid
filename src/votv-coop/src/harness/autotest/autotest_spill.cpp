// harness/autotest/autotest_spill.cpp -- TEST-ONLY: spilled liquids are the host's and lie on both peers.
//
// A. The host stands a coffee cup on its head: the client must have the host's decal at its place, none of its own, and
//    hear the splash. B. The host wipes part of it, then the client does: each fall must reach the other's copy. The host
//    wipes it away: the client's goes. A second host spill the client wipes away: the host's goes. C. An opened beer and
//    an opened wine turned over, and a gas bottle broken, on the host: every decal each leaves must lie on the client at
//    its place, once. D. A cup in the client's hand turned over: its own decal goes, the host makes it and hears the
//    splash, the client has it back once. E. The client is killed; the host spills once more; the rejoined client must
//    have every live host spill at its place, once (VOTVCOOP_SPILL_REJOIN=1 runs the rejoined half). Gated by
//    VOTVCOOP_RUN_SPILL; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/dev/drill_hand.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"
#include "coop/world/spill_sync.h"
#include "ue_wrap/actors/grime_spill.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/grime.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace E = ue_wrap::engine;
namespace GS = ue_wrap::grime_spill;
namespace R = ue_wrap::reflection;

constexpr const wchar_t* kCupPath = L"/Game/objects/prop_food_coffeeCup.prop_food_coffeeCup_C";
constexpr const wchar_t* kBeerPath = L"/Game/objects/prop_snack_beer.prop_snack_beer_C";
constexpr const wchar_t* kWinePath = L"/Game/objects/prop_snack_wine.prop_snack_wine_C";
constexpr const wchar_t* kGasPath = L"/Game/objects/prop_gasBottle.prop_gasBottle_C";
constexpr const wchar_t* kCoffee = L"grime_coffee_C";
const wchar_t* const kClasses[] = {L"grime_coffee_C", L"grime_beer_C", L"grime_wine_C", L"grime_gasoline_C"};

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
        if (f) {
            if (body) body->assign(std::istreambuf_iterator<char>(f), {});
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("spill-drill: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void Check(bool ok, const char* what) {
    UE_LOGI("spill-drill: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

bool WaitFor(const std::function<bool()>& pred, int seconds = 10) {
    for (int i = 0; i < seconds * 10; ++i) {
        bool ok = false;
        OnGameThread([&] { ok = pred(); });
        if (ok) return true;
        ::Sleep(100);
    }
    return false;
}

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }

std::vector<void*> Decals(const wchar_t* cls) {
    std::vector<void*> out;
    GS::EnsureResolved();
    void* c = ue_wrap::object_index::ClassByName(cls);
    if (!c) return out;
    ue_wrap::object_index::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
        if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) static_cast<std::vector<void*>*>(ctx)->push_back(obj);
    }, &out);
    return out;
}

std::vector<void*> AllDecals() {
    std::vector<void*> out;
    for (const wchar_t* c : kClasses)
        for (void* d : Decals(c)) out.push_back(d);
    return out;
}

std::vector<void*> NewDecals(const std::vector<void*>& before, const wchar_t* cls) {
    std::vector<void*> out;
    for (void* d : Decals(cls)) {
        bool old = false;
        for (void* b : before) old = old || b == d;
        if (!old) out.push_back(d);
    }
    return out;
}

size_t Near(const wchar_t* cls, const ue_wrap::FVector& at, float cm) {
    size_t n = 0;
    for (void* d : Decals(cls)) {
        ue_wrap::FVector l{};
        if (!E::TryGetActorLocation(d, l)) continue;
        const float dx = l.X - at.X, dy = l.Y - at.Y, dz = l.Z - at.Z;
        if (dx * dx + dy * dy + dz * dz <= cm * cm) ++n;
    }
    return n;
}

// "class x y z" lines, one per decal.
std::string Rows(const std::vector<void*>& decals) {
    std::string out;
    for (void* d : decals) {
        ue_wrap::FVector l{};
        if (!E::TryGetActorLocation(d, l)) continue;
        char b[160];
        std::snprintf(b, sizeof(b), "%s %.3f %.3f %.3f\n", Narrow(R::ClassNameOf(d)).c_str(), l.X, l.Y, l.Z);
        out += b;
    }
    return out;
}

struct Row {
    std::wstring cls;
    ue_wrap::FVector at{};
};
std::vector<Row> ParseRows(const std::string& s) {
    std::vector<Row> out;
    std::istringstream in(s);
    std::string c;
    Row r;
    while (in >> c >> r.at.X >> r.at.Y >> r.at.Z) {
        r.cls = Widen(c);
        out.push_back(r);
    }
    return out;
}


constexpr float kExact = 0.05f;   // a copy lies at the host's own floats; the rows carry them to the thousandth

// The rows at `r`'s place, `r` among them: a bottle's pours stack, so one place can hold several.
size_t Expected(const std::vector<Row>& rows, const Row& r) {
    size_t n = 0;
    for (const Row& o : rows) {
        const float dx = o.at.X - r.at.X, dy = o.at.Y - r.at.Y, dz = o.at.Z - r.at.Z;
        if (o.cls == r.cls && dx * dx + dy * dy + dz * dz <= kExact * kExact) ++n;
    }
    return n;
}

// Every row lies here at its place, as many times as the host has it there.
bool AllHere(const std::vector<Row>& rows) {
    for (const Row& r : rows)
        if (Near(r.cls.c_str(), r.at, kExact) != Expected(rows, r)) return false;
    return !rows.empty();
}

// The rows not lying here as the host has them, logged with both counts.
void LogOff(const char* what, const std::vector<Row>& rows) {
    for (const Row& r : rows) {
        const size_t n = Near(r.cls.c_str(), r.at, kExact), want = Expected(rows, r);
        if (n != want)
            UE_LOGI("spill-drill: %hs %ls at (%.3f %.3f %.3f): %zu here, %zu on the host", what, r.cls.c_str(), r.at.X, r.at.Y,
                    r.at.Z, n, want);
    }
}

// The decal's own clean by `sub`; past its dirt it ends the decal.
bool Clean(void* decal, float sub) {
    void* fn = decal ? R::FindDispatchFunctionCached(R::ClassOf(decal), L"clean") : nullptr;
    ue_wrap::ParamFrame f(fn);
    return fn && f.valid() && f.Set<void*>(L"sponge", nullptr) && f.Set(L"sub", sub) && f.Set(L"noSound", true) &&
           ue_wrap::Call(decal, f);
}

float Process(void* decal) {
    float p = -1.f;
    ue_wrap::grime::ReadProcess(decal, p);
    return p;
}

void* Me() { return coop::players::Registry::Get().Local(); }

// A prop of `path` on its head `side` cm beside the point 150 cm before the host. `open` runs a bottle's openBottle.
void* StandOnHead(const wchar_t* path, float side, bool open) {
    void* made = nullptr;
    OnGameThread([&] {
        void* cls = ue_wrap::asset_load::LoadObjectByPath(path);
        ue_wrap::FVector at{};
        if (!Me() || !cls || !E::TryGetActorLocation(Me(), at)) return;
        made = E::SpawnActor(cls, {at.X + 150.f, at.Y + side, at.Z + 20.f});
        if (!made) return;
        if (open) {
            void* fn = R::FindDispatchFunctionCached(R::ClassOf(made), L"openBottle");
            ue_wrap::ParamFrame f(fn);
            if (fn && f.valid()) ue_wrap::Call(made, f);
        }
        E::SetActorRotation(made, {0.f, 0.f, 180.f});
    });
    return made;
}

// The coffee spill of a cup on its head, or null.
void* CoffeeSpill(float side) {
    std::vector<void*> before;
    OnGameThread([&] { before = Decals(kCoffee); });
    Check(StandOnHead(kCupPath, side, false) != nullptr, "a cup stands on its head");
    void* spill = nullptr;
    Check(WaitFor([&] {
              const auto n = NewDecals(before, kCoffee);
              spill = n.empty() ? nullptr : n.front();
              return spill != nullptr;
          }, 15),
          "the host's cup spilled");
    return spill;
}

// Every decal of `cls` a bottle leaves while it pours: until none has landed for 3 s, at most 20 s.
std::vector<void*> Pour(const wchar_t* path, const wchar_t* cls, float side, bool open, bool breakIt, const char* what) {
    std::vector<void*> before, got;
    OnGameThread([&] { before = Decals(cls); });
    void* bottle = StandOnHead(path, side, open);
    Check(bottle != nullptr, what);
    if (bottle && breakIt) {
        OnGameThread([&] {
            void* fn = R::FindDispatchFunctionCached(R::ClassOf(bottle), L"broken");
            ue_wrap::ParamFrame f(fn);
            if (fn && f.valid()) ue_wrap::Call(bottle, f);
        });
    }
    size_t last = 0;
    int quiet = 0;
    for (int i = 0; i < 200 && quiet < 30; ++i) {
        OnGameThread([&] { got = NewDecals(before, cls); });
        quiet = got.size() == last && !got.empty() ? quiet + 1 : 0;
        last = got.size();
        ::Sleep(100);
    }
    UE_LOGI("spill-drill: %hs left %zu decals", what, got.size());
    return got;
}

void RunHost() {
    if (!Await("client_ready", nullptr, 480)) return;
    // A.
    void* spill = CoffeeSpill(0.f);
    if (!spill) return;
    ue_wrap::FVector at{};
    OnGameThread([&] { E::TryGetActorLocation(spill, at); });
    Mark("a_spill", Rows({spill}));
    if (!Await("a_seen", nullptr, 30)) return;
    // B. The grime lane indexes a decal on its scan; give it the time.
    ::Sleep(10000);
    float mine = -1.f;
    OnGameThread([&] {
        const float was = Process(spill);
        Clean(spill, 10.f);
        mine = Process(spill);
        UE_LOGI("spill-drill: the host wiped part of its spill: %.2f -> %.2f", was, mine);
    });
    Check(mine >= 0.f, "the host wiped part of its spill");
    char v[32];
    std::snprintf(v, sizeof(v), "%.4f", mine);
    Mark("b_host_part", v);
    std::string theirs;
    if (!Await("b_client_part", &theirs, 40)) return;
    const float c = std::stof(theirs);
    Check(WaitFor([&] { return Process(spill) <= c + 0.05f; }, 15), "the client's partial wipe reached the host's copy");
    OnGameThread([&] { UE_LOGI("spill-drill: the host's copy now %.2f (client %.2f)", Process(spill), c); });
    bool wiped = false;
    OnGameThread([&] { wiped = Clean(spill, 100000.f); });
    Check(wiped && WaitFor([&] { return Near(kCoffee, at, 2.f) == 0; }, 5), "the host wiped its spill away");
    Mark("b_wiped");
    if (!Await("b_wiped_seen", nullptr, 30)) return;
    void* second = CoffeeSpill(-120.f);
    if (!second) return;
    ue_wrap::FVector at2{};
    OnGameThread([&] { E::TryGetActorLocation(second, at2); });
    Mark("b_spill2", Rows({second}));
    if (!Await("b_client_wiped", nullptr, 30)) return;
    Check(WaitFor([&] { return Near(kCoffee, at2, 2.f) == 0; }, 10), "the client's wipe took the host's spill here");
    // C.
    const uint32_t sent0 = coop::spill_sync::PoursSent();
    const auto beer = Pour(kBeerPath, L"grime_beer_C", 120.f, true, false, "an opened beer on its head");
    Check(!beer.empty(), "the beer spilled");
    const auto wine = Pour(kWinePath, L"grime_wine_C", 240.f, true, false, "an opened wine on its head");
    Check(!wine.empty(), "the wine spilled");
    const auto gas = Pour(kGasPath, L"grime_gasoline_C", -240.f, false, true, "a gas bottle broken");
    Check(!gas.empty(), "the gas bottle spilled");
    std::string rows;
    OnGameThread([&] { rows = Rows(beer) + Rows(wine) + Rows(gas); });
    Mark("c_pours", std::to_string(coop::spill_sync::PoursSent() - sent0));
    UE_LOGI("spill-drill: the host sent %u pours", coop::spill_sync::PoursSent() - sent0);
    Mark("c_rows", rows);
    OnGameThread([&] { LogOff("host", ParseRows(rows)); });
    if (!Await("c_seen", nullptr, 60)) return;
    // D.
    const uint32_t heard = coop::spill_sync::SplashesPlayed();
    std::string where;
    if (!Await("d_spill", &where, 60)) return;
    const auto drows = ParseRows(where);
    Check(WaitFor([&] { return AllHere(drows); }, 10), "the client's spill is made here, once");
    OnGameThread([&] { LogOff("host", drows); });
    Check(coop::spill_sync::SplashesPlayed() > heard, "the host heard the client's splash");
    const uint32_t poured0 = coop::spill_sync::PoursHeard();
    Mark("d_made");
    std::string cp;
    if (!Await("d2_pours", &cp, 60)) return;
    const uint32_t clientPours = static_cast<uint32_t>(std::stoul(cp));
    Check(clientPours > 0 && WaitFor([&] { return coop::spill_sync::PoursHeard() - poured0 == clientPours; }, 10),
          "every pour of the client's held beer was heard here");
    UE_LOGI("spill-drill: the host heard %u of the client's %u pours", coop::spill_sync::PoursHeard() - poured0, clientPours);
    Mark("d2_heard");
    // E.
    if (!Await("client_killed", nullptr, 300)) return;
    CoffeeSpill(60.f);
    std::string live;
    OnGameThread([&] { live = Rows(AllDecals()); });
    Mark("e_rows", live);
    UE_LOGI("spill-drill: %zu spills live for the rejoin", ParseRows(live).size());
    Await("rejoin_done", nullptr, 400);
    OnGameThread([&] {
        for (void* d : AllDecals()) Clean(d, 100000.f);
    });
    UE_LOGI("spill-drill: DONE on host fails=%d", g_fails);
}

bool WorldReady() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = Me() && coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    return ready;
}

void RunClient() {
    WorldReady();
    ::Sleep(8000);
    Mark("client_ready");
    // A.
    const uint32_t heard = coop::spill_sync::SplashesPlayed();
    std::string a;
    if (!Await("a_spill", &a, 120)) return;
    const auto arows = ParseRows(a);
    if (arows.empty()) return;
    const ue_wrap::FVector h = arows[0].at;
    Check(WaitFor([&] { return AllHere(arows); }, 10), "the host's spill lies here, at its place");
    Check(coop::spill_sync::SplashesPlayed() > heard, "the host's splash was heard here");
    ::Sleep(1500);
    OnGameThread([&] { Check(Near(kCoffee, h, 400.f) == 1, "no spill of this copy's own beside it"); });
    Mark("a_seen");
    // B.
    std::string hp;
    if (!Await("b_host_part", &hp, 60)) return;
    const float hostPart = std::stof(hp);
    void* copy = nullptr;
    Check(WaitFor([&] {
              copy = GS::FindSpill(kCoffee, h, 2.f);
              return copy && Process(copy) <= hostPart + 0.05f;
          }, 15),
          "the host's partial wipe reached this copy");
    float mine = -1.f;
    OnGameThread([&] {
        UE_LOGI("spill-drill: this copy after the host's wipe: %.2f (host %.2f)", Process(copy), hostPart);
        Clean(copy, 10.f);
        mine = Process(copy);
    });
    Check(mine >= 0.f && mine < hostPart, "this player wiped part of the spill");
    char v[32];
    std::snprintf(v, sizeof(v), "%.4f", mine);
    Mark("b_client_part", v);
    if (!Await("b_wiped", nullptr, 40)) return;
    Check(WaitFor([&] { return Near(kCoffee, h, 2.f) == 0; }, 10), "the host's wipe took the spill here");
    Mark("b_wiped_seen");
    std::string b2;
    if (!Await("b_spill2", &b2, 60)) return;
    const auto b2rows = ParseRows(b2);
    if (b2rows.empty()) return;
    bool wiped = false;
    Check(WaitFor([&] { return AllHere(b2rows); }, 10), "the host's second spill lies here");
    OnGameThread([&] { wiped = Clean(GS::FindSpill(kCoffee, b2rows[0].at, 2.f), 100000.f); });
    Check(wiped, "this player wiped the host's spill away");
    Mark("b_client_wiped");
    // C.
    const uint32_t heard0 = coop::spill_sync::PoursHeard();
    std::string c;
    if (!Await("c_rows", &c, 120)) return;
    std::string hp2;
    if (Await("c_pours", &hp2, 5)) {
        const uint32_t hostPours = static_cast<uint32_t>(std::stoul(hp2));
        Check(hostPours > 0 && WaitFor([&] { return coop::spill_sync::PoursHeard() - heard0 == hostPours; }, 10),
              "every pour of the host's bottles was heard here");
        UE_LOGI("spill-drill: heard %u of the host's %u pours", coop::spill_sync::PoursHeard() - heard0, hostPours);
    }
    const auto crows = ParseRows(c);
    if (crows.empty()) return;
    Check(WaitFor([&] { return AllHere(crows); }, 10), "every beer, wine and gas decal lies here at its place");
    OnGameThread([&] {
        size_t n[4] = {};
        for (const Row& r : crows)
            for (int i = 0; i < 4; ++i)
                if (r.cls == kClasses[i]) ++n[i];
        UE_LOGI("spill-drill: decals checked here: beer %zu, wine %zu, gas %zu", n[1], n[2], n[3]);
        LogOff("client", crows);
        size_t extra = 0, want = 0;   // within 4 m of the pours: the save holds beer decals elsewhere
        for (int i = 1; i < 4; ++i) extra += Near(kClasses[i], crows.front().at, 400.f);
        for (const Row& r : crows) {
            const float dx = r.at.X - crows.front().at.X, dy = r.at.Y - crows.front().at.Y, dz = r.at.Z - crows.front().at.Z;
            want += dx * dx + dy * dy + dz * dz <= 400.f * 400.f;
        }
        Check(extra == want, "no beer, wine or gas decal of this copy's own");
    });
    Mark("c_seen");
    // D.
    std::vector<void*> before;
    void* cup = nullptr;
    OnGameThread([&] {
        before = Decals(kCoffee);
        ue_wrap::asset_load::LoadObjectByPath(kCupPath);
        coop::dev::drill_hand::TakeIntoHandNow(L"coffee_b", L"prop_food_coffeeCup_C");   // the cup's row
    });
    Check(WaitFor([&] {
              E::MainPlayerGrabState gs{};
              cup = Me() && E::ReadMainPlayerGrabState(Me(), gs) ? gs.holdingActor : nullptr;
              return cup && R::ClassNameOf(cup) == L"prop_food_coffeeCup_C";
          }, 5),
          "a cup is in this player's hand");
    if (!cup) return;
    bool gone = false;
    for (int i = 0; i < 60 && !gone; ++i) {
        OnGameThread([&] {
            if (R::IsLive(cup)) E::SetActorRotation(cup, {0.f, 0.f, 180.f});
            else gone = true;
        });
        ::Sleep(50);
    }
    Check(gone, "the cup in the hand spilled");
    std::vector<void*> back;
    Check(WaitFor([&] { return !(back = NewDecals(before, kCoffee)).empty(); }, 10), "the host's spill came back here");
    std::string drow;
    OnGameThread([&] { drow = Rows({back.front()}); });
    Mark("d_spill", drow);
    if (!Await("d_made", nullptr, 30)) return;
    OnGameThread([&] { Check(back.size() == 1, "one spill here, the host's"); });
    // D2. A beer in this player's hand, opened and turned over: its pours go to the host to be heard.
    void* beer = nullptr;
    for (const wchar_t* row : {L"beer"}) {   // the beer's row in the props table
        OnGameThread([&] {
            ue_wrap::asset_load::LoadObjectByPath(kBeerPath);
            coop::dev::drill_hand::TakeIntoHandNow(row, L"prop_snack_beer_C");
        });
        const bool in = WaitFor([&] {
            E::MainPlayerGrabState gs{};
            beer = Me() && E::ReadMainPlayerGrabState(Me(), gs) ? gs.holdingActor : nullptr;
            return beer && R::ClassNameOf(beer) == L"prop_snack_beer_C";
        }, 2);
        UE_LOGI("spill-drill: the hand row '%ls' %hs", row, in ? "holds a beer" : "holds none");
        if (in) break;
        beer = nullptr;
    }
    Check(beer != nullptr, "a beer is in this player's hand");
    const uint32_t sent0 = coop::spill_sync::PoursSent();
    if (beer) {
        OnGameThread([&] {
            void* fn = R::FindDispatchFunctionCached(R::ClassOf(beer), L"openBottle");
            ue_wrap::ParamFrame f(fn);
            if (fn && f.valid()) ue_wrap::Call(beer, f);
        });
        for (int i = 0; i < 40; ++i) {   // 2 s turned over in the hand
            OnGameThread([&] {
                if (R::IsLive(beer)) E::SetActorRotation(beer, {0.f, 0.f, 180.f});
            });
            ::Sleep(50);
        }
        ::Sleep(500);
    }
    const uint32_t poured = coop::spill_sync::PoursSent() - sent0;
    Check(poured > 0, "the beer in the hand poured");
    UE_LOGI("spill-drill: this player's beer poured %u times", poured);
    OnGameThread([] { coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr); });
    Mark("d2_pours", std::to_string(poured));
    Await("d2_heard", nullptr, 30);
    Mark("client_done");
    UE_LOGI("spill-drill: DONE on client fails=%d", g_fails);
}

void RunRejoin() {
    WorldReady();
    std::string e;
    if (!Await("e_rows", &e, 120)) return;
    const auto rows = ParseRows(e);
    ::Sleep(5000);
    Check(WaitFor([&] { return AllHere(rows); }, 20), "every live host spill lies here after the rejoin, none twice");
    OnGameThread([&] {
        size_t missing = 0, doubled = 0;
        for (const Row& r : rows) {
            const size_t n = Near(r.cls.c_str(), r.at, kExact), want = Expected(rows, r);
            missing += n < want;
            doubled += n > want;
        }
        UE_LOGI("spill-drill: rejoin checked %zu spills: %zu missing, %zu doubled", rows.size(), missing, doubled);
        LogOff("rejoin", rows);
    });
    Mark("rejoin_done");
    UE_LOGI("spill-drill: REJOIN DONE fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI SpillThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("spill-drill: VERDICT ABORT -- VOTVCOOP_CAN_DIR is not set");
        return 0;
    }
    if (!IsClientRole()) RunHost();
    else if (coop::config::ReadEnv("VOTVCOOP_SPILL_REJOIN") == "1") RunRejoin();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest
