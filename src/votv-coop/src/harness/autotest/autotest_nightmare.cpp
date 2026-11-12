// harness/autotest/autotest_nightmare.cpp -- TEST-ONLY: a nightmare is one player's (Ghezra/Multivoid issue 26).

// Phase A, the client's nightmare: both peers go to bed (the nearest bed, through the game's own sleep) and are held
// asleep by a sleep need written to 30 each second. The host's world is put back to a dilation of 1 [dev write], since
// at the game's 20 the night's scheduled events arrive within seconds and every sleeper wakes for them; its roll,
// which counts that world time, is held off besides (dreamProbability 0). The client's own copy of its bed is given
// dreamProb 1, so coop/player/nightmare_roll's first roll, 25 s into the rest, wins. Once the client dreams, it leaves
// the dream 15 s later through the game's own leaveDream.

// Phase B, the host's: the host goes to bed (or stays where it stands when an event refuses the bed), runs the game's
// own winning calls (wakeup, createDream) at once and leaves the dream 15 s later; the client stays awake.

// Each second each peer logs `nightmare: STATE`: its sleep and dream flags, its place, health and need, and every
// slot's rest state as it knows it. The world's nightmare rule (gamemode.nightmaresEnabled, latched from the save's
// rules) is written on at the start [dev write], since a save can turn nightmares off. Verdicts:
// `nightmare: VERDICT <phase> <role> PASS|FAIL <why>`. Gated by VOTVCOOP_RUN_NIGHTMARE, with the rig's needs refill
// off; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"
#include "coop/player/sleep_sync.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/sleep.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/world/active_events.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace SLP = ue_wrap::sleep;
namespace V   = ue_wrap::vitals;

std::string g_dir;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

const char* Who() { return IsClientRole() ? "client" : "host"; }
void* Me() { return coop::players::Registry::Get().Local(); }

void Write(const std::string& name) { std::ofstream(g_dir + "\\" + name, std::ios::binary | std::ios::trunc) << "x"; }
bool Seen(const std::string& name) { return static_cast<bool>(std::ifstream(g_dir + "\\" + name)); }

bool Await(const std::string& name, int seconds = 300) {
    for (int i = 0; i < seconds * 10; ++i) {
        if (Seen(name)) return true;
        ::Sleep(100);
    }
    UE_LOGW("nightmare: ABORT -- no '%hs' from the other peer", name.c_str());
    return false;
}

struct Now {
    bool asleep = false, dreaming = false;
    ue_wrap::FVector at{};
    float health = 0.f, need = 0.f;
};

Now Read() {
    Now n;
    OnGameThread([&n] {
        n.asleep = SLP::IsSleeping();
        n.dreaming = SLP::IsDreaming();
        if (void* body = SLP::OrigPawn()) E::TryGetActorLocation(body, n.at);
        else if (Me()) E::TryGetActorLocation(Me(), n.at);
        V::Read(V::Field::Health, &n.health);
        V::Read(V::Field::Sleep, &n.need);
    });
    return n;
}

void Log(const char* phase, const Now& n) {
    int st[3] = {};
    OnGameThread([&st] {
        for (int i = 0; i < 3; ++i) st[i] = static_cast<int>(coop::sleep_sync::PeerState(i));
    });
    UE_LOGI("nightmare: STATE %hs %hs asleep=%d dreaming=%d at=(%.0f,%.0f,%.0f) health=%.0f need=%.0f peers=[%d %d %d]",
            phase, Who(), n.asleep ? 1 : 0, n.dreaming ? 1 : 0, n.at.X, n.at.Y, n.at.Z, n.health, n.need, st[0], st[1],
            st[2]);
}

void Verdict(const char* phase, bool ok, const char* why) {
    UE_LOGI("nightmare: VERDICT %hs %hs %hs %hs", phase, Who(), ok ? "PASS" : "FAIL", why);
}

// To bed, the nearest, through the game's own sleep; the need first, so the game's wake at 100 is far off.
bool GoToBed() {
    bool ok = false;
    OnGameThread([&ok] {
        V::Write(V::Field::Food, 100.f);
        V::Write(V::Field::Sleep, 30.f);
        ue_wrap::FVector at{};
        if (Me()) E::TryGetActorLocation(Me(), at);
        void* bed = SLP::NearestBed(at);
        ok = bed && SLP::CallSleep(bed);
        // The host's world back to 1 in the same frame [dev write]: at the game's 20 the night's scheduled
        // events arrive within seconds, and every sleeper wakes for them.
        if (ok && !IsClientRole()) SLP::SetGlobalTimeDilation(1.f);
    });
    ::Sleep(1500);
    return ok && Read().asleep;
}

void HoldNeed() { OnGameThread([] { V::Write(V::Field::Sleep, 30.f); }); }

// This peer's own copy of its bed (the gamemode's bed while it sleeps) given a dream chance of `p`.
void BedChance(float p) {
    OnGameThread([p] {
        void* gm = ue_wrap::world_singleton::Gamemode();
        const int32_t offBed = gm ? R::FindPropertyOffset(R::ClassOf(gm), L"bed") : -1;
        void* bed = offBed >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(gm) + offBed) : nullptr;
        const int32_t off = bed ? R::FindPropertyOffset(R::ClassOf(bed), L"dreamProb") : -1;
        if (off >= 0) *reinterpret_cast<float*>(static_cast<char*>(bed) + off) = p;
        UE_LOGI("nightmare: %hs bed %ls dreamProb set to %.0f (%d)", Who(), bed ? R::ClassNameOf(bed).c_str() : L"none",
                static_cast<double>(p), off >= 0 ? 1 : 0);
    });
}

void LeaveDream() {
    OnGameThread([] {
        void* gm = ue_wrap::world_singleton::Gamemode();
        void* fn = gm ? R::FindFunction(R::ClassOf(gm), L"leaveDream") : nullptr;
        ue_wrap::ParamFrame pf(fn);
        const bool ok = fn && pf.valid() && ue_wrap::Call(gm, pf);
        UE_LOGI("nightmare: %hs leaveDream ran=%d", Who(), ok ? 1 : 0);
    });
}

void RuleOn() {
    OnGameThread([] {
        void* gm = ue_wrap::world_singleton::Gamemode();
        const int32_t off = gm ? R::FindPropertyOffset(R::ClassOf(gm), L"nightmaresEnabled") : -1;
        UE_LOGI("nightmare: %hs nightmaresEnabled=%d -- set to 1 for the drill [dev write]", Who(),
                off >= 0 ? static_cast<int>(*(static_cast<uint8_t*>(gm) + off)) : -1);
        if (off >= 0) *(static_cast<uint8_t*>(gm) + off) = 1;   // the rig's save has the rule off
    });
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The dreamer: wait for its dream (bounded), log it, leave after 15 s, and judge the return.
void Dream(const char* phase, int boundS, const Now& before) {
    int t = 0;
    Now n = before;
    for (; t < boundS && !n.dreaming; ++t) {
        ::Sleep(1000);
        if (!n.dreaming) HoldNeed();
        n = Read();
        Log(phase, n);
    }
    if (!n.dreaming) return Verdict(phase, false, "no dream came within the bound");
    UE_LOGI("nightmare: %hs %hs dreams after %d s, %.0f cm from its bed's place", phase, Who(), t,
            Dist(n.at, before.at));
    Write(std::string(phase) + "_dreaming");
    for (int i = 0; i < 15; ++i) { ::Sleep(1000); Log(phase, Read()); }
    LeaveDream();
    ::Sleep(2000);
    n = Read();
    Log(phase, n);
    const bool back = !n.dreaming && Dist(n.at, before.at) < 1500.f;
    Verdict(phase, back, back ? "dreamt and came back beside its bed" : "not back from the dream");
    Write(std::string(phase) + "_woke");
}

// The sleeper beside a dream: held asleep until the dreamer is back, and judged asleep throughout.
void StayAsleep(const char* phase, int otherSlot) {
    bool sawDream = false, stayed = true;
    for (int t = 0; t < 150 && !Seen(std::string(phase) + "_woke"); ++t) {
        ::Sleep(1000);
        HoldNeed();
        const Now n = Read();
        Log(phase, n);
        if (!n.asleep) stayed = false;
        int st = 0;
        OnGameThread([&st, otherSlot] { st = static_cast<int>(coop::sleep_sync::PeerState(otherSlot)); });
        if (st == static_cast<int>(coop::net::SleepPeerState::Dreaming)) sawDream = true;
    }
    Verdict(phase, stayed && sawDream,
            !stayed ? "woke while the other player dreamt" : sawDream ? "slept through it and saw the other dream"
                                                                    : "never saw the other player dream");
}

// Staging, for a person's test (VOTVCOOP_NIGHTMARE_STAGE): each peer alone, with no handshake. `sleeper` turns the
// rule on, goes to bed and is held there for good, back to bed whenever an event wakes it; `dreamer` turns the rule
// on, waits for the host to end the world's first event (an event ends a dream), and goes to bed with its bed's
// dreamProb at 1; with VOTVCOOP_NIGHTMARE_DREAM=<class> it starts that dream itself 20 s into the sleep instead. The
// rest is the person's.
//
// [dev, staging] The host ends every running ventCrawler event as the game ends one: lib_C::setEvent(false) with
// the creature as its sender, then the creature destroyed. The save's night brings one within minutes of the load,
// and it holds the event (which refuses every bed and ends every dream) for as long as the creature lives.
void EndCrawlerEvents() {
    OnGameThread([] {
        ue_wrap::active_events::Senders snd{};
        if (!ue_wrap::active_events::ReadSenders(snd) || snd.num <= 0) return;
        void* lib = R::FindClassDefaultObject(L"lib_C");
        void* fn = lib ? R::FindFunction(R::ClassOf(lib), L"setEvent") : nullptr;
        std::vector<void*> crawlers;
        for (int32_t i = 0; i < snd.num; ++i)
            if (snd.data[i] && R::IsLive(snd.data[i]) && R::ClassNameOf(snd.data[i]) == L"ventCrawler_C")
                crawlers.push_back(snd.data[i]);
        for (void* c : crawlers) {
            ue_wrap::ParamFrame pf(fn);
            const bool ended = fn && pf.valid() && pf.Set<bool>(L"isEventActive", false) &&
                               pf.Set<bool>(L"deactivateAmbientTrack", false) && pf.Set<void*>(L"__WorldContext", c) &&
                               ue_wrap::Call(lib, pf);
            void* destroy = R::FindDispatchFunctionCached(R::ClassOf(c), L"K2_DestroyActor");
            ue_wrap::ParamFrame d(destroy);
            const bool gone = destroy && d.valid() && ue_wrap::Call(c, d);
            int32_t n = -1;
            ue_wrap::active_events::ReadCount(n);
            UE_LOGI("nightmare: host ended a ventCrawler's event (setEvent=%d destroyed=%d) -- %d event(s) left",
                    ended ? 1 : 0, gone ? 1 : 0, n);
        }
        if (!crawlers.empty()) Write("crawler_ended");
    });
}

void Stage(const std::string& as) {
    if (IsClientRole()) {
        bool ready = false;
        for (int i = 0; i < 480 && !ready; ++i) {
            OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
            if (!ready) ::Sleep(500);
        }
        ::Sleep(10000);
    } else {
        ::Sleep(20000);
    }
    RuleOn();
    if (as == "dreamer") {
        // Any running event ends a dream (dreamBase_C's tick asks getEvent), and a client counts the host world's
        // events too: the save's night brings one within minutes of the load, so the dream waits for it to pass.
        // The host ends it, and leaves a file saying so.
        const bool passed = Await("crawler_ended", 360);
        UE_LOGI("nightmare: %hs dreamer -- %hs", Who(),
                passed ? "the world's event has passed" : "no event passed in 6 min");
        for (int i = 0; i < 10 && !GoToBed(); ++i) ::Sleep(5000);
        const std::string cls = coop::config::ReadEnv("VOTVCOOP_NIGHTMARE_DREAM");
        if (!cls.empty()) {
            // A named dream: the game's own calls with that class, 20 s into the sleep, the roll held off.
            BedChance(0.f);
            ::Sleep(20000);
            OnGameThread([&cls] {
                const std::wstring w(cls.begin(), cls.end());
                void* c = ue_wrap::object_index::ClassByName(w.c_str());
                UE_LOGI("nightmare: %hs dreamer -- %ls: BeginNightmare ran=%d", Who(), w.c_str(),
                        c && SLP::BeginNightmare(c) ? 1 : 0);
            });
            UE_LOGI("nightmare: STAGED %hs dreamer -- in a %hs nightmare", Who(), cls.c_str());
            return;
        }
        BedChance(1.f);
        UE_LOGI("nightmare: STAGED %hs dreamer -- in bed with a certain nightmare", Who());
        return;
    }
    UE_LOGI("nightmare: STAGED %hs sleeper -- held asleep, with no nightmare of its own", Who());
    OnGameThread([] { SLP::SetDreamProbability(0.f); });   // the host's own roll; a client's is held at 0 already
    for (int t = 0;; ++t) {
        if (!IsClientRole()) EndCrawlerEvents();
        if (!Read().asleep && t % 5 == 0 && GoToBed()) BedChance(0.f);
        HoldNeed();
        ::Sleep(1000);
    }
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
        OnGameThread([] { SLP::SetDreamProbability(0.f); });   // phase A: the host's roll held off
    }
    // Phase A: both asleep; the client dreams.
    RuleOn();
    const bool slept = GoToBed();
    UE_LOGI("nightmare: A %hs in bed=%d", Who(), slept ? 1 : 0);
    if (!slept) return Verdict("A", false, "the game's sleep did not take this player to bed");
    Write(std::string("A_bed_") + Who());
    if (!Await(std::string("A_bed_") + (IsClientRole() ? "host" : "client"))) return;
    if (IsClientRole()) {
        BedChance(1.f);
        Dream("A", 60, Read());
    } else {
        StayAsleep("A", 1);
        OnGameThread([] { V::Write(V::Field::Sleep, 100.f); });   // the game's own wake
    }
    ::Sleep(5000);
    // Phase B: the host dreams; the client is awake.
    if (!IsClientRole()) {
        // An event running now refuses the bed (the night's own, often begun as phase A ends); the dream does not
        // need it, so the host then dreams from where it stands.
        if (!GoToBed()) UE_LOGI("nightmare: B host not in bed (an event refused it) -- dreaming from where it stands");
        const Now inBed = Read();
        OnGameThread([] { UE_LOGI("nightmare: B host BeginNightmare ran=%d", SLP::BeginNightmare() ? 1 : 0); });
        Dream("B", 10, inBed);
        OnGameThread([] { SLP::SetDreamProbability(-1.f); });
    } else {
        for (int t = 0; t < 120 && !Seen("B_woke"); ++t) { ::Sleep(1000); Log("B", Read()); }
    }
    UE_LOGI("nightmare: DONE %hs", Who());
}

}  // namespace

DWORD WINAPI NightmareThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("nightmare: ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    const std::string stage = coop::config::ReadEnv("VOTVCOOP_NIGHTMARE_STAGE");
    if (!stage.empty()) Stage(stage);
    else Run();
    return 0;
}

}  // namespace harness::autotest
