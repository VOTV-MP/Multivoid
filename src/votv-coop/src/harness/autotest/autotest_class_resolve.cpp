// harness/autotest/autotest_class_resolve.cpp -- TEST-ONLY: the host's spawn of a class the client has not loaded
// (class_resolve_drill, both peers).
//
// The client asks the engine's asset registry for prop Blueprints and keeps the ones its object index does not hold
// (a class a fresh client never touched), up to kCandidates, and names them to the host. The host loads and spawns the
// first kWanted of them in front of its player, where its spawn watcher sends each as a PropSpawn. The client resolves
// each wire class through class_resolve, which loads a class it has not loaded; it counts the mirrors it holds and
// tells the host once each class is DECIDED by an event: its mirror bound, or its own resolve refused it. The host judges
// and says `rig: VERDICT class-resolve PASS|FAIL|INVALID`: PASS when the client holds
// a bound mirror of every class the host spawned. INVALID when the client holds every prop class already, or the host
// could not spawn kWanted of the candidates. Each wait is on an event, and its bound says INVALID only, never PASS or FAIL.
//
// Red arm: class_resolve_lookup_only=1 on the CLIENT turns the registry step off, so the client's resolve is the object
// index alone, as before the resolver; its resolve refuses each class (the deciding event) and the host's verdict is FAIL.
// Lines: `class-resolve: ...`; the host's end is `class-resolve: DONE on host`, the client's `class-resolve: DONE on client`.

#include "harness/autotest.h"   // IsClientRole, WaitPeerWorldReady
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/element.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/remote_prop.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/class_resolve.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace CR = ue_wrap::class_resolve;
namespace OI = ue_wrap::object_index;
namespace PS = peer_signal;
namespace RV = coop::rig_verdict;

constexpr const char* kTag = "class-resolve";
constexpr size_t kCandidates = 12;  // unloaded prop classes the client names: the host may not spawn every one
constexpr size_t kWanted = 3;       // classes the host spawns and the client must mirror
constexpr int kJoinSec = 480;
constexpr int kStepSec = 240;       // the bound on one step of the other peer's
constexpr int kMirrorSec = 60;      // the client's bound on every class being decided: INVALID past it
constexpr int kTrackSec = 20;       // the host's bound on its spawns being tracked: INVALID past it
constexpr float kAheadCm = 250.f;
constexpr float kApartCm = 110.f;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }  // a class name is ASCII
std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::string Join(const std::vector<std::wstring>& names) {
    std::string out;
    for (const auto& n : names) out += (out.empty() ? "" : ",") + Narrow(n);
    return out;
}

std::vector<std::wstring> Split(const std::string& text) {
    std::vector<std::wstring> out;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t comma = text.find(',', at);
        const std::string one = text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (!one.empty()) out.push_back(Widen(one));
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    return out;
}

bool WorldUp() {
    return PS::WaitFor("the announced world", kJoinSec, [] {
        bool ready = false;
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        return ready;
    });
}

// ---- CLIENT ----------------------------------------------------------------------------------------------------

// Prop classes the registry holds and this client's object index does not: what a fresh client has not loaded.
std::vector<std::wstring> UnloadedPropClasses() {
    std::vector<std::wstring> out;
    OnGameThread([&] {
        for (const std::wstring& name : CR::RegistryClassNames(L"prop_")) {
            if (out.size() >= kCandidates) break;
            if (OI::ClassByName(name.c_str()) || R::FindClass(name.c_str())) continue;
            out.push_back(name);
        }
    });
    return out;
}

// How many of `names` this client holds a bound mirror of.
size_t MirrorsBound(const std::vector<std::wstring>& names) {
    size_t n = 0;
    OnGameThread([&] {
        for (const std::wstring& name : names) {
            for (void* actor : R::FindObjectsByClass(name.c_str())) {
                if (coop::remote_prop::ResolveMirrorEidByActor(actor) != coop::element::kInvalidId) {
                    ++n;
                    break;
                }
            }
        }
    });
    return n;
}

// How many of `names` are decided here: a bound mirror, or a refusal this client's own resolver gave the class.
size_t Decided(const std::vector<std::wstring>& names) {
    size_t n = 0;
    OnGameThread([&] {
        for (const std::wstring& name : names) {
            bool bound = false;
            for (void* actor : R::FindObjectsByClass(name.c_str())) {
                if (coop::remote_prop::ResolveMirrorEidByActor(actor) != coop::element::kInvalidId) {
                    bound = true;
                    break;
                }
            }
            if (bound || CR::WasDeclined(name.c_str())) ++n;
        }
    });
    return n;
}

void RunClient() {
    if (!WorldUp()) return;
    if (coop::config::ResolveFlag(coop::config_registry::rows::class_resolve_lookup_only)) {
        OnGameThread([] { CR::SetRegistryStep(false); });
        UE_LOGW("class-resolve: CLIENT red arm: the registry step is off, so a class this client has not loaded is "
                "not loaded");
    }
    const std::vector<std::wstring> unloaded = UnloadedPropClasses();
    for (const auto& n : unloaded) UE_LOGI("class-resolve: CLIENT candidate %ls is not in the object index", n.c_str());
    if (unloaded.empty()) {
        UE_LOGW("class-resolve: CLIENT holds every prop class the registry lists -- nothing to load");
        PS::Mark("candidates", "NONE");
        return;
    }
    PS::Mark("candidates", Join(unloaded));

    std::string spawnedText;
    if (!PS::AwaitText("spawned", spawnedText, kStepSec) || spawnedText == "NONE") return;
    const std::vector<std::wstring> spawned = Split(spawnedText);
    // Each class is decided by an event: its mirror is bound, or this client's own resolve refused it. The bound
    // only says INVALID (a spawn that neither bound nor was refused).
    if (!PS::WaitFor("every class the host spawned bound or refused here", kMirrorSec,
                     [&] { return Decided(spawned) == spawned.size(); }))
        return;
    const size_t bound = MirrorsBound(spawned);
    UE_LOGI("class-resolve: CLIENT holds a bound mirror of %zu of the %zu classes the host spawned (%zu refused by its "
            "own resolve)", bound, spawned.size(), spawned.size() - bound);
    UE_LOGI("class-resolve: CLIENT loads run=%llu, of them during the save replay=%llu",
            static_cast<unsigned long long>(CR::LoadsRun()), static_cast<unsigned long long>(CR::LoadsDuringReplay()));
    PS::Mark("bound", std::to_string(bound));
    UE_LOGI("class-resolve: DONE on client");
}

// ---- HOST ------------------------------------------------------------------------------------------------------

// Loads (through the resolver) and spawns `names` in front of the host's player until kWanted stand; the classes that
// stood are in `spawned`, their actors in `actors`. The host loads the CLIENT's candidates here on purpose, as a test
// driver that stands in for the host's own gameplay reaching a class (shipping code never loads on a client's name), and
// loads Immediate because the drill acts in place on the game thread, with nothing to wait for. Each class is resolved
// twice in the same tick: the second resolve must load nothing (the first one's drain lists the class), else `reloaded`.
void SpawnCandidates(const std::vector<std::wstring>& names, std::vector<std::wstring>& spawned,
                     std::vector<ue_wrap::CachedObjRef>& actors, bool& reloaded) {
    OnGameThread([&] {
        void* me = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!me || !E::TryGetActorLocation(me, at)) return;
        const ue_wrap::FVector fwd = E::GetActorForwardVector(me);
        for (const std::wstring& name : names) {
            if (spawned.size() >= kWanted) break;
            const auto r = CR::ResolveClass(name.c_str(), ue_wrap::prop::PropFamily(), CR::Load::Immediate);
            const uint64_t loadsAfterFirst = CR::LoadsRun();
            const auto again = CR::ResolveClass(name.c_str(), ue_wrap::prop::PropFamily(), CR::Load::Immediate);
            if (CR::LoadsRun() != loadsAfterFirst || again.cls != r.cls) {
                reloaded = true;
                UE_LOGW("class-resolve: HOST a second resolve of %ls in the same tick loaded it again", name.c_str());
            }
            if (r.state != CR::ClassState::Found) {
                UE_LOGW("class-resolve: HOST cannot resolve %ls (state %d) -- skipped", name.c_str(),
                        static_cast<int>(r.state));
                continue;
            }
            const float side = kApartCm * static_cast<float>(spawned.size());
            void* actor = E::SpawnActor(r.cls, {at.X + fwd.X * kAheadCm - fwd.Y * side,
                                                at.Y + fwd.Y * kAheadCm + fwd.X * side, at.Z + 60.f});
            if (!actor) {
                UE_LOGW("class-resolve: HOST spawn of %ls failed -- skipped", name.c_str());
                continue;
            }
            ue_wrap::CachedObjRef ref;
            ref.Set(actor);
            actors.push_back(ref);
            spawned.push_back(name);
            UE_LOGI("class-resolve: HOST spawned %ls", name.c_str());
        }
    });
}

bool AllTracked(std::vector<ue_wrap::CachedObjRef>& actors) {
    bool all = true;
    OnGameThread([&] {
        for (auto& ref : actors) {
            void* a = ref.Get();
            if (!a || coop::prop_element_tracker::GetPropElementIdForActor(a) == coop::element::kInvalidId) all = false;
        }
    });
    return all;
}

void RunHost() {
    if (!WaitPeerWorldReady(1, static_cast<DWORD>(kJoinSec) * 1000)) {
        RV::Say(kTag, RV::Outcome::Invalid, "the client never reached its world");
        return;
    }
    std::string text;
    if (!PS::AwaitText("candidates", text, kStepSec)) return;
    if (text == "NONE") {
        PS::Mark("spawned", "NONE");
        RV::Say(kTag, RV::Outcome::Invalid, "the client holds every prop class the registry lists");
        return;
    }
    std::vector<std::wstring> spawned;
    std::vector<ue_wrap::CachedObjRef> actors;
    bool reloaded = false;
    SpawnCandidates(Split(text), spawned, actors, reloaded);
    // The verdict known is said at once, before any later leg: the second resolve loaded the class again. The mark
    // releases the client, which waits for the host's word.
    if (reloaded) {
        PS::Mark("spawned", "NONE");
        RV::Say(kTag, RV::Outcome::Fail, "a second resolve of a class in the same tick loaded it again");
        return;
    }
    if (spawned.size() < kWanted) {
        PS::Mark("spawned", "NONE");
        RV::Say(kTag, RV::Outcome::Invalid, "the host could spawn only %zu of the %zu classes it needs", spawned.size(),
                kWanted);
        return;
    }
    if (!PS::WaitFor("the host's spawns tracked", kTrackSec, [&] { return AllTracked(actors); })) {
        PS::Mark("spawned", "NONE");   // the client has nothing to wait for
        return;
    }
    PS::Mark("spawned", Join(spawned));

    std::string boundText;
    if (!PS::AwaitText("bound", boundText, kStepSec)) return;
    const size_t bound = static_cast<size_t>(std::stoul(boundText));
    UE_LOGI("class-resolve: HOST spawned %zu classes the client had not loaded; the client holds %zu mirrors", spawned.size(),
            bound);
    UE_LOGI("class-resolve: HOST loads run=%llu, of them during the save replay=%llu",
            static_cast<unsigned long long>(CR::LoadsRun()), static_cast<unsigned long long>(CR::LoadsDuringReplay()));
    if (bound == spawned.size()) RV::Say(kTag, RV::Outcome::Pass);
    else RV::Say(kTag, RV::Outcome::Fail, "the client holds a mirror of %zu of the %zu classes the host spawned", bound,
                 spawned.size());
    UE_LOGI("class-resolve: DONE on host");
}

}  // namespace

DWORD WINAPI ClassResolveDrillThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest
