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
#include <cwchar>
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

// ---------------------------------------------------------------------------------
// WHERE IN ITS DESTRUCTION a UObject is standing -- the term that separates the two
// readings of "dead but never purged".
//
// `IsLive` answers one bit and that bit is exhausted here: every candidate world is
// already "dead". UE4 tears an object down in two GC phases, and each leaves its own
// mark:
//
//   phase 1  ConditionalBeginDestroy() -> sets RF_BeginDestroyed, calls BeginDestroy(),
//            and pushes the object onto the pending-destruction list.
//   phase 2  for each pending object, IsReadyForFinishDestroy() is polled; only when it
//            answers true does ConditionalFinishDestroy() run (RF_FinishDestroyed) and
//            the slot get released.
//
// So the husk's flags decide the question outright, with no further instrumentation:
//
//   RF_BeginDestroyed set, RF_FinishDestroyed clear  -> phase 1 ran, phase 2 is STUCK.
//                                                       IsReadyForFinishDestroy keeps
//                                                       answering false. (reading 2)
//   neither set                                      -> GC never reached it at all, so
//                                                       something REFERENCES it and it is
//                                                       not being destroyed. (reading 1)
//
// Unreachable/PendingKill come from the GUObjectArray slot (EInternalObjectFlags) and
// corroborate: PendingKill alone is "marked, GC has not run on it yet"; Unreachable is
// "GC has claimed it".
// ---------------------------------------------------------------------------------
std::wstring DestroyStage(void* obj) {
    if (!obj) return L"<null>";
    const int32_t objFlags = *reinterpret_cast<int32_t*>(
        static_cast<char*>(obj) + P::off::UObject_ObjectFlags);
    const int32_t intFlags = R::InternalFlagsOf(obj);
    std::wstring s;
    auto bit = [&s](int32_t have, int32_t mask, const wchar_t* name) {
        if (have & mask) { if (!s.empty()) s += L'|'; s += name; }
    };
    bit(intFlags, 0x20000000, L"PendingKill");
    bit(intFlags, 0x10000000, L"Unreachable");
    bit(intFlags, 0x40000000, L"RootSet");
    bit(intFlags, 0x01000000, L"ClusterRoot");
    bit(intFlags, 0x00800000, L"ReachableInCluster");
    bit(objFlags, 0x00008000, L"RF_BeginDestroyed");
    bit(objFlags, 0x00010000, L"RF_FinishDestroyed");
    if (s.empty()) s = L"<none>";
    wchar_t raw[64];
    swprintf(raw, 64, L" (rf=%08X int=%08X)", static_cast<unsigned>(objFlags),
             static_cast<unsigned>(intFlags));
    return s + raw;
}

// True iff the object is sitting in GC phase 2 -- begun and not finished. This is the
// crash's precondition stated positively, and the scenario greps for it.
bool IsStuckInFinishDestroy(void* obj) {
    if (!obj) return false;
    const int32_t f = *reinterpret_cast<int32_t*>(
        static_cast<char*>(obj) + P::off::UObject_ObjectFlags);
    return (f & 0x00008000) != 0 && (f & 0x00010000) == 0;
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
    int stuck = 0;
    size_t live = 0;
    // EVERY dead world and level, not just the last one seen. The first version kept a single
    // `deadWorld` and graded on it, which meant a rooted holder of a DIFFERENT dead world
    // reported zero and passed -- and reload churn is precisely what produces two dead worlds
    // at once. A gate that can step over the bug it was written for is worse than none.
    struct Terminal { void* obj; bool isWorld; };
    std::vector<Terminal> deadTerminals;
    int rootedWorst = 0;
    UE_LOGI("reloadchurn[%s c%d]: %zu UWorld object(s) in the array", tag, cycle, worlds.size());
    for (void* w : worlds) {
        if (!w) continue;
        // DEAD worlds are printed too, not skipped. The first version skipped them and that
        // hid the whole question: `open untitled_1` a second time in one process REUSES the
        // still-resident map package, so the world/level whose WorldSettings the next LoadMap
        // reads may be one that is already kill-flagged at the menu. A census that only
        // reports what is live cannot see a reused corpse.
        const bool alive = R::IsLive(w);
        if (alive) ++live; else deadTerminals.push_back({w, true});
        const std::wstring wname = R::ToString(R::NameOf(w));
        void* lvl = ReadPtr(w, o.persistentLevel);
        void* ws  = ReadPtr(lvl, o.worldSettings);
        void* gm  = ReadPtr(ws, o.defaultGameMode);
        std::wstring lname = L"<none>";
        if (lvl && R::IsLive(lvl)) lname = R::ToString(R::NameOf(lvl));
        if (alive && lvl && !ws) ++nullWs;
        UE_LOGI("reloadchurn[%s c%d]:   world='%ls' @%p %s stage=%ls PersistentLevel=%p ('%ls')  "
                "WorldSettings=%p  DefaultGameMode=%p%s",
                tag, cycle, wname.c_str(), w, alive ? "LIVE" : "dead", DestroyStage(w).c_str(),
                lvl, lname.c_str(), ws, gm,
                (lvl && !ws) ? "   <<< WORLDSETTINGS IS NULL -- this world would CRASH LoadMap"
                             : "");
        if (!alive) stuck += IsStuckInFinishDestroy(w) ? 1 : 0;
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
        if (!alive) deadTerminals.push_back({l, false});
        if (!ws) ++orphanLevels;
        if (!ws || !alive) {
            UE_LOGI("reloadchurn[%s c%d]:   level @%p %s stage=%ls WorldSettings=%p%s",
                    tag, cycle, l, alive ? "LIVE" : "dead", DestroyStage(l).c_str(), ws,
                    ws ? "" : "   <<< a LEVEL with NO WorldSettings");
        }
    }
    const std::vector<void*> settings = R::FindObjectsByClass(L"WorldSettings");
    size_t wsLive = 0;
    for (void* a : settings) if (a && R::IsLive(a)) ++wsLive;

    // WHO STILL REACHES THE DEAD WORLD -- by the OUTER CHAIN, not by the world term.
    //
    // The previous version of this census asked `world_identity::WorldOf(o) == deadWorld`
    // and reported 0 at every frame. That 0 was never a finding: WorldOf resolves an
    // object's world by walking to a ULevel and reading OwningWorld, and the husk's
    // PersistentLevel is already null, so nothing can resolve to it BY CONSTRUCTION. It
    // was an instrument blind to its own subject, and it is retired here rather than kept
    // beside the working one.
    //
    // The right question follows from what the flags say. `[V]` the husk is PendingKill
    // and NOTHING else -- not Unreachable, no RF_BeginDestroyed -- so GC has never claimed
    // it: on every pass it comes out REACHABLE. UE4 nulls a strong UPROPERTY reference to
    // a PendingKill object during collection, but it does NOT eliminate the structural
    // references every UObject carries (Outer, Class, Name), so the surviving path is an
    // OUTER CHAIN from something the GC is required to keep: a ROOT-SET object. Our own
    // layer GC-pins runtime spawns in five places (`R::AddToRoot`), and an actor spawned
    // into the gameplay world is outered to that world's ULevel.
    //
    // So: walk the whole array, follow each object's Outer chain, and name every object
    // whose chain passes through the dead world or its dead level -- plus, independently,
    // every RootSet object, since that is the set GC starts from.
    if (!deadTerminals.empty()) {
        const int32_t n = R::NumObjects();
        std::vector<int> reached(deadTerminals.size(), 0);
        std::vector<int> rootedReached(deadTerminals.size(), 0);
        std::vector<std::vector<std::pair<std::wstring, int>>> byClass(deadTerminals.size());
        int rooted = 0, printed = 0;
        for (int32_t i = 0; i < n; ++i) {
            void* obj = R::ObjectAt(i);
            if (!obj) continue;
            bool isTerminal = false;
            for (const auto& t : deadTerminals) if (t.obj == obj) { isTerminal = true; break; }
            if (isTerminal) continue;
            // Bounded Outer walk. A PendingKill object is marked but NOT yet freed, so its
            // memory is still mapped and this read is safe -- the same reason the world lines
            // above can read a dead world's fields. The whole census runs inside a posted
            // game-thread task, whose SEH wrapper absorbs a fault on a slot caught mid-purge.
            void* chain[24] = {};
            int depth = 0;
            int hit = -1;
            for (void* o = R::OuterOf(obj); o && depth < 24; o = R::OuterOf(o)) {
                chain[depth++] = o;
                for (size_t t = 0; t < deadTerminals.size(); ++t)
                    if (deadTerminals[t].obj == o) { hit = static_cast<int>(t); break; }
                if (hit >= 0) break;
            }
            const bool isRoot = (R::InternalFlagsOf(obj) & 0x40000000) != 0;
            if (isRoot) ++rooted;
            if (hit < 0) continue;
            ++reached[hit];
            if (isRoot) ++rootedReached[hit];
            const std::wstring cn = R::ClassNameOf(obj);
            auto& tally = byClass[hit];
            bool found = false;
            for (auto& e : tally) if (e.first == cn) { ++e.second; found = true; break; }
            if (!found && tally.size() < 512) tally.emplace_back(cn, 1);
            // Print the ROOTED ones in full -- those are the candidate holders, and there
            // should be none. A non-rooted object in the chain is a passenger, not a cause.
            if (isRoot && printed < 24) {
                ++printed;
                std::wstring path;
                for (int d = 0; d < depth; ++d) {
                    path += L" -> ";
                    path += R::ToString(R::NameOf(chain[d]));
                }
                UE_LOGW("reloadchurn[%s c%d]:   HOLDER (RootSet) '%ls' class=%ls @%p%ls",
                        tag, cycle, R::ToString(R::NameOf(obj)).c_str(), cn.c_str(), obj,
                        path.c_str());
            }
        }
        for (size_t t = 0; t < deadTerminals.size(); ++t) {
            auto& tally = byClass[t];
            std::sort(tally.begin(), tally.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            std::wstring top;
            for (size_t i = 0; i < tally.size() && i < 8; ++i)
                top += tally[i].first + L"x" + std::to_wstring(tally[i].second) + L" ";
            // One line PER dead object, each carrying its own RootSet count, so the harness
            // grades the worst of them rather than whichever happened to be last.
            UE_LOGI("reloadchurn[%s c%d]:   DEAD %ls @%p reached by %d object(s) via Outer "
                    "(%d of them RootSet); RootSet objects in array=%d; top: %ls",
                    tag, cycle, deadTerminals[t].isWorld ? L"world" : L"level",
                    deadTerminals[t].obj, reached[t], rootedReached[t], rooted, top.c_str());
            if (rootedReached[t] > rootedWorst) rootedWorst = rootedReached[t];
        }
    }

    // The headline the scenario greps. A non-zero count here IS the crash condition,
    // observed without having to die of it.
    UE_LOGI("reloadchurn[%s c%d]: VERDICT nullWorldSettings=%d liveWorlds=%zu "
            "stuckInFinishDestroy=%d rootedHoldersOfDead=%d "
            "levels=%zu(%d with no WorldSettings) worldSettingsActors=%zu/%zu live",
            tag, cycle, nullWs, live, stuck, rootedWorst, levels.size(), orphanLevels, wsLive,
            settings.size());
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
