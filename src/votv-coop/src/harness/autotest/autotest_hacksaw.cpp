// harness/autotest/autotest_hacksaw.cpp -- TEST-ONLY: a big prop sawn apart, and what every peer sees of it.
//
// One peer (the client, or the host with VOTVCOOP_HACKSAW_WHO=host) spawns a hacksaw of its own beside the nearest
// heavy prop it can saw (of VOTVCOOP_HACKSAW_LINEAGE's lineage, when given) and lets the saw's own tick cut it: the
// drill writes what the saw's use would (the prop, the game's own processMat for the scrap it gives, two pieces, half
// a second), so the game's completion runs whole -- the scrap spawned in the prop's bounds, the prop's broken, the
// prop destroyed. Each peer counts the scrap and the other props within 4 m before the cut and five seconds after,
// and reads whether the prop is gone. The host says `rig: VERDICT hacksaw PASS|FAIL|INVALID`: PASS when the prop is
// gone on both peers and both count the same new scrap, at least one, and the same new other props (a container's
// spill). Gated by VOTVCOOP_RUN_HACKSAW; the peers' folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/host_world_settle.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"  // DestroyActor

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace PS = harness::autotest::peer_signal;
namespace R = ue_wrap::reflection;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

// HOST: its player stands and its world has settled, so what the drill cuts is not taken back by the load.
bool HostWorldUp() {
    if (!PS::WaitFor("the host's player", 240, [] {
            void* pawn = nullptr;
            OnGameThread([&] { pawn = coop::players::Registry::Get().Local(); });
            return pawn != nullptr;
        }))
        return false;
    return PS::WaitFor("the host's world settled", 120, [] {
        bool settled = false;
        OnGameThread([&] {
            settled = coop::host_world_settle::Settled() && ue_wrap::object_index::IsSeeded() &&
                      ue_wrap::object_index::Backlog() == 0;
        });
        return settled;
    });
}

// CLIENT: its world is announced and the host's puppet stands here.
bool HostPuppetUp() {
    if (!PS::WaitFor("the announced world", 480, [] {
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

constexpr const char* kTag = "hacksaw";
constexpr int kStepSec = 120;
constexpr float kNearCm = 400.f;

bool HostActs() { return coop::config::ReadEnv("VOTVCOOP_HACKSAW_WHO") == "host"; }

struct Target {
    std::wstring cls, key;
    ue_wrap::FVector at{};
};

bool IsScrap(void* p) { return ue_wrap::prop::GetPropNameString(p).rfind(L"scrap_", 0) == 0; }

// The scrap props within kNearCm of `at`; with `say`, each one within 10 m is logged with its distance.
int ScrapNear(const ue_wrap::FVector& at, const char* say = nullptr) {
    int n = 0;
    OnGameThread([&] {
        for (void* p : R::FindObjectsByClass(L"prop_C")) {
            if (!R::IsLive(p) || !IsScrap(p)) continue;
            ue_wrap::FVector l{};
            if (!ue_wrap::engine::TryGetActorLocation(p, l)) continue;
            const float dx = l.X - at.X, dy = l.Y - at.Y, dz = l.Z - at.Z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 <= kNearCm * kNearCm) ++n;
            if (say && d2 <= 1000.f * 1000.f)
                UE_LOGI("hacksaw: %hs scrap '%ls' key '%ls' %.0f cm from the cut", say,
                        ue_wrap::prop::GetPropNameString(p).c_str(), ue_wrap::prop::GetInteractableKeyString(p).c_str(),
                        std::sqrt(d2));
        }
    });
    return n;
}

// Every prop within kNearCm of the target but itself and the scrap: what a container's spill adds. A nearest-walk
// per prop, so a handful of walks.
int OthersNear(const Target& t) {
    int n = 0;
    OnGameThread([&] {
        std::vector<void*> seen;
        for (int i = 0; i < 40; ++i) {
            const ue_wrap::prop::NearestResult r =
                ue_wrap::prop::FindNearest(t.at, false, nullptr, nullptr, {seen.data(), seen.size()});
            if (!r.prop || r.dist > kNearCm) break;
            seen.push_back(r.prop);
            if (r.keyString == t.key || IsScrap(r.prop)) continue;
            ++n;
        }
    });
    return n;
}

bool StillThere(const Target& t) {
    bool there = false;
    OnGameThread([&] {
        for (void* p : R::FindObjectsByClass(t.cls.c_str()))
            if (R::IsLive(p) && ue_wrap::prop::GetInteractableKeyString(p) == t.key) there = true;
    });
    return there;
}

template <class T>
bool WriteField(void* obj, const wchar_t* name, const T& v) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), name);
    if (off < 0) return false;
    *reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + off) = v;
    return true;
}

void* g_saw = nullptr;  // the actor's saw, between Pick and Start

// The actor's cut, part one: a saw of its own beside the nearest heavy, dynamic prop its processMat takes, set as the
// saw's use sets it. Returns the prop, or an empty class when it found none.
Target Pick(const char* me) {
    Target t;
    const std::string lin = coop::config::ReadEnv("VOTVCOOP_HACKSAW_LINEAGE");
    const std::wstring lineage(lin.begin(), lin.end());
    const bool full = coop::config::ReadEnv("VOTVCOOP_HACKSAW_FULL") == "1";
    OnGameThread([&] {
        void* local = coop::players::Registry::Get().Local();
        void* sawCls = ue_wrap::object_index::ClassByName(L"prop_hacksaw_C");
        ue_wrap::FVector from{};
        if (!local || !sawCls || !ue_wrap::engine::TryGetActorLocation(local, from)) return;
        void* saw = ue_wrap::engine::SpawnActor(sawCls, ue_wrap::FVector{from.X, from.Y, from.Z + 200.f});
        if (!saw) return;
        void* processMat = R::FindFunction(R::ClassOf(saw), L"processMat");
        std::vector<void*> tried;
        for (int i = 0; i < 64 && processMat; ++i) {
            const ue_wrap::prop::NearestResult r = ue_wrap::prop::FindNearest(
                from, true, nullptr, lineage.empty() ? nullptr : lineage.c_str(), {tried.data(), tried.size()});
            if (!r.prop) break;
            tried.push_back(r.prop);
            if (r.isStatic || r.keyString.empty() || r.keyString == L"None") continue;
            if (full) {  // VOTVCOOP_HACKSAW_FULL=1: a container holding something
                int32_t count = -1;
                std::vector<ue_wrap::save_record::SaveRecord> recs;
                void* inv = ue_wrap::container_inventory::IsContainer(r.prop)
                                ? ue_wrap::container_inventory::InventoryOf(r.prop)
                                : nullptr;
                if (!inv) continue;
                ue_wrap::container_inventory::ReadContents(inv, recs, 64, &count);
                if (count < 1) continue;
                UE_LOGI("hacksaw: %hs '%ls' holds %d record(s)", me, r.className.c_str(), count);
            }
            if (!WriteField<void*>(saw, L"sawingProp", r.prop)) break;
            ue_wrap::ParamFrame f(processMat);
            if (!f.valid() || !ue_wrap::Call(saw, f) || !f.Get<bool>(L"return")) continue;
            ue_wrap::engine::TryGetActorLocation(r.prop, t.at);
            t.cls = r.className;
            t.key = r.keyString;
            break;
        }
        if (t.cls.empty()) {
            ue_wrap::engine::DestroyActor(saw);
            return;
        }
        WriteField<int32_t>(saw, L"amount", 2);
        WriteField<float>(saw, L"time", 0.5f);
        WriteField<float>(saw, L"complete", 0.f);
        g_saw = saw;
    });
    UE_LOGI("hacksaw: %hs saws '%ls' key '%ls' at %.0f,%.0f,%.0f", me, t.cls.c_str(), t.key.c_str(), t.at.X, t.at.Y,
            t.at.Z);
    return t;
}

// Part two: the saw's tick runs, and its completion cuts the prop half a second later.
void Start() {
    OnGameThread([&] {
        if (g_saw && R::IsLive(g_saw)) ue_wrap::engine::SetActorTickEnabled(g_saw, true);
    });
}

std::string Encode(const Target& t) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%.1f %.1f %.1f %ls %ls", t.at.X, t.at.Y, t.at.Z, t.cls.c_str(), t.key.c_str());
    return buf;
}

bool Decode(const std::string& s, Target& t) {
    char cls[96] = {}, key[128] = {};
    if (std::sscanf(s.c_str(), "%f %f %f %95s %127s", &t.at.X, &t.at.Y, &t.at.Z, cls, key) != 5) return false;
    for (const char* c = cls; *c; ++c) t.cls.push_back(static_cast<wchar_t>(*c));
    for (const char* c = key; *c; ++c) t.key.push_back(static_cast<wchar_t>(*c));
    return true;
}

// "gone newScrap newOthers" on this peer, five seconds after the cut.
std::string Watch(const char* me, const Target& t, int scrapBefore, int othersBefore) {
    ::Sleep(5000);
    const bool gone = !StillThere(t);
    const int scrap = ScrapNear(t.at, me) - scrapBefore;
    const int others = OthersNear(t) - othersBefore;
    UE_LOGI("hacksaw: %hs the prop gone=%d, new within %.0f m: scrap %d, other props %d", me, gone ? 1 : 0,
            kNearCm / 100.f, scrap, others);
    return std::to_string(gone ? 1 : 0) + " " + std::to_string(scrap) + " " + std::to_string(others);
}

// The actor picks the prop and says where; each peer counts what stands near it; the actor saws once the other peer
// has counted.
void RunClient() {
    if (!HostPuppetUp()) return;
    ::Sleep(20000);  // past the join's reconcile window, whose churn a client's births and destroys sit out
    PS::Mark("client_ready");
    Target t;
    std::string text;
    if (!HostActs()) {
        if (!PS::Await("host_ready", kStepSec)) return;
        t = Pick("CLIENT");
        PS::Mark("picked", t.cls.empty() ? std::string("none") : Encode(t));
        if (t.cls.empty()) return;
    } else if (!PS::AwaitText("picked", text, kStepSec) || !Decode(text, t)) {
        return;
    }
    const int scrapBefore = ScrapNear(t.at), othersBefore = OthersNear(t);
    PS::Mark("client_counted");
    if (!HostActs()) {
        if (!PS::Await("host_counted", kStepSec)) return;
        Start();
    }
    PS::Mark("client_result", Watch("CLIENT", t, scrapBefore, othersBefore));
    PS::Await("host_done", kStepSec);
    UE_LOGI("hacksaw: DONE on client");
}

void RunHost() {
    if (!HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    PS::Mark("host_ready");
    Target t;
    std::string text;
    if (HostActs()) {
        t = Pick("HOST");
        PS::Mark("picked", t.cls.empty() ? std::string("none") : Encode(t));
    } else if (PS::AwaitText("picked", text, kStepSec)) {
        Decode(text, t);
    }
    if (t.cls.empty()) {
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "no heavy prop the saw takes near the %s",
                               HostActs() ? "host" : "client");
        PS::Mark("host_done");
        return;
    }
    const int scrapBefore = ScrapNear(t.at), othersBefore = OthersNear(t);
    PS::Mark("host_counted");
    if (HostActs()) {
        if (!PS::Await("client_counted", kStepSec)) return;
        Start();
    }
    const std::string mine = Watch("HOST", t, scrapBefore, othersBefore);
    std::string theirs;
    PS::AwaitText("client_result", theirs, kStepSec);
    PS::Mark("host_done");
    int hg = 0, hs = 0, ho = 0, cg = 0, cs = 0, co = 0;
    if (std::sscanf(mine.c_str(), "%d %d %d", &hg, &hs, &ho) != 3 ||
        std::sscanf(theirs.c_str(), "%d %d %d", &cg, &cs, &co) != 3) {
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "a peer's reading did not come");
        return;
    }
    const bool pass = hg && cg && hs >= 1 && hs == cs && ho == co;
    coop::rig_verdict::Say(kTag, pass ? coop::rig_verdict::Outcome::Pass : coop::rig_verdict::Outcome::Fail,
                           "the %s sawed '%ls': gone on the host=%d, on the client=%d; new near it on the host: scrap %d, "
                           "other props %d; on the client: scrap %d, other props %d",
                           HostActs() ? "host" : "client", t.cls.c_str(), hg, cg, hs, ho, cs, co);
    UE_LOGI("hacksaw: DONE on host");
}

}  // namespace

DWORD WINAPI HacksawThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    if (!IsClientRole()) RunHost();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest
