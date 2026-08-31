// harness/autotest_reloadchurn.cpp -- the RE-LOAD CHURN probe.
//
// WHY THIS EXISTS (2026-08-31, the user's rejoin crash).
// A client joined, left to the menu, joined again, and the process died two seconds
// into the SECOND world load:
//
//     EXCEPTION_ACCESS_VIOLATION reading address 0x0000000000000268
//
// The minidump's PortableCallStack was resolved statically against the shipped exe
// (.pdata + capstone, no IDB), and the fault is not a mystery -- every frame and both
// offsets are measured:
//
//     UEngine::Browse                    VotV+0x2f253c0  ("Invalid URL: {0}", "Servers can't open network URLs")
//       -> UE4SS's LoadMap detour        UE4SS+0x554abd  (UE4SS hooks UEngine::LoadMap; HookLoadMap defaults true)
//       -> UEngine::LoadMap              VotV+0x2f3a7fd  ("Couldn't spawn player: %s", "Mutator=", "DemoRec")
//         -> UGameInstance::CreateGameModeForURL  VotV+0x2b3f0c0  ("GAME=", "LoadForAllGameModes")
//              rbx = UWorld::GetWorldSettings(InWorld, bCheckStreamingPersistent=0, bChecked=1)
//              mov rbx, [rbx + 0x268]    <-- FAULT, rbx == nullptr
//
//   [V] UWorld::PersistentLevel         @ 0x030  (disasm + CXXHeaderDump/Engine.hpp agree)
//   [V] ULevel::WorldSettings           @ 0x258  (ULevel::GetWorldSettings is literally
//                                                 "mov rax,[rcx+0x258]; ret")
//   [V] AWorldSettings::DefaultGameMode @ 0x268  -- the faulting offset, exactly.
//
// So the world the engine was about to hand a GameMode had NO WorldSettings: either
// PersistentLevel was null, or PersistentLevel->WorldSettings was. In a SHIPPING build
// ULevel::GetWorldSettings's checkf(WorldSettings != nullptr) is compiled out, so the
// null is returned silently and dereferenced one frame up.
//
// Multivoid is on NEITHER side of that: our DLL does not appear anywhere in the stack,
// and the tree contains ZERO references to WorldSettings or PersistentLevel. That is
// why this probe's first arm is a NEGATIVE CONTROL -- solo, sessionless, no peer, no
// coop layer running at all -- because "the coop rejoin broke it" and "the second
// in-process map load breaks it" produce the identical user-visible report, and only
// one of them is ours to fix.
//
// What it does, per cycle: settle in gameplay, census, travel to the menu with the
// layer LIVE (the player's own exit shape), census AT THE MENU -- the decisive frame,
// because an Untitled_1 UWorld still resident there with a null WorldSettings IS the
// crash, one load early and visible without dying -- then re-load and census again. It
// reports every live UWorld with its PersistentLevel / WorldSettings / DefaultGameMode
// chain, so the failing link is NAMED rather than inferred from a fault address.
//
// Gated by env VOTVCOOP_RUN_RELOAD_CHURN=1; launch "python tools/mp.py reloadchurn".
// Diagnostic -- not a shipping path.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/session/session_manager.h"
#include "harness/session_runtime.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace P  = ue_wrap::profile;
namespace GT = ue_wrap::game_thread;
namespace E  = ue_wrap::engine;

bool WaitDone(const std::shared_ptr<std::atomic<int>>& d, int timeoutMs) {
    for (int i = 0; i < timeoutMs / 5 && d->load() == 0; ++i) ::Sleep(5);
    return d->load() != 0;
}

int EnvInt(const char* key, int fallback) {
    const std::string v = coop::config::ReadEnv(key);
    return v.empty() ? fallback : atoi(v.c_str());
}

// ---------------------------------------------------------------------------------
// The census. One line per live UWorld, naming every link of the chain the crash
// walked. Runs INLINE on the game thread (the caller posts it).
//
// The three offsets come from reflection, never from the literals in the banner: the
// banner records what the crash measured on THIS build, the code must survive a recook.
// A missing property is reported as such rather than silently reading offset 0 -- a
// census that cannot see the field must not print a confident "null".
// ---------------------------------------------------------------------------------
struct Offsets {
    int32_t persistentLevel = -1;
    int32_t worldSettings   = -1;
    int32_t defaultGameMode = -1;
    bool ok() const { return persistentLevel >= 0 && worldSettings >= 0 && defaultGameMode >= 0; }
};

Offsets ResolveOffsetsGT() {
    Offsets o;
    if (void* c = R::FindClass(P::name::WorldClass))
        o.persistentLevel = R::FindPropertyOffset(c, L"PersistentLevel");
    if (void* c = R::FindClass(L"Level"))
        o.worldSettings = R::FindPropertyOffset(c, L"WorldSettings");
    if (void* c = R::FindClass(L"WorldSettings"))
        o.defaultGameMode = R::FindPropertyOffset(c, L"DefaultGameMode");
    return o;
}

void* ReadPtr(void* base, int32_t off) {
    return (base && off >= 0) ? *reinterpret_cast<void**>(static_cast<char*>(base) + off) : nullptr;
}

// "tag" names WHEN in the cycle this frame was taken, so a log reader can pair a null
// with the moment it appeared instead of counting lines.
void CensusGT(const char* tag, int cycle, const Offsets& o) {
    if (!o.ok()) {
        UE_LOGW("reloadchurn[%s c%d]: OFFSETS UNRESOLVED (PersistentLevel=%d WorldSettings=%d "
                "DefaultGameMode=%d) -- census skipped, NOT a clean world",
                tag, cycle, o.persistentLevel, o.worldSettings, o.defaultGameMode);
        return;
    }
    const std::vector<void*> worlds = R::FindObjectsByClass(P::name::WorldClass);
    int nullWs = 0;
    size_t live = 0;
    void* deadWorld = nullptr;
    UE_LOGI("reloadchurn[%s c%d]: %zu UWorld object(s) in the array", tag, cycle, worlds.size());
    for (void* w : worlds) {
        if (!w) continue;
        // DEAD worlds are printed too, not skipped. The first version skipped them and that
        // hid the whole question: `open untitled_1` a second time in one process REUSES the
        // still-resident map package, so the world/level whose WorldSettings the next LoadMap
        // reads may be one that is already kill-flagged at the menu. A census that only
        // reports what is live cannot see a reused corpse.
        const bool alive = R::IsLive(w);
        if (alive) ++live; else deadWorld = w;
        const std::wstring wname = R::ToString(R::NameOf(w));
        void* lvl = ReadPtr(w, o.persistentLevel);
        void* ws  = ReadPtr(lvl, o.worldSettings);
        void* gm  = ReadPtr(ws, o.defaultGameMode);
        std::wstring lname = L"<none>";
        if (lvl && R::IsLive(lvl)) lname = R::ToString(R::NameOf(lvl));
        if (alive && lvl && !ws) ++nullWs;
        UE_LOGI("reloadchurn[%s c%d]:   world='%ls' @%p %s PersistentLevel=%p ('%ls')  "
                "WorldSettings=%p  DefaultGameMode=%p%s",
                tag, cycle, wname.c_str(), w, alive ? "LIVE" : "dead", lvl, lname.c_str(), ws, gm,
                (lvl && !ws) ? "   <<< WORLDSETTINGS IS NULL -- this world would CRASH LoadMap"
                             : "");
    }
    // The LEVELS and the WORLDSETTINGS ACTORS, independently of any world.
    //
    // `ULevel::WorldSettings` is a UPROPERTY, so if the AWorldSettings ACTOR is destroyed the
    // GC nulls the reference -- and the coop layer destroys actors by the thousand. That is
    // the one mechanism that could produce this fault while the tree never names either
    // field, so the census has to be able to SEE it: a level whose WorldSettings went null,
    // or a WorldSettings actor that stopped being live.
    const std::vector<void*> levels = R::FindObjectsByClass(L"Level");
    int orphanLevels = 0;
    for (void* l : levels) {
        if (!l) continue;
        void* ws = ReadPtr(l, o.worldSettings);
        const bool alive = R::IsLive(l);
        if (!ws) ++orphanLevels;
        if (!ws || !alive) {
            UE_LOGI("reloadchurn[%s c%d]:   level @%p %s WorldSettings=%p%s",
                    tag, cycle, l, alive ? "LIVE" : "dead", ws,
                    ws ? "" : "   <<< a LEVEL with NO WorldSettings");
        }
    }
    const std::vector<void*> settings = R::FindObjectsByClass(L"WorldSettings");
    size_t wsLive = 0;
    for (void* a : settings) if (a && R::IsLive(a)) ++wsLive;

    // WHAT IS STILL STANDING IN A DEAD WORLD -- the question the world list cannot answer.
    //
    // A dead-but-unpurged world is only interesting if something is HOLDING it, and the
    // holder is an object, not a pointer we can see from here. One walk over the object array
    // per dead world, counting live objects whose world term IS that world and tallying their
    // classes, names the holder by kind. `AddToRoot`ed mirror actors are the standing
    // suspicion (the coop layer GC-pins its runtime pile natives), and they would show up here
    // by the hundred while the solo control shows none.
    if (deadWorld) {
        const int32_t n = R::NumObjects();
        int inDead = 0;
        std::vector<std::pair<std::wstring, int>> byClass;
        for (int32_t i = 0; i < n; ++i) {
            void* o = R::ObjectAt(i);
            if (!o || !R::IsLiveByIndex(o, i)) continue;
            if (ue_wrap::world_identity::WorldOf(o) != deadWorld) continue;
            ++inDead;
            const std::wstring cn = R::ClassNameOf(o);
            bool found = false;
            for (auto& e : byClass) if (e.first == cn) { ++e.second; found = true; break; }
            if (!found && byClass.size() < 512) byClass.emplace_back(cn, 1);
        }
        std::sort(byClass.begin(), byClass.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::wstring top;
        for (size_t i = 0; i < byClass.size() && i < 8; ++i) {
            top += byClass[i].first + L"x" + std::to_wstring(byClass[i].second) + L" ";
        }
        UE_LOGI("reloadchurn[%s c%d]:   DEAD world @%p still holds %d live object(s): %ls",
                tag, cycle, deadWorld, inDead, top.c_str());
    }

    // The headline the scenario greps. A non-zero count here IS the crash condition,
    // observed without having to die of it.
    UE_LOGI("reloadchurn[%s c%d]: VERDICT nullWorldSettings=%d liveWorlds=%zu "
            "levels=%zu(%d with no WorldSettings) worldSettingsActors=%zu/%zu live",
            tag, cycle, nullWs, live, levels.size(), orphanLevels, wsLive, settings.size());
}

void PostCensus(const char* tag, int cycle, const Offsets& o) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, tag, cycle, o] { CensusGT(tag, cycle, o); done->store(1); });
    WaitDone(done, 20000);
}

// WHICH world is current -- through world_identity, NEVER through
// FindObjectByClass(World).
//
// The first version of this probe asked FindObjectByClass and it cost a whole run:
// after the client travelled out, the DEAD Untitled_1 world sat in the object array
// unpurged and FindObjectByClass kept returning it, so the probe reported "never left
// gameplay" for 60 s while the log right beside it showed world_identity moving to the
// menu world in four seconds and the session ending. world_identity.h says this in its
// own header -- FindObjectByClass(WorldClass) is "measured ambiguous" -- and the module
// exists precisely so a world question is not answered by a liveness scan.
const wchar_t* KindName(ue_wrap::world_identity::WorldKind k) {
    using WK = ue_wrap::world_identity::WorldKind;
    return k == WK::Gameplay ? L"Gameplay" : (k == WK::Other ? L"Other" : L"Unknown");
}

// Wait for the world KIND to hold. Unknown is never a match in either direction: it is
// the legitimate ~1 s null window of a travel, and treating it as "left" or "arrived"
// is the bug the enum's third value exists to prevent.
ue_wrap::world_identity::WorldKind WaitForWorldKind(
        ue_wrap::world_identity::WorldKind want, int seconds) {
    using WK = ue_wrap::world_identity::WorldKind;
    WK k = WK::Unknown;
    for (int i = 0; i < seconds; ++i) {
        k = ue_wrap::world_identity::CurrentWorldKind();
        if (k == want) return k;
        ::Sleep(1000);
    }
    return k;
}

// FORCE A COLLECTION, on the game thread.
//
// `[V]` `UKismetSystemLibrary::CollectGarbage` is reflected on this build; in 4.27 its body is
// `GEngine->ForceGarbageCollection(true)`, so it ARMS a collection for the end of the frame
// rather than running one inline -- the caller must give it a frame before censusing.
//
// This is the arm that separates the two live readings of the husk. If a forced collection
// clears the dead world and the rejoin then survives, the fault is that the coop travel leaves
// the collection UNFINISHED and the next LoadMap adopts what it left behind. If the husk
// survives a forced collection, something still REFERENCES it and no amount of collecting will
// help.
bool ForceGcGT() {
    void* cdo = R::FindClassDefaultObject(L"KismetSystemLibrary");
    if (!cdo) { UE_LOGW("reloadchurn: KismetSystemLibrary CDO not resolved"); return false; }
    void* fn = R::FindFunction(R::ClassOf(cdo), L"CollectGarbage");
    if (!fn) { UE_LOGW("reloadchurn: CollectGarbage not resolved"); return false; }
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(cdo, f);
}

// VOTV's own travel verb, called inline on the game thread. Same primitive the
// menu-travel probe found and the death arc ships on -- a bare engine "open" does not
// travel here.
bool TransitionToMenuGT() {
    void* gm = R::FindObjectByClass(P::name::GamemodeClass);
    if (!gm || !R::IsLive(gm)) { UE_LOGW("reloadchurn: no live mainGamemode_C"); return false; }
    void* fn = R::FindFunction(R::ClassOf(gm), L"transition");
    if (!fn) { UE_LOGW("reloadchurn: mainGamemode_C::transition not resolved"); return false; }
    R::FName ln = ue_wrap::fname_utils::StringToFName(L"/Game/menu");
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !f.SetRaw(L"LevelName", &ln, sizeof(ln))) return false;
    return ue_wrap::Call(gm, f);
}

// The address of the live UWorld, as an identity. The probe uses it to prove a re-load
// ACTUALLY HAPPENED rather than trusting a return value: `LoadStorySave` answers false
// when it DEFERS (it re-issues `open untitled_1` from the menu and completes a second
// later), so its bool cannot distinguish "refused" from "queued" -- and a scenario that
// scored itself on that bool would have called a real load a failure, and a load that
// never ran a success. A world POINTER that changed is the event itself.
void* WorldPtrGT() { return ue_wrap::world_identity::CurrentWorld(); }

void RunProbe() {
    const int cycles = EnvInt("VOTVCOOP_RELOAD_CYCLES", 3);
    const int dwellS = EnvInt("VOTVCOOP_RELOAD_DWELL_S", 15);
    const int menuS  = EnvInt("VOTVCOOP_RELOAD_MENU_S", 12);
    // WHICH save to re-load. mp.py hands the host its slot in VOTVCOOP_SAVE; a fresh
    // peer has none and re-loads by starting a New Game, which travels through exactly
    // the same LoadMap -- the map load is the subject, not the save's contents.
    const std::string  slot = coop::config::ReadEnv("VOTVCOOP_SAVE");
    const std::wstring wslot(slot.begin(), slot.end());

    // THE COOP ARM. The solo control above re-loads the map by itself; a CLIENT
    // re-loads it by REJOINING, which is the reported flow and drags the whole join
    // fan-out (save transfer, mirror spawn, sweeps, roster teardown) through the same
    // LoadMap. The two arms differ in exactly that, which is the point.
    const bool  rejoin = coop::config::ReadEnv("VOTVCOOP_RELOAD_REJOIN") == "1";
    const std::string peer = coop::config::ReadEnv("VOTVCOOP_NET_PEER");
    const std::string port = coop::config::ReadEnv("VOTVCOOP_NET_PORT");
    const std::string addr = peer.empty() ? std::string() : (peer + ":" + (port.empty() ? "47621" : port));
    if (rejoin && addr.empty()) {
        UE_LOGW("reloadchurn: REJOIN arm asked for but VOTVCOOP_NET_PEER is unset -- abort");
        UE_LOGI("reloadchurn: DONE");
        ue_wrap::log::Flush();
        return;
    }

    const std::string arm = rejoin ? ("REJOIN -> " + addr)
                                   : (slot.empty() ? std::string("SOLO <fresh new game>")
                                                   : ("SOLO save '" + slot + "'"));
    UE_LOGI("reloadchurn: === RE-LOAD CHURN probe START (cycles=%d dwell=%ds menu=%ds arm=%s) ===",
            cycles, dwellS, menuS, arm.c_str());

    Offsets offs;
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        auto out  = std::make_shared<Offsets>();
        GT::Post([done, out] { *out = ResolveOffsetsGT(); done->store(1); });
        WaitDone(done, 20000);
        offs = *out;
    }
    UE_LOGI("reloadchurn: offsets PersistentLevel=+0x%X WorldSettings=+0x%X DefaultGameMode=+0x%X",
            offs.persistentLevel, offs.worldSettings, offs.defaultGameMode);

    if (rejoin) {
        // A client only reaches gameplay THROUGH the join (the world it plays is the
        // host's transferred save), so waiting for the world without waiting for the
        // session would pass on a peer that never connected.
        bool up = false;
        for (int i = 0; i < 240; ++i) {
            if (harness::session_runtime::Session().running()) { up = true; break; }
            ::Sleep(1000);
        }
        if (!up) {
            UE_LOGW("reloadchurn: REJOIN arm -- no running session after 240 s, abort");
            UE_LOGI("reloadchurn: DONE");
            ue_wrap::log::Flush();
            return;
        }
        UE_LOGI("reloadchurn: REJOIN arm -- session LIVE, waiting for the transferred world");
    }

    using WK = ue_wrap::world_identity::WorldKind;
    WK w = WaitForWorldKind(WK::Gameplay, 240);
    if (w != WK::Gameplay) {
        UE_LOGW("reloadchurn: never reached gameplay (worldKind=%ls) -- abort", KindName(w));
        UE_LOGI("reloadchurn: DONE");
        ue_wrap::log::Flush();
        return;
    }

    for (int c = 1; c <= cycles; ++c) {
        UE_LOGI("reloadchurn: ---- cycle %d/%d: in gameplay (world @%p) ----",
                c, cycles, ue_wrap::world_identity::CurrentWorld());
        PostCensus("gameplay", c, offs);
        ::Sleep(static_cast<DWORD>(dwellS) * 1000);
        PostCensus("pre-travel", c, offs);

        // Exit to the menu with the layer LIVE. This is the player's own in-game exit,
        // not the death flee: no transparent bypass, because the bypass keeps our layer
        // dormant through the teardown and that is precisely the state the report was
        // NOT made in.
        {
            auto done = std::make_shared<std::atomic<int>>(0);
            auto ok   = std::make_shared<int>(0);
            GT::Post([done, ok] { if (TransitionToMenuGT()) *ok = 1; done->store(1); });
            WaitDone(done, 8000);
            UE_LOGI("reloadchurn: cycle %d transition(/Game/menu) dispatched=%d", c, *ok);
        }

        const WK m = WaitForWorldKind(WK::Other, 90);
        if (m != WK::Other) {
            UE_LOGW("reloadchurn: cycle %d never left gameplay (worldKind=%ls) -- abort",
                    c, KindName(m));
            break;
        }
        UE_LOGI("reloadchurn: cycle %d at the menu (world @%p)", c, ue_wrap::world_identity::CurrentWorld());

        // THE DECISIVE FRAME. If an Untitled_1 UWorld is still resident here with a null
        // WorldSettings, the next load faults before it renders anything -- and this
        // line says so while the process is still alive to print it.
        PostCensus("menu", c, offs);
        // Sampled ACROSS the dwell, not just at its ends. The solo control purges the old
        // gameplay world somewhere between +2 s and +14 s at the menu while the coop arm still
        // has it; whether that husk EVER goes away is the question that separates "we re-load
        // before the engine finished" from "something is holding it", and two samples cannot
        // answer it.
        for (int t = 0; t < menuS; t += 10) {
            ::Sleep(static_cast<DWORD>(menuS - t < 10 ? menuS - t : 10) * 1000);
            PostCensus("menu-dwell", c, offs);
        }
        PostCensus("menu-settled", c, offs);

        // The GC ARM. Off by default: the run that has to reproduce the field crash must not
        // also be the run that tries to prevent it.
        if (coop::config::ReadEnv("VOTVCOOP_RELOAD_GC") == "1") {
            auto done = std::make_shared<std::atomic<int>>(0);
            auto ok   = std::make_shared<int>(0);
            GT::Post([done, ok] { if (ForceGcGT()) *ok = 1; done->store(1); });
            WaitDone(done, 8000);
            UE_LOGI("reloadchurn: cycle %d forced CollectGarbage dispatched=%d", c, *ok);
            ::Sleep(4000);   // ForceGarbageCollection arms the collection for end-of-frame
            PostCensus("post-gc", c, offs);
        }

        const void* menuWorld = WorldPtrGT();
        UE_LOGI("reloadchurn: cycle %d RE-LOADING the world (%s) -- menu world @%p",
                c, rejoin ? "by REJOINING the host" : "from the save", menuWorld);
        ue_wrap::log::Flush();   // survive a hard fault: the next line may never be written
        if (rejoin) {
            // ConnectDirect queues the start; the harness's own tick consumes it and runs
            // the menu-mode save-transfer bootstrap -- i.e. the whole join, exactly as the
            // browser's Connect does. Called off the game thread on purpose: it only
            // touches queue state, and the session start happens on the timeline thread.
            const bool accepted = coop::session_manager::ConnectDirect(addr);
            UE_LOGI("reloadchurn: cycle %d ConnectDirect('%s') accepted=%d",
                    c, addr.c_str(), accepted ? 1 : 0);
            // Census REPEATEDLY through the join, each one FLUSHED. The fatal LoadMap runs on
            // the game thread inside the join, so no posted task can observe it from the
            // inside -- the best available evidence is the last census that reached disk
            // before the process stopped existing. Buffered INFO would be lost with it.
            for (int i = 0; i < 30; ++i) {
                if (ue_wrap::world_identity::CurrentWorldKind() == WK::Gameplay) break;
                PostCensus("joining", c, offs);
                ue_wrap::log::Flush();
                ::Sleep(2000);
            }
        } else {
            auto done = std::make_shared<std::atomic<int>>(0);
            auto ok   = std::make_shared<int>(0);
            GT::Post([done, ok, wslot] {
                E::ResetCachedSave();
                *ok = wslot.empty() ? (E::StartFreshGame(true) ? 1 : 0)
                                    : (E::LoadStorySave(wslot.c_str()) ? 1 : 0);
                done->store(1);
            });
            WaitDone(done, 30000);
            // NOT a verdict term: LoadStorySave answers false when it DEFERS (it re-issues
            // `open untitled_1` from the menu and lands a second later), so this bool cannot
            // tell "refused" from "queued". The world-pointer change below is the evidence.
            UE_LOGI("reloadchurn: cycle %d LoadStorySave returned %d (false == deferred, not failed)",
                    c, *ok);
        }

        w = WaitForWorldKind(WK::Gameplay, rejoin ? 240 : 120);
        if (w != WK::Gameplay) {
            UE_LOGW("reloadchurn: cycle %d re-load never reached gameplay (worldKind=%ls)",
                    c, KindName(w));
            PostCensus("reload-stuck", c, offs);
            break;
        }
        // The re-load is only PROVEN by a different UWorld: reaching a world named
        // Untitled_1 proves nothing on its own, because that is also what "the travel
        // never happened" looks like.
        const void* newWorld = WorldPtrGT();
        if (newWorld == menuWorld) {
            UE_LOGW("reloadchurn: cycle %d world pointer UNCHANGED (@%p) -- no load actually ran",
                    c, newWorld);
            break;
        }
        UE_LOGI("reloadchurn: cycle %d SURVIVED the re-load (world @%p, menu world was @%p)",
                c, newWorld, menuWorld);
        PostCensus("reloaded", c, offs);
        ue_wrap::log::Flush();
    }

    UE_LOGI("reloadchurn: DONE");
    ue_wrap::log::Flush();
}

}  // namespace

void RunReloadChurnProbe() { RunProbe(); }
DWORD WINAPI ReloadChurnProbeThread(LPVOID) { RunReloadChurnProbe(); return 0; }

}  // namespace harness::autotest
