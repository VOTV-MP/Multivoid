// harness/autotest/autotest_woodchipper.cpp -- TEST-ONLY: the woodchipper lane across two peers (woodchipper_drill).
//
// The host takes the world's woodchipper nearest to it by route, or spawns one beside itself; the client's player is
// walked to the host by the director (harness/autotest/meet), puts a gas can into its hand the way a player's pick does,
// and then acts at the machine. Each arm is judged on the HOST from what both peers read, each wait a state with a bound:

#include "harness/autotest.h"   // IsClientRole
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/routes.h"
#include "coop/dev/drill_hand.h"
#include "coop/interactables/woodchipper_gate.h"
#include "coop/interactables/woodchipper_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/woodchipper.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

//   g1  the client pours: the host's fuel rises by what the client's copy gained, and the client's copy shows it.
//   g2  the client presses the button: the host's chipper is on and burning, the client's copy shows both, and the gate
//       refused the copy's own tick (the count is the evidence).
//   g3  its own arm, woodchipper_drill=feed (after g1 and g2): a paper dropped into the hopper. Its key leaves the host,
//       a clump not in the baseline taken before the drop appears, the client shows that key gone and that clump
//       present: by identity, never a count. INVALID when the chipper is not running or never eats the paper.
//   g5  an over-pour: the host takes only what fits. INVALID when the tank holds 20 or less before it.
//   g6  a pour from a client whose hand holds no gas can: the host refuses it.
//   g4  with the rig's --rejoin: the chipper is on; the client leaves and comes back, and its copy must read it on.
// Red arms: g2 woodchipper_gate_off and woodchipper_intent_off; g5, g6 woodchipper_pour_check_off; g4
// woodchipper_replay_off. Tags, said by the host: `woodchipper` (g1, g2, g5, g6; the arm `1`), `woodchipper-rejoin` (g4)
// and `woodchipper-feed` (g3); the lines are tagged `woodchipper:`. The second life carries VOTVCOOP_WOODCHIPPER_DRILL_REJOIN=1.

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace W  = ue_wrap::woodchipper;
namespace PS = peer_signal;
namespace RV = coop::rig_verdict;

constexpr const char* kTag = "woodchipper";
constexpr const char* kTagRejoin = "woodchipper-rejoin";
constexpr const char* kTagFeed = "woodchipper-feed";
constexpr const wchar_t* kChipperPath = L"/Game/objects/woodchipper.woodchipper_C";
constexpr const wchar_t* kPropPath = L"/Game/objects/prop_paper.prop_paper_C";   // wood, bag, paper...: canChip's list
constexpr const wchar_t* kCanClass = L"prop_gascan_C";
constexpr int kJoinSec = 480;
constexpr int kSignalSec = 240;
constexpr int kCopySec = 40;    // a state reaches a copy: the send, the round trip, the apply
constexpr int kBurnSec = 90;    // the spin-up, then fuel falling by a measurable amount
constexpr int kEatSec = 60;     // the drop, the chip animation, the clump (and a census scan to show it to the client)
constexpr float kFuelTol = 0.6f;
constexpr float kBurnDrop = 0.2f;
constexpr float kChipperRouteCm = 400.f;   // a world chipper this near the host by route is the drill's; else one is spawned
constexpr float kChipperStandCm = 150.f;   // a route's end this near the chipper is a place its use reaches
constexpr float kOverPourSpare = 20.f;     // g5 pours 80: a tank holding more than this has no room for all of it
constexpr float kCanFuel = 100.f;          // the full tank the drill's can loads with; a hold record without it loads empty

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string Narrow(const std::wstring& w) {
    std::string s;
    for (const wchar_t c : w) s.push_back(static_cast<char>(c));   // a prop key is ASCII
    return s;
}

// One reading of a chipper's copy, as signalled.
struct Reading {
    float fuel = -1.f;
    int active = -1;
};

void* ChipperOf(const std::wstring& key) {
    void* c = coop::prop_key_index::Find(key);
    return W::IsChipper(c) ? c : nullptr;
}

Reading ReadCopy(const std::wstring& key) {
    Reading r;
    float fuel = 0.f;
    bool active = false;
    if (W::Read(ChipperOf(key), fuel, active)) {
        r.fuel = fuel;
        r.active = active ? 1 : 0;
    }
    return r;
}

std::string Text(const Reading& r) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "fuel=%.2f active=%d", static_cast<double>(r.fuel), r.active);
    return buf;
}

double FieldOf(const std::string& text, const char* name, double none = -1.0) {
    const size_t at = text.find(name);
    return at == std::string::npos ? none : std::strtod(text.c_str() + at + std::strlen(name), nullptr);
}

Reading Parse(const std::string& text) {
    Reading r;
    r.fuel = static_cast<float>(FieldOf(text, "fuel="));
    r.active = static_cast<int>(FieldOf(text, "active="));
    return r;
}

Reading Now(const std::wstring& key) {
    Reading r;
    OnGameThread([&] { r = ReadCopy(key); });
    return r;
}

using View = coop::woodchipper_sync::DrillView;

View ViewNow() {
    View v;
    OnGameThread([&] { v = coop::woodchipper_sync::ReadForDrill(); });
    return v;
}

// What the host's arms add up to: the arms that failed, and the reason an arm could not be measured.
struct Judge {
    std::string failed;
    std::string invalid;
};

void Arm(Judge& j, const char* arm, bool ok, const std::string& what) {
    UE_LOGI("woodchipper: ARM %s %s -- %s", arm, ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) j.failed += std::string(j.failed.empty() ? "" : ", ") + arm;
}

void Finish(const Judge& j) {
    if (!j.failed.empty()) RV::Say(kTag, RV::Outcome::Fail, "%s", j.failed.c_str());
    else if (!j.invalid.empty()) RV::Say(kTag, RV::Outcome::Invalid, "%s", j.invalid.c_str());
    else RV::Say(kTag, RV::Outcome::Pass);
}

// The announced world and the host's puppet exist here.
bool HostPuppetUp() {
    if (!PS::WaitFor("the announced world", kJoinSec, [] {
            bool ready = false;
            OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
            return ready;
        }))
        return false;
    return PS::WaitFor("the host's puppet", 60, [] {
        bool have = false;
        OnGameThread([&] {
            coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
            have = p && p->valid();
        });
        return have;
    });
}

bool CopyResolves(const std::wstring& key) {
    return PS::WaitFor("the chipper the host named", 120, [&] {
        void* c = nullptr;
        OnGameThread([&] { c = ChipperOf(key); });
        return c != nullptr;
    });
}

// ---- HOST ------------------------------------------------------------------------------------------------

// The world's chipper nearest the host's player by NavMesh route, among those a route from it reaches within
// kChipperRouteCm. The client stands beside the host's puppet (meet::WalkToHost), so the same chipper is within its reach;
// a chipper behind a wall, or across the base, is not chosen, and a drill that took it would read the host's reach refusal
// as a failed pour.
void* NearestChipper(void* me) {
    ue_wrap::FVector at{};
    if (!me || !E::TryGetActorLocation(me, at)) return nullptr;
    void* best = nullptr;
    float bestLen = kChipperRouteCm;
    for (void* o : R::FindObjectsByClass(L"woodchipper_C")) {
        ue_wrap::FVector pos{};
        float len = 0.f;
        if (!W::IsChipper(o) || W::KeyOf(o).empty() || !E::TryGetActorLocation(o, pos)) continue;
        if (!coop::director::RouteInLegs(me, at, pos, kChipperStandCm, &len) || len > bestLen) continue;
        best = o;
        bestLen = len;
    }
    return best;
}

// The world's chipper nearest the host, or one spawned in front of the host's player; its key.
std::wstring ChipperKey() {
    std::wstring key;
    void* chosen = nullptr;
    bool spawned = false;
    OnGameThread([&] {
        void* c = NearestChipper(coop::players::Registry::Get().Local());
        if (!c) {
            void* me = coop::players::Registry::Get().Local();
            ue_wrap::FVector at{};
            void* cls = ue_wrap::asset_load::LoadObjectByPath(kChipperPath);
            if (me && cls && E::TryGetActorLocation(me, at)) {
                const ue_wrap::FVector fwd = E::GetActorForwardVector(me);
                c = E::SpawnActor(cls, {at.X + fwd.X * 250.f, at.Y + fwd.Y * 250.f, at.Z + 60.f});
                spawned = c != nullptr;
            }
        }
        chosen = c;
        key = W::KeyOf(c);
    });
    for (int i = 0; i < 100 && key.empty() && chosen; ++i) {
        ::Sleep(100);
        OnGameThread([&] { key = W::KeyOf(chosen); });
    }
    UE_LOGI("woodchipper: HOST chipper key='%ls' (%s)", key.c_str(), spawned ? "spawned" : "the world's own");
    return key;
}

bool FeedArm() { return coop::config::ResolveEnum(coop::config_registry::rows::woodchipper_drill) == "feed"; }

// ---- the feed arm: identities, never counts ----
// The pile eid 3136 near the hopper flips between pile and clump about once a second, so a count of clumps moves
// without the chipper doing anything. A clump is therefore told by its object and its key against a baseline taken
// before the drop, and the paper by its key.

struct Clump {
    void* object;   // an identity compared, never dereferenced after the walk
    std::wstring key;
};

void CollectInstance(void* ctx, void* obj, int32_t) { static_cast<std::vector<void*>*>(ctx)->push_back(obj); }

// Every clump of the world now, with the key it answers to (empty and "None" ones left out: they cannot be named to the
// other peer). Game thread. The keys are asked after the walk, since a key may dispatch a function.
std::vector<Clump> ClumpsNow() {
    std::vector<void*> all;
    void* cls = ue_wrap::object_index::ClassByName(L"prop_garbageClump_C");
    if (cls) ue_wrap::object_index::ForEachInstance(cls, &CollectInstance, &all);
    std::vector<Clump> out;
    for (void* o : all) {
        std::wstring k = ue_wrap::prop::GetInteractableKeyString(o);
        if (!k.empty() && k != L"None") out.push_back({o, std::move(k)});
    }
    return out;
}

struct Baseline {
    std::vector<void*> objects;
    std::set<std::wstring> keys;
};

Baseline TakeBaseline() {
    Baseline b;
    OnGameThread([&] {
        for (const Clump& c : ClumpsNow()) {
            b.objects.push_back(c.object);
            b.keys.insert(c.key);
        }
    });
    return b;
}

// A clump neither in the baseline by object nor by key; its key in `out`.
bool NewClumpAgainst(const Baseline& b, std::wstring& out) {
    bool found = false;
    OnGameThread([&] {
        for (const Clump& c : ClumpsNow()) {
            if (std::find(b.objects.begin(), b.objects.end(), c.object) != b.objects.end() || b.keys.count(c.key)) continue;
            out = c.key;
            found = true;
            return;
        }
    });
    return found;
}

enum class Seen { Yes, No, Unknown };

// Whether an actor of the current world holds `key` (and is of class `cls` when one is named). Unknown while the key
// index cannot answer yet.
Seen KeySeen(const std::wstring& key, const wchar_t* cls) {
    Seen s = Seen::Unknown;
    OnGameThread([&] {
        void* a = nullptr;
        const coop::prop_key_index::Read r = coop::prop_key_index::Resolve(key, &a);
        if (r == coop::prop_key_index::Read::Miss) s = Seen::No;
        else if (r == coop::prop_key_index::Read::Hit) s = !cls || R::ClassNameOf(a) == cls ? Seen::Yes : Seen::No;
    });
    return s;
}

// A paper prop released over the hopper: the host simulates it, as it does a prop a player let go of there. Its key in
// `paperKey` (left empty when the paper holds none within a bound); false when it could not be spawned.
bool DropPropOverHopper(const std::wstring& key, std::wstring& paperKey) {
    ue_wrap::CachedObjRef paper;
    OnGameThread([&] {
        void* chipper = ChipperOf(key);
        static R::InstanceOffset sEat{L"eat"};
        const int32_t off = chipper ? sEat.Of(chipper) : -1;
        void* eat = nullptr;
        if (off >= 0) std::memcpy(&eat, static_cast<const uint8_t*>(chipper) + off, sizeof(eat));
        ue_wrap::asset_load::LoadObjectByPath(kPropPath);
        void* cls = R::FindClass(L"prop_paper_C");
        if (!eat || !cls) return;
        ue_wrap::FVector at = E::GetComponentLocation(eat);
        at.Z += 120.f;
        paper.Set(E::SpawnActor(cls, at));
    });
    if (!paper.Raw()) return false;
    PS::WaitFor("the dropped paper's key", 10, [&] {
        OnGameThread([&] {
            if (void* p = paper.Get()) {
                std::wstring k = ue_wrap::prop::GetKeyString(p);
                if (!k.empty() && k != L"None") paperKey = std::move(k);
            }
        });
        return !paperKey.empty();
    }, false);
    return true;
}

// The host's half of the feed arm, verdict 'woodchipper-feed'. PASS: the dropped paper is gone from the host and from
// the client, and a clump new against the baseline is on both. INVALID: the chipper was not running, or never ate the
// paper (nothing was measured). FAIL: it ate the paper and the change did not cross, or no clump followed.
void HostFeed(const std::wstring& key) {
    const Reading running = Now(key);
    const Baseline base = TakeBaseline();
    if (running.active != 1 || !(running.fuel > 0.f)) {
        PS::Mark("g3_go", "none");
        RV::Say(kTagFeed, RV::Outcome::Invalid, "the chipper was not running when the paper was dropped (%s)", Text(running).c_str());
        return;
    }
    std::wstring paperKey;
    if (!DropPropOverHopper(key, paperKey) || paperKey.empty()) {
        PS::Mark("g3_go", "none");
        RV::Say(kTagFeed, RV::Outcome::Invalid, "the paper could not be spawned over the hopper with a key");
        return;
    }
    UE_LOGI("woodchipper: FEED the host dropped prop_paper_C '%ls' over chipper '%ls' (%zu clumps in the baseline)",
            paperKey.c_str(), key.c_str(), base.objects.size());
    PS::Mark("g3_go", "paper=" + Narrow(paperKey));
    if (!PS::WaitFor("the host's chipper eats the dropped paper", kEatSec, [&] { return KeySeen(paperKey, nullptr) == Seen::No; }, false)) {
        PS::Mark("g3_host", "none");
        RV::Say(kTagFeed, RV::Outcome::Invalid, "the host's chipper did not eat the dropped prop_paper_C %s in %d s",
                Narrow(paperKey).c_str(), kEatSec);
        return;
    }
    std::wstring clumpKey;
    PS::WaitFor("a clump new against the baseline", kEatSec, [&] { return NewClumpAgainst(base, clumpKey); }, false);
    if (clumpKey.empty()) {
        PS::Mark("g3_host", "none");
        RV::Say(kTagFeed, RV::Outcome::Fail, "the host's chipper ate the paper %s, and no clump new against the baseline of %zu appeared in %d s",
                Narrow(paperKey).c_str(), base.objects.size(), kEatSec);
        return;
    }
    PS::Mark("g3_host", "clump=" + Narrow(clumpKey));
    std::string shown;
    if (!PS::AwaitText("g3_client", shown, kSignalSec)) {
        RV::Say(kTagFeed, RV::Outcome::Invalid, "the client never reported what it saw of the paper and the clump");
        return;
    }
    const bool crossed = shown == "paper=gone clump=present";
    UE_LOGI("woodchipper: ARM g3 %s -- the host ate paper %ls and made clump %ls; the client reports '%s'", crossed ? "PASS" : "FAIL",
            paperKey.c_str(), clumpKey.c_str(), shown.c_str());
    RV::Say(kTagFeed, crossed ? RV::Outcome::Pass : RV::Outcome::Fail, "the host ate paper %s and made clump %s; the client: %s",
            Narrow(paperKey).c_str(), Narrow(clumpKey).c_str(), shown.c_str());
}

// The client's half: it waits for what the host names, then reads whether the paper is gone and the clump is here.
void ClientFeed() {
    std::string go;
    if (!PS::AwaitText("g3_go", go, kSignalSec)) return;
    if (go == "none") {
        UE_LOGI("woodchipper: CLIENT no feed (the host could not drop a paper)");
        return;
    }
    std::string host;
    if (!PS::AwaitText("g3_host", host, kSignalSec)) return;
    if (host == "none") {
        UE_LOGI("woodchipper: CLIENT no clump to look for (the host reported none)");
        return;
    }
    const std::wstring paperKey = Widen(go.substr(go.find('=') + 1));
    const std::wstring clumpKey = Widen(host.substr(host.find('=') + 1));
    PS::WaitFor("the paper gone and the clump here", kEatSec, [&] {
        return KeySeen(paperKey, nullptr) == Seen::No && KeySeen(clumpKey, L"prop_garbageClump_C") == Seen::Yes;
    }, false);
    const Seen paper = KeySeen(paperKey, nullptr);
    const Seen clump = KeySeen(clumpKey, L"prop_garbageClump_C");
    const std::string text = std::string("paper=") + (paper == Seen::No ? "gone" : paper == Seen::Yes ? "here" : "unknown") +
                             " clump=" + (clump == Seen::Yes ? "present" : clump == Seen::No ? "absent" : "unknown");
    UE_LOGI("woodchipper: CLIENT feed: %s", text.c_str());
    PS::Mark("g3_client", text);
}

// Everything the host judges is signalled by the client in a line of readings; the host's own reading is taken when the
// client's arrives.
void RunHost() {
    const bool feed = FeedArm();
    Judge j;
    if (!PS::Await("client_arrived", kJoinSec)) return;
    const std::wstring key = ChipperKey();
    if (key.empty()) {
        UE_LOGW("woodchipper: VERDICT ABORT -- no woodchipper in the world and none spawned");
        return;
    }
    PS::Mark("chipper_key", Narrow(key));
    if (!PS::Await("can_in_hand", kSignalSec)) return;
    if (!PS::WaitFor("the client's gas can in hand here", 30, [&] {
            bool held = false;
            OnGameThread([&] { held = coop::hand_item::HeldClassIs(1, kCanClass); });
            return held;
        }))
        return;
    UE_LOGI("woodchipper: HOST sees the client's hand hold a gas can");

    // g1. The client pours.
    const Reading f1 = Now(key);
    PS::Mark("g1_go", Text(f1));
    std::string poured;
    if (!PS::AwaitText("g1_poured", poured, kSignalSec)) return;
    const float gained = static_cast<float>(FieldOf(poured, "gained="));
    PS::WaitFor("the host's fuel after the pour", kCopySec, [&] { return Now(key).fuel >= f1.fuel + gained - kFuelTol; }, false);
    const Reading h1 = Now(key);
    PS::Mark("g1_host", Text(h1));
    std::string shown1;
    if (!PS::AwaitText("g1_client", shown1, kSignalSec)) return;
    const Reading c1 = Parse(shown1);
    char what[200];
    std::snprintf(what, sizeof(what), "the client's copy gained %.2f; the host's fuel %.2f -> %.2f, the client's copy then %.2f",
                  static_cast<double>(gained), static_cast<double>(f1.fuel), static_cast<double>(h1.fuel),
                  static_cast<double>(c1.fuel));
    Arm(j, "g1", gained > 1.f && std::fabs(h1.fuel - (f1.fuel + gained)) <= kFuelTol && std::fabs(c1.fuel - h1.fuel) <= kFuelTol, what);

    // g2. The client presses the button.
    const Reading f2 = Now(key);
    PS::Mark("g2_go", Text(f2));
    PS::WaitFor("the host's chipper on, burning", kBurnSec, [&] {
        const Reading r = Now(key);
        return r.active == 1 && r.fuel <= f2.fuel - kBurnDrop;
    }, false);
    const Reading h2 = Now(key);
    std::string shown2;
    if (!PS::AwaitText("g2_client", shown2, kSignalSec)) return;
    const Reading c2 = Parse(shown2);
    const Reading h2b = Now(key);
    const double refused = FieldOf(shown2, "tickRefused=", 0.0);
    std::snprintf(what, sizeof(what),
                  "host %s (was %.2f), client's copy %s, the host then %.2f; the client's gate refused %.0f ticks",
                  Text(h2).c_str(), static_cast<double>(f2.fuel), Text(c2).c_str(), static_cast<double>(h2b.fuel), refused);
    Arm(j, "g2", h2.active == 1 && h2.fuel <= f2.fuel - kBurnDrop && c2.active == 1 && c2.fuel <= f2.fuel - kBurnDrop &&
                     std::fabs(c2.fuel - h2b.fuel) <= 2.f && refused >= 5.0, what);

    if (!feed) {
        // g5. An over-pour: more than the tank has room for.
        const Reading f5 = Now(key);
        const View w5 = ViewNow();
        PS::Mark("g5_go");
        PS::WaitFor("the over-pour judged", kCopySec, [&] { return ViewNow().clamped > w5.clamped || Now(key).fuel > W::kMaxFuel + 1.f; }, false);
        const Reading h5 = Now(key);
        const View v5 = ViewNow();
        std::snprintf(what, sizeof(what), "the host's fuel %.2f -> %.2f after a pour of 80, clamped %llu times", static_cast<double>(f5.fuel),
                      static_cast<double>(h5.fuel), v5.clamped - w5.clamped);
        if (f5.fuel > kOverPourSpare) {
            Arm(j, "g5", h5.fuel <= W::kMaxFuel + 0.01f && v5.clamped > w5.clamped, what);
        } else {
            // The premise: a pour of 80 overflows only a tank holding more than 20. Judging it would read a small g1 pour as a fail.
            j.invalid += std::string(j.invalid.empty() ? "" : "; ") + "g5: the tank held " + std::to_string(f5.fuel) +
                         " fuel, not above 20, so a pour of 80 has no excess to clamp";
            UE_LOGW("woodchipper: ARM g5 INVALID -- %s", j.invalid.c_str());
        }
        PS::Mark("g5_done");
    
        // g6. A pour from a client with an empty hand.
        if (!PS::Await("g6_hand_empty", kSignalSec)) return;
        PS::WaitFor("the client's hand empty here", 30, [&] {
            bool held = true;
            OnGameThread([&] { held = coop::hand_item::HeldClassIs(1, kCanClass); });
            return !held;
        }, false);
        const Reading f6 = Now(key);
        const View w6 = ViewNow();
        PS::Mark("g6_go");
        PS::WaitFor("the empty-handed pour judged", kCopySec, [&] { return ViewNow().refusedCan > w6.refusedCan || Now(key).fuel > f6.fuel + 1.f; }, false);
        const Reading h6 = Now(key);
        const View v6 = ViewNow();
        std::snprintf(what, sizeof(what), "the host's fuel %.2f -> %.2f after a pour of 5 with no can in the hand, refused for the can %llu times",
                      static_cast<double>(f6.fuel), static_cast<double>(h6.fuel), v6.refusedCan - w6.refusedCan);
        Arm(j, "g6", h6.fuel <= f6.fuel + 0.01f && v6.refusedCan > w6.refusedCan, what);
        PS::Mark("g6_done");
    }

    if (feed) {
        HostFeed(key);
        UE_LOGI("woodchipper: FEED DONE on host");
        return;
    }
    Finish(j);
    UE_LOGI("woodchipper: G1-G6 DONE fails='%s'", j.failed.c_str());

    // g4. The chipper stays on; the client leaves and comes back into it.
    const uint32_t life1Gen = harness::session_runtime::Session().peerGenerationForSlot(1);
    const Reading f4 = Now(key);
    PS::Mark("g4_on", Text(f4));
    if (!PS::Await("client_done", kSignalSec)) return;
    PS::Mark("host_ack");   // now watching slot 1: life 1 may end
    if (!PS::WaitFor("the client's life 1 gone from slot 1", 300, [life1Gen] {
            auto& s = harness::session_runtime::Session();
            return !s.IsSlotConnected(1) || s.peerGenerationForSlot(1) != life1Gen;
        }))
        return;
    if (!PS::Await("rejoin_ready", 400)) return;
    PS::Mark("e_key", Narrow(key));
    std::string again;
    if (!PS::AwaitText("rejoin_done", again, 400)) return;
    const Reading r4 = Parse(again);
    const Reading h4 = Now(key);
    std::snprintf(what, sizeof(what), "the rejoined client's copy after its world-ready: %s; the host's %s", again.c_str(), Text(h4).c_str());
    const bool ok = r4.active == 1 && std::fabs(r4.fuel - h4.fuel) <= 8.f;
    UE_LOGI("woodchipper: ARM g4 %s -- %s", ok ? "PASS" : "FAIL", what);
    RV::Say(kTagRejoin, ok ? RV::Outcome::Pass : RV::Outcome::Fail, "%s", what);
    UE_LOGI("woodchipper: DONE on host");
}

// ---- CLIENT ----------------------------------------------------------------------------------------------

bool CanInHand() {
    bool in = false;
    OnGameThread([&] {
        void* h = coop::hand_item::LocalHandActor();
        in = h && R::ClassNameOf(h) == kCanClass;
    });
    return in;
}

// The can's fuel, its `amount` property; -1 when the property does not resolve. Game thread.
float CanAmount(void* can) {
    static int32_t off = -2;
    if (off == -2) off = can ? R::FindPropertyOffset(R::ClassOf(can), L"amount") : -2;
    if (!can || off < 0) return -1.f;
    float v = -1.f;
    std::memcpy(&v, static_cast<const uint8_t*>(can) + off, sizeof(v));
    return v;
}

// The client's player pours its gas can into its copy of the chipper as the can's own use does, through the chipper's
// gascanFuel; the gate's watch reads the fuel the copy gained and asks the host. The fuel gained is returned.
float Pour(const std::wstring& key) {
    float gained = -1.f;
    OnGameThread([&] {
        void* chipper = ChipperOf(key);
        void* me = coop::players::Registry::Get().Local();
        void* can = coop::hand_item::LocalHandActor();
        void* fn = chipper ? R::FindFunction(R::ClassOf(chipper), L"gascanFuel") : nullptr;
        float before = 0.f, after = 0.f;
        bool active = false;
        if (!fn || !me || !can || !W::Read(chipper, before, active)) return;
        const float canBefore = CanAmount(can);
        ue_wrap::ParamFrame f(fn);
        if (!f.valid() || !f.Set<void*>(L"gascan", can) || !f.Set<void*>(L"player", me) || !f.Set<bool>(L"back", false) ||
            !ue_wrap::Call(chipper, f) || !W::Read(chipper, after, active))
            return;
        gained = after - before;
        UE_LOGI("woodchipper: CLIENT pour: the can's amount %.2f -> %.2f, the copy's fuel %.2f -> %.2f", static_cast<double>(canBefore),
                static_cast<double>(CanAmount(can)), static_cast<double>(before), static_cast<double>(after));
    });
    return gained;
}

// A pour no player's can made: sent as the gate sends its own.
bool SendPour(const std::wstring& key, float amount) {
    coop::net::WoodchipperPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.op = coop::net::kWoodchipperOpPour;
    p.fuel = amount;
    return harness::session_runtime::Session().SendReliable(coop::net::ReliableKind::WoodchipperIntent, &p, sizeof(p));
}

void ReportCopy(const std::wstring& key, int active, const char* signal) {
    PS::WaitFor("the copy at the host's state", kCopySec, [&] { return Now(key).active == active; }, false);
    const std::string text = Text(Now(key));
    UE_LOGI("woodchipper: CLIENT copy %s", text.c_str());
    PS::Mark(signal, text);
}

void RunClient() {
    const bool feed = FeedArm();
    if (!HostPuppetUp()) return;
    PS::Mark("client_ready");
    if (!meet::WalkToHost(kTag)) return;
    PS::Mark("client_arrived");
    std::string keyText;
    if (!PS::AwaitText("chipper_key", keyText, kSignalSec)) return;
    const std::wstring key = Widen(keyText);
    if (!CopyResolves(key)) return;
    OnGameThread([] { coop::dev::drill_hand::TakeIntoHandNow(L"gascan", kCanClass, &kCanFuel); });
    if (!PS::WaitFor("the gas can in this hand", 20, [] { return CanInHand(); })) return;
    float canAmount = -1.f;
    OnGameThread([&] { canAmount = CanAmount(coop::hand_item::LocalHandActor()); });
    UE_LOGI("woodchipper: CLIENT the can in hand holds %.2f", static_cast<double>(canAmount));
    PS::Mark("can_in_hand");

    // g1.
    std::string go;
    if (!PS::AwaitText("g1_go", go, kSignalSec)) return;
    const float gained = Pour(key);
    UE_LOGI("woodchipper: CLIENT poured: the copy gained %.2f", static_cast<double>(gained));
    PS::Mark("g1_poured", "gained=" + std::to_string(gained));
    std::string host;
    if (!PS::AwaitText("g1_host", host, kSignalSec)) return;
    const float hostFuel = Parse(host).fuel;
    PS::WaitFor("the copy at the host's fuel", kCopySec, [&] { return std::fabs(Now(key).fuel - hostFuel) <= kFuelTol; }, false);
    PS::Mark("g1_client", Text(Now(key)));

    // g2.
    if (!PS::AwaitText("g2_go", go, kSignalSec)) return;
    const Reading f2 = Parse(go);
    bool pressed = false;
    OnGameThread([&] { pressed = W::CallPress(ChipperOf(key), coop::players::Registry::Get().Local()); });
    UE_LOGI("woodchipper: CLIENT pressed the button (ran=%d)", pressed ? 1 : 0);
    PS::WaitFor("the copy on and burning", kBurnSec, [&] {
        const Reading r = Now(key);
        return r.active == 1 && r.fuel <= f2.fuel - kBurnDrop;
    }, false);
    coop::woodchipper_gate::Counts gc;
    OnGameThread([&] { gc = coop::woodchipper_gate::GetCounts(); });
    PS::Mark("g2_client", Text(Now(key)) + " tickRefused=" + std::to_string(gc.tickRefused));

    if (feed) {
        ClientFeed();
        UE_LOGI("woodchipper: FEED DONE on client");
        return;
    }

    // g5.
    if (!PS::Await("g5_go", kSignalSec)) return;
    const bool sent5 = SendPour(key, 80.f);
    UE_LOGI("woodchipper: CLIENT sent an over-pour of 80 (sent=%d)", sent5 ? 1 : 0);
    if (!PS::Await("g5_done", kSignalSec)) return;

    // g6.
    OnGameThread([] { coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr); });
    PS::WaitFor("this hand empty", 20, [] { return !CanInHand(); }, false);
    PS::Mark("g6_hand_empty");
    if (!PS::Await("g6_go", kSignalSec)) return;
    const bool sent6 = SendPour(key, 5.f);
    UE_LOGI("woodchipper: CLIENT sent a pour of 5 with an empty hand (sent=%d)", sent6 ? 1 : 0);
    if (!PS::Await("g6_done", kSignalSec)) return;

    // g4: this life's end, as the cremator drill's: the DONE line is the rig's cue to kill it.
    if (!PS::AwaitText("g4_on", go, kSignalSec)) return;
    PS::Mark("client_done", Text(Now(key)));
    PS::Await("host_ack", 60);
    OnGameThread([] {
        auto& s = harness::session_runtime::Session();
        if (s.running()) coop::net_pump::FleeAfterNativeMenuTravel(s, "woodchipper drill: life 1 leaves for the rejoin");
    });
    PS::WaitFor("this life's session stopped", 15, [] { return !harness::session_runtime::Session().running(); }, false);
    UE_LOGI("woodchipper: DONE on client");
}

void RunRejoin() {
    if (!HostPuppetUp()) return;
    PS::Mark("rejoin_ready");
    std::string keyText;
    if (!PS::AwaitText("e_key", keyText, 120)) return;
    const std::wstring key = Widen(keyText);
    if (!CopyResolves(key)) return;
    ReportCopy(key, 1, "rejoin_done");
    UE_LOGI("woodchipper: REJOIN DONE");
}

}  // namespace

DWORD WINAPI WoodchipperThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    if (!IsClientRole()) RunHost();
    else if (coop::config::ReadEnv("VOTVCOOP_WOODCHIPPER_DRILL_REJOIN") == "1") RunRejoin();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest
