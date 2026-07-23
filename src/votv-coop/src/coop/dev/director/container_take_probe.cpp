// coop/dev/director/container_take_probe.cpp -- the container-take INPUT PROBE (director Phase-2
// HALT gate). Decides whether the CONTAINER concurrent-take race (the user's literal ask, "два
// пира ... ОДНОВРЕМЕННО берут предмет X") is buildable on the director's reflected-verb model.
//
// DESIGN of record: research/findings/tooling/votv-baritone-analog-autonomous-director-DESIGN-
// 2026-07-23.md + the Phase-2 /qf thread (5 rounds, converged 2026-07-23). Why a probe first:
//   - takeObj/addObject are 0x45 EX_LocalVirtualFunction INTERCEPTION verbs (container_contents_
//     sync.cpp) -- NOT reflection-callable; they only fire when the BP VM dispatches them.
//   - BUT the take is drivable one layer UP by CALLABLE BP verbs (measured, the CXXHeaderDump):
//     prop_container::openContainer() opens the UI; a container slot's
//     uicomp_playerInvContainerSlot::pressButton() sets ui_playerInventory.selected;
//     ui_playerInventory::em_take() takes the selected item. These are plain BP functions (the
//     BndEvt__..OnButtonClickedEvent twins are the inert delegates), the SAME model already
//     proven for door_C::doorOpen -- so the take is likely drivable at the human-INPUT seam.
//   - The ONE remaining unknown is answerable only by running code: does em_take/pressButton
//     EXECUTE its body when CallFunction'd, or is it reflection-inert like InpActEvt_use? The
//     probe NEVER infers "callable => ran" (the door/InpActEvt lesson); it MEASURES the body ran
//     via the container's GObjStack item-count DECREMENT (the take removed an item).
//
// The probe is an honest LADDER: it reports the highest rung reached, so a failure names WHICH
// rung (open / select / take) went inert rather than a false green. extract(int32 Index) is a
// NON-FAITHFUL diagnostic fallback (the effect seam, one layer below the human UI path -- it may
// bypass the selection state the real race traverses, a B4 spine deviation) run only to prove the
// mechanism CAN be driven at all when the faithful path is inert.
//
// DEV-ONLY (RULE 3), solo, env-gated (VOTVCOOP_RUN_CTAKE_PROBE=1). Non-destructive intent: one
// take from a world container (the game corrects it; a solo probe has no peer to diverge). Its
// verdict + the remaining unbuilt piece (a whole-GObjStack no-dup verifier) go to the USER as the
// container-race vs generic-prop-race go/no-go. Greppable "director/ctake: VERDICT".

#include "coop/dev/director/director.h"
#include "coop/dev/director/dup_verifier.h"   // the no-dup verifier + its positive control

#include "coop/config/config.h"               // ReadEnv (the codebase's env reader)
#include "coop/player/players_registry.h"
#include "coop/props/prop_element_tracker.h"  // CollectKeyIndexEntries -- the stable save-key index
#include "ue_wrap/actors/inventory.h"      // ResolveSaveSlot
#include "ue_wrap/actors/prop.h"           // WalksToBase
#include "ue_wrap/actors/save_record.h"    // ReadArr, kMxStride
#include "ue_wrap/core/call.h"             // ParamFrame, Call
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

namespace coop::director {
namespace {
namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace GT  = ue_wrap::game_thread;
namespace SR  = ue_wrap::save_record;
namespace INV = ue_wrap::inventory;
namespace PR  = ue_wrap::prop;
namespace PT  = coop::prop_element_tracker;

template <class Fn>
int RunGT(Fn&& body) {   // bounded: a stalled game thread returns 0 instead of hanging forever
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, body]() mutable { body(*done); });
    int waited = 0;
    while (done->load() == 0) { ::Sleep(5); waited += 5; if (waited >= 4000) return 0; }
    return done->load();
}

float HorizDist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y;
    return std::sqrt(dx * dx + dy * dy);
}

// ---- container reflection helpers (self-contained; a dev instrument) -----------------------
void* ContainerClass() {
    static void* cls = nullptr;
    if (!cls) cls = R::FindClass(L"prop_container_C");
    return cls;
}
bool IsPlacedContainer(void* o) {
    void* base = ContainerClass();
    if (!base || !o || !R::IsLive(o)) return false;
    if (R::NameStartsWith(R::NameOf(o), L"Default__")) return false;
    return PR::WalksToBase(R::ClassOf(o), base);
}
// The propInventory component of a container actor (or null).
void* InventoryOf(void* container) {
    static int32_t off = -2;
    if (off == -2) off = R::FindPropertyOffset(R::ClassOf(container), L"propInventory");
    if (off < 0) return nullptr;
    void* inv = nullptr;
    std::memcpy(&inv, reinterpret_cast<const uint8_t*>(container) + off, sizeof(inv));
    return (inv && R::IsLive(inv)) ? inv : nullptr;
}
// A WORLD container inventory (Player==0), never a personal one (which shares the global GObjStack).
bool IsWorldContainerInv(void* inv) {
    static int32_t off = -2;
    if (off == -2) off = inv ? R::FindPropertyOffset(R::ClassOf(inv), L"Player") : -1;
    if (off < 0) return false;
    uint8_t player = 1;
    std::memcpy(&player, reinterpret_cast<const uint8_t*>(inv) + off, 1);
    return player == 0;
}
// The container's item count via its GObjStack slice (the SAME read container_contents_sync uses:
// saveSlot.GObjStack[ inv.Index ] is a Fstruct_mObject wrapping a TArray<Fstruct_save> @ +0).
// Returns -1 if unresolvable (never-initialised index / no saveSlot) -- distinct from 0 (empty).
int32_t ContainerItemCount(void* container) {
    void* inv = InventoryOf(container);
    if (!inv || !IsWorldContainerInv(inv)) return -1;
    void* save = INV::ResolveSaveSlot();
    if (!save) return -1;
    static int32_t offStack = -2, offIndex = -2;
    if (offStack == -2) offStack = R::FindPropertyOffset(R::ClassOf(save), L"GObjStack");
    if (offIndex == -2) offIndex = R::FindPropertyOffset(R::ClassOf(inv), L"Index");
    if (offStack < 0 || offIndex < 0) return -1;
    int32_t idx = -1;
    std::memcpy(&idx, reinterpret_cast<const uint8_t*>(inv) + offIndex, sizeof(idx));
    if (idx < 0) return -1;
    const SR::Arr stack = SR::ReadArr(save, offStack);
    if (idx >= stack.num) return -1;
    const uint8_t* slot = stack.data + static_cast<size_t>(idx) * SR::kMxStride;
    return SR::ReadArr(slot, 0).num;
}

// Call a no-arg UFunction `fnName` on `obj` (declaring class = obj's class or an ancestor via
// FindFunction's exact-owner resolution -- these verbs are declared on the leaf BP class).
bool CallNoArg(void* obj, void* cls, const wchar_t* fnName) {
    void* fn = cls ? R::FindFunction(cls, fnName) : nullptr;
    if (!fn) { UE_LOGW("director/ctake: verb %ls NOT FOUND on the class -- cannot drive", fnName); return false; }
    ue_wrap::ParamFrame pf(fn);
    if (!pf.valid()) return false;
    return ue_wrap::Call(obj, pf);
}

// The first live, non-Default instance whose class walks to `className` (post-openContainer widget
// discovery). One GUObjectArray pass -- only run on the take rung (not per frame). null if none.
void* FirstLiveOfClass(const wchar_t* className) {
    void* base = R::FindClass(className);
    if (!base) return nullptr;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o)) continue;
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
        if (PR::WalksToBase(R::ClassOf(o), base)) return o;
    }
    return nullptr;
}

int32_t ReadInt32Field(void* obj, const wchar_t* field) {
    if (!obj) return -0x7fffffff;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), field);
    if (off < 0) return -0x7fffffff;
    int32_t v = 0;
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(obj) + off, sizeof(v));
    return v;
}

constexpr float kMinCm      = 150.f;
constexpr float kMaxCm      = 6000.f;
constexpr float kReachCm    = 250.f;   // a container's interaction reach (not a nav constant)
constexpr int   kMaxCand    = 12;
constexpr int   kUiWaitTicks = 30;     // ~poll after openContainer for the UI widget to spawn

// Drive the faithful take chain on the game thread; fills the measurements. Returns via `d`.
struct Probe {
    void*    container = nullptr;
    std::wstring fname;
    int32_t  countBefore  = -1;
    bool     openCalled   = false;
    bool     uiOpened     = false;   // a live ui_playerInventory_C appeared after openContainer
    bool     slotFound    = false;   // ui.slots_prop[0] -- the UI's OWN bound container slot
    int32_t  slotId       = -1;      // the bound slot's ID (its container index -> which item)
    bool     pressCalled  = false;
    int32_t  countAfterPress = -1;   // count after ONLY the faithful pressButton (attribution-clean)
    bool     extractCalled = false;
    int32_t  countAfterExtract = -1;
    // The no-dup verifier's POSITIVE CONTROL: X captured from the taken slot, its global instance count
    // BEFORE the take (phaseA, X in the container) and AFTER a solo take (phaseB, X moved to the player).
    // A dup is impossible in a solo run, so both MUST be 1 -- else count==1 on a real race is ambiguous
    // (no-dup vs instrument-blind). This run IS the verifier's control.
    ItemSig  xSig;
    int32_t  phaseA = -1;
    int32_t  phaseB = -1;
    std::wstring targetKey;   // the shared container's stable SAVE KEY (the by-construction race key)
};

}  // namespace

void RunContainerTakeProbe() {
    UE_LOGI("director/ctake: container-take input probe -- +20 s settle for the world to load");
    ::Sleep(20000);

    // Resolve a possessed local player (the body that walks).
    struct Rsv { void* player = nullptr; };
    auto rsv = std::make_shared<Rsv>();
    for (int waited = 0; waited < 60 && !rsv->player; ++waited) {
        const int r = RunGT([rsv](std::atomic<int>& d) {
            void* p = coop::players::Registry::Get().Local();
            if (p && R::IsLive(p) && E::GetController(p)) { rsv->player = p; d.store(1); }
            else d.store(2);
        });
        if (r == 1) break;
        ::Sleep(1000);
    }
    if (!rsv->player) { UE_LOGW("director/ctake: VERDICT no possessed local player -- ABORT"); return; }

    // Pick a placed, non-empty, nav-reachable world container (shortest reachable route).
    DirectorGoal goal;
    goal.reachCm = kReachCm;
    auto pb = std::make_shared<Probe>();
    const int pick = RunGT([rsv, &goal, pb](std::atomic<int>& d) {
        const ue_wrap::FVector at = E::GetActorLocation(rsv->player);
        struct Cand { void* c; ue_wrap::FVector pos; int32_t count; };
        std::vector<Cand> cands;
        const int32_t n = R::NumObjects();
        for (int32_t i = 0; i < n; ++i) {
            void* o = R::ObjectAt(i);
            if (!IsPlacedContainer(o)) continue;
            const int32_t cnt = ContainerItemCount(o);
            if (cnt <= 0) continue;                       // need a NON-EMPTY container to take from
            const ue_wrap::FVector p = E::GetActorLocation(o);
            const float dist = HorizDist(p, at);
            if (dist < kMinCm || dist > kMaxCm) continue;
            cands.push_back({ o, p, cnt });
        }
        if (cands.empty()) {
            UE_LOGW("director/ctake: NO placed non-empty world container in the %.0f-%.0fcm band -- "
                    "put an item in a world container near the player (or spawn one)", kMinCm, kMaxCm);
            d.store(2); return;
        }
        std::sort(cands.begin(), cands.end(),
                  [&](const Cand& a, const Cand& b){ return HorizDist(a.pos, at) < HorizDist(b.pos, at); });
        if (cands.size() > static_cast<size_t>(kMaxCand)) cands.resize(kMaxCand);
        float bestLen = 1e30f;
        for (const Cand& c : cands) {
            std::vector<ue_wrap::FVector> path;
            if (!E::FindNavPath(rsv->player, at, c.pos, path)) continue;   // unreachable
            if (HorizDist(path.back(), c.pos) > goal.reachCm) continue;    // route ends short of reach
            float len = 0.f;
            for (size_t i = 1; i < path.size(); ++i) len += HorizDist(path[i - 1], path[i]);
            if (len < bestLen) { bestLen = len; goal.targetActor = c.c; goal.targetPos = c.pos;
                                 pb->container = c.c; pb->countBefore = c.count; }
        }
        if (!goal.targetActor) {
            UE_LOGW("director/ctake: no nav-reachable non-empty container (all off-mesh / behind locked doors) -- ABORT");
            d.store(2); return;
        }
        pb->fname = R::ToString(R::NameOf(goal.targetActor));
        UE_LOGI("director/ctake: target container=%p fname=%ls items=%d pos=(%.0f,%.0f,%.0f) routeLen=%.0fcm",
                goal.targetActor, pb->fname.c_str(), pb->countBefore,
                goal.targetPos.X, goal.targetPos.Y, goal.targetPos.Z, bestLen);
        d.store(1);
    });
    if (pick != 1) { UE_LOGW("director/ctake: VERDICT could not pick a container -- ABORT"); return; }

    // Walk to it with the brain (ClearHand > Goto > Reach).
    {
        ControlManager mgr;
        AddWalkToProcesses(mgr, goal);
        mgr.Run(goal, /*maxSeconds=*/45);
        if (!goal.reached) {
            UE_LOGW("director/ctake: VERDICT walk did NOT reach the container (reason=%s) -- ABORT "
                    "(container=%ls)", goal.failReason, pb->fname.c_str());
            return;
        }
    }

    // The take LADDER: each rung runtime-verified; report the highest rung + the count delta.
    RunGT([rsv, &goal, pb](std::atomic<int>& d) {
        // openContainer/extract are declared on the BASE prop_container_C, but the target's leaf class
        // is a subclass (e.g. prop_container_fileCabs_C). FindFunction is exact-owner (it does NOT walk
        // the superclass chain -- lesson-findfunction-does-not-walk-the-superclass-chain), so resolve on
        // the DECLARING class, never the instance's leaf class (measured 2026-07-23: leaf resolution
        // returned null -> a false NOT-DRIVABLE). Dispatch still runs ON the instance via ProcessEvent.
        void* contBaseCls = ContainerClass();   // prop_container_C -- where openContainer/extract live
        // Re-read the count at the container NOW (the walk could have jostled nothing, but read fresh).
        pb->countBefore = ContainerItemCount(pb->container);
        // Aim at the container (the interaction reads the player's look target).
        E::WriteMainPlayerLookAtActor(rsv->player, pb->container);
        // RUNG 1: openContainer() -- open the UI a human's interact opens.
        pb->openCalled = CallNoArg(pb->container, contBaseCls, L"openContainer");
        UE_LOGI("director/ctake: RUNG1 openContainer call=%d (countBefore=%d)", pb->openCalled ? 1 : 0, pb->countBefore);
        d.store(1);
    });
    // Let the UI widget spawn. The CONTAINER SLOT (uicomp_playerInvContainerSlot_C) is the real
    // "a container UI is open" signal -- a bare ui_playerInventory_C can pre-exist (a closed/pooled
    // widget), so waiting on that alone false-positives (measured 2026-07-23). Wait for the container
    // slot; record the ui presence separately.
    for (int i = 0; i < kUiWaitTicks && !pb->slotFound; ++i) {
        RunGT([pb](std::atomic<int>& d) {
            pb->uiOpened  = (FirstLiveOfClass(L"ui_playerInventory_C") != nullptr);
            pb->slotFound = (FirstLiveOfClass(L"uicomp_playerInvContainerSlot_C") != nullptr);
            d.store(1);
        });
        if (!pb->slotFound) ::Sleep(30);
    }
    UE_LOGI("director/ctake: RUNG2 container UI: ui_playerInventory=%d containerSlot=%d",
            pb->uiOpened ? 1 : 0, pb->slotFound ? 1 : 0);

    // RUNG 3: the FAITHFUL take, ISOLATED to pressButton so the delta is ATTRIBUTED. The container-slot
    // CLICK handler is pressButton on the slot; its bytecode (kismet-analyzer 2026-07-23) calls
    // setHoverContainerSlot(self) on its Owner UI + references IsHovered -- so the take is keyed on WHICH
    // slot the UI considers HOVERED, and it must be the UI's OWN bound slot (ui.slots_prop[i], ID+Owner
    // set), not a stray live instance. Faithful sequence a human's click produces:
    // ui.setHoverContainerSlot(slot) -> slot.pressButton(). We drive ONLY this (NO em_take) and measure
    // the count delta: a clean 2->1 proves pressButton takes EXACTLY ONE item (the hovered one) -- the
    // "take exactly X" the race is built on ("both take X"). em_take is player-side (its bytecode works
    // playerListIds/slots_player) and would confound the attribution, so it is dropped from the take path.
    // (3a) Resolve the UI's bound slot + which item X it maps to, and CAPTURE X's signature while it is
    // still in the container. Then count X globally = the verifier's POSITIVE CONTROL phase A (expect 1).
    RunGT([pb](std::atomic<int>& d) {
        void* ui = FirstLiveOfClass(L"ui_playerInventory_C");
        void* slot = nullptr;
        if (ui) {   // ui.slots_prop[0] -- the container slot the UI actually built (bound to the container)
            const int32_t off = R::FindPropertyOffset(R::ClassOf(ui), L"slots_prop");
            if (off >= 0) {
                const SR::Arr arr = SR::ReadArr(ui, off);   // TArray<slot*>: data = array of 8-byte ptrs
                if (arr.num > 0 && arr.data) std::memcpy(&slot, arr.data, sizeof(slot));
            }
        }
        pb->slotFound = (slot != nullptr && R::IsLive(slot));
        if (slot) pb->slotId = ReadInt32Field(slot, L"ID");   // which container-slice index this slot maps to
        pb->xSig = CaptureContainerSlotSig(pb->container, pb->slotId);   // X, before the take
        d.store(1);
    });
    RunGT([pb](std::atomic<int>& d) { pb->phaseA = CountItemInstances(pb->xSig, /*print=*/true); d.store(1); });

    // (3b) The faithful take: re-resolve the bound slot, set the hover, fire pressButton (NO em_take).
    RunGT([pb](std::atomic<int>& d) {
        void* ui = FirstLiveOfClass(L"ui_playerInventory_C");
        void* slot = nullptr;
        if (ui) {
            const int32_t off = R::FindPropertyOffset(R::ClassOf(ui), L"slots_prop");
            if (off >= 0) { const SR::Arr arr = SR::ReadArr(ui, off); if (arr.num > 0 && arr.data) std::memcpy(&slot, arr.data, sizeof(slot)); }
        }
        if (ui && slot && R::IsLive(slot)) {
            void* hoverFn = R::FindFunction(R::ClassOf(ui), L"setHoverContainerSlot");
            if (hoverFn) { ue_wrap::ParamFrame pf(hoverFn); pf.Set<void*>(L"containerSlot", slot); ue_wrap::Call(ui, pf); }
            pb->pressCalled = CallNoArg(slot, R::ClassOf(slot), L"pressButton");   // the take -- NO em_take
        }
        UE_LOGI("director/ctake: RUNG3 faithful boundSlot=%p slotID=%d hover+press=%d (NO em_take -- isolated)",
                slot, pb->slotId, pb->pressCalled ? 1 : 0);
        d.store(1);
    });
    ::Sleep(200);   // let the take + the container_contents 0x45 edge settle
    RunGT([pb](std::atomic<int>& d) { pb->countAfterPress = ContainerItemCount(pb->container); d.store(1); });
    // (3c) Count X again AFTER the solo take = the verifier's POSITIVE CONTROL phase B (expect 1: X moved
    // to the player, NOT duplicated). phaseA==1 && phaseB==1 validates the instrument sees X in the
    // source AND destination store, counts each once, and X is unique -- so count==2 on a race == a dup.
    RunGT([pb](std::atomic<int>& d) { pb->phaseB = CountItemInstances(pb->xSig, /*print=*/true); d.store(1); });
    const int pressDelta = (pb->countBefore >= 0 && pb->countAfterPress >= 0) ? (pb->countBefore - pb->countAfterPress) : -1;
    UE_LOGI("director/ctake: RUNG3 pressButton delta = %d (before=%d after=%d) -- %s",
            pressDelta, pb->countBefore, pb->countAfterPress,
            pressDelta == 1 ? "EXACTLY ONE (take-exactly-X proven)"
                            : pressDelta > 1 ? "MORE THAN ONE (pressButton took multiple -- not single-item)"
                                             : "ZERO (pressButton did not take)");

    // RUNG 5 (DIAGNOSTIC, NON-FAITHFUL): only if the faithful pressButton took NOTHING (pressDelta<=0),
    // prove the mechanism CAN be driven at the effect seam -- extract(0) on the container actor. Flagged.
    // A pressDelta>1 (took multiple) is NOT a drivability failure -- it drove, just not single-item -- so
    // extract is not run for that case.
    if (pressDelta <= 0) {
        RunGT([pb](std::atomic<int>& d) {
            // extract is declared on the base prop_container_C -- resolve on the declaring class, not the
            // leaf (the same findfunction-superclass trap as openContainer above).
            void* fn = R::FindFunction(ContainerClass(), L"extract");
            if (fn) {
                ue_wrap::ParamFrame pf(fn);
                if (pf.valid()) { pf.Set<int32_t>(L"Index", 0); pb->extractCalled = ue_wrap::Call(pb->container, pf); }
            }
            UE_LOGI("director/ctake: RUNG5 (diagnostic, NON-FAITHFUL) extract(0) call=%d", pb->extractCalled ? 1 : 0);
            d.store(1);
        });
        ::Sleep(200);
        RunGT([pb](std::atomic<int>& d) { pb->countAfterExtract = ContainerItemCount(pb->container); d.store(1); });
    }
    const bool extractWorked = (pb->extractCalled && pb->countBefore > 0 && pb->countAfterExtract >= 0 &&
                                pb->countAfterExtract < pb->countBefore);

    // The race needs TAKE-EXACTLY-ONE-X. Only pressDelta==1 (pressButton ALONE removed exactly one item,
    // NO em_take) proves it. pressDelta>1 = drivable-but-multi (needs a single-item mechanism);
    // pressDelta<=0 falls through to the extract diagnostic (effect-seam-only, not race-usable).
    const char* verdict = (pressDelta == 1) ? "DRIVABLE-FAITHFUL-SINGLE (take-exactly-X)"
                        : (pressDelta > 1)  ? "DRIVABLE-FAITHFUL-MULTI (took >1 -- not single-item)"
                        : extractWorked     ? "DRIVABLE-EFFECT-SEAM-ONLY (extract; not race-faithful)"
                                            : "NOT-DRIVABLE";
    // The no-dup verifier's POSITIVE CONTROL: a solo take cannot dup, so X must count 1 before AND after.
    const bool controlPass = (pb->phaseA == 1 && pb->phaseB == 1);
    UE_LOGI("director/ctake: VERDICT %s | container=%ls itemsBefore=%d "
            "| open=%d uiOpened=%d boundSlot=%d slotID=%d hover+press=%d pressDelta=%d(after=%d) "
            "| [diag] extract=%d extractAfter=%d "
            "| (cross-check the container_contents 0x45 edge line in the log for the same eid)",
            verdict, pb->fname.c_str(), pb->countBefore,
            pb->openCalled ? 1 : 0, pb->uiOpened ? 1 : 0, pb->slotFound ? 1 : 0, pb->slotId,
            pb->pressCalled ? 1 : 0, pressDelta, pb->countAfterPress,
            pb->extractCalled ? 1 : 0, pb->countAfterExtract);
    UE_LOGI("director/ctake: DUP-VERIFIER CONTROL %s | X(cls=%ls key=%ls) phaseA(before)=%d phaseB(after)=%d "
            "| control needs 1 & 1 (solo cannot dup); if either != 1 the instrument is NOT race-ready "
            "(blind / double-counting / X not unique) -- FIX before staging the race",
            controlPass ? "PASS" : "FAIL", pb->xSig.className.c_str(), pb->xSig.key.c_str(), pb->phaseA, pb->phaseB);
}

DWORD WINAPI ContainerTakeProbeThread(LPVOID /*arg*/) {
    RunContainerTakeProbe();
    return 0;
}

// ============================================================================================
// The two-peer CONTAINER concurrent-take RACE (the director's raison d'etre).
// ============================================================================================
namespace {

std::string EnvStr(const char* k) { return coop::config::ReadEnv(k); }

uint64_t NowUnixMs() {   // system wall-clock (shared across peers on one box) as Unix ms
    FILETIME ft; ::GetSystemTimeAsFileTime(&ft);
    uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;  // 100ns since 1601
    return (t - 116444736000000000ULL) / 10000ULL;   // -> Unix ms
}

// Deterministic SHARED target: among placed NON-EMPTY containers, the one CLOSEST to world origin --
// identical on BOTH peers (same host world), independent of each peer's own position (so both choose
// the SAME actor; nav-reachability is a feasibility check AFTER, not a selection input -- otherwise the
// two peers' different candidate sets could diverge). Closest-to-origin favors the BASE (its furniture
// sits near origin ~6400cm; sandbox/far-storage boxes are tens of thousands of cm out and unreachable --
// measured 2026-07-23: "smallest X" picked a prop_container_sbox at -61856 that neither peer could walk
// to). Keyed by world position (stable for a static placed container), NOT eid (join-window unstable,
// LESSONS §2) and NOT the runtime FName (its numeric suffix differs cross-peer -- measured this run).
void* PickSharedContainer(void* player, int32_t& outCount, ue_wrap::FVector& outPos, std::wstring& outKey) {
    // STABLE-BY-CONSTRUCTION shared key = the persistent SAVE KEY (the FName the game itself writes into
    // the save to identify the object across save/load): identical on every peer (same save), unchanged
    // by spawn order or geometry, via the mod's keyed-element index (CollectKeyIndexEntries). This is NOT
    // position (a RULE-1 hope -- a property of THIS save's geometry, rejected in /qf R2-Q2) and NOT the
    // runtime FName: for a SAVE-LOADED prop the FName Number is assigned by spawn order at load and
    // DIVERGES cross-peer (measured 2026-07-23: 2147472736 vs 2147471758). The baked-FName lesson
    // (placed_actor_identity_use_baked_fname) covers LEVEL sublevel exports (door_box), NOT save-loaded
    // props -- so R11-Q3 ("shared target by baked FName") did not apply to containers. Post-s22 the keyed
    // eid this maps to is host-authoritative (what R2-Q2 lacked). Pick the lexicographically SMALLEST key
    // among non-empty keyed containers (deterministic cross-peer). Nav-reachability is a DEMOTED
    // TEST-FEASIBILITY filter (so the walk can finish) -- NOT the identity; the orchestrator validates
    // both peers picked the SAME key.
    std::vector<PT::KeyIndexEntry> keyed;
    PT::CollectKeyIndexEntries(keyed);
    struct Cand { void* o; ue_wrap::FVector p; int32_t cnt; std::wstring key; };
    std::vector<Cand> cs;
    for (const auto& ke : keyed) {
        if (!ke.actor || ke.key.empty() || !R::IsLiveByIndex(ke.actor, ke.internalIdx)) continue;
        if (!IsPlacedContainer(ke.actor)) continue;
        const int32_t cnt = ContainerItemCount(ke.actor);
        if (cnt <= 0) continue;
        cs.push_back({ ke.actor, E::GetActorLocation(ke.actor), cnt, ke.key });
    }
    std::sort(cs.begin(), cs.end(), [](const Cand& a, const Cand& b){ return a.key < b.key; });  // BY CONSTRUCTION
    const ue_wrap::FVector at = E::GetActorLocation(player);
    for (const Cand& c : cs) {
        std::vector<ue_wrap::FVector> path;
        if (!E::FindNavPath(player, at, c.p, path)) continue;                  // feasibility: reachable
        if (path.empty() || HorizDist(path.back(), c.p) > kReachCm) continue;
        outCount = c.cnt; outPos = c.p; outKey = c.key; return c.o;            // smallest-key reachable
    }
    outCount = 0; outPos = {}; outKey.clear();
    return nullptr;
}

// Wait on the orchestrator GO sentinel: mp.py writes a FUTURE Unix-ms into `goFile` once BOTH peers have
// logged ARRIVED; each bot busy-waits to that instant -> sub-ms simultaneity on one box (shared clock,
// /qf R2 / §B5). Returns true when the GO instant is reached; false on timeout (never hang).
bool WaitForGo(const std::string& goFile, uint64_t timeoutMs) {
    const uint64_t start = NowUnixMs();
    uint64_t targetMs = 0;
    while (NowUnixMs() - start < timeoutMs) {
        if (targetMs == 0) {
            std::ifstream f(goFile, std::ios::binary);
            if (f) { unsigned long long v = 0; if (f >> v && v > 0) targetMs = v; }
        }
        if (targetMs != 0) {
            if (NowUnixMs() >= targetMs) return true;   // GO instant reached
            ::Sleep(1);   // tight spin to the future timestamp
        } else {
            ::Sleep(20);  // poll for the sentinel to appear
        }
    }
    UE_LOGW("director/ctake-race: GO sentinel timed out (%llums) file=%s", (unsigned long long)timeoutMs, goFile.c_str());
    return false;
}

}  // namespace

void RunContainerRace() {
    const std::string mode = EnvStr("VOTVCOOP_RACE_MODE");    // "race" | "control"
    const std::string role = EnvStr("VOTVCOOP_RACE_ROLE");    // "host" | "client"
    const std::string goFile = EnvStr("VOTVCOOP_RACE_GO_FILE");
    std::string taker = EnvStr("VOTVCOOP_RACE_TAKER");        // control mode: which SINGLE peer takes
    if (taker.empty()) taker = "host";
    // control mode: only ONE peer takes -> the solo sum across peers MUST be 1. Run it BOTH directions
    // (taker=host AND taker=client): host-takes proves the client sees the CONTAINER; client-takes proves
    // the client's OWN PERSONAL store walk finds X (where a losing client's optimistic dup copy lives) --
    // otherwise a real race dup on the client would read 0 -> sum 1 -> a FALSE "no dup" (invisible copy).
    const bool shouldTake = (mode == "race") || (mode == "control" && role == taker);
    UE_LOGI("director/ctake-race: START mode=%s role=%s taker=%s shouldTake=%d goFile=%s",
            mode.c_str(), role.c_str(), taker.c_str(), shouldTake ? 1 : 0, goFile.c_str());

    ::Sleep(20000);   // settle for the world to load

    auto rsv = std::make_shared<void*>(nullptr);
    for (int waited = 0; waited < 60 && !*rsv; ++waited) {
        const int r = RunGT([rsv](std::atomic<int>& d) {
            void* p = coop::players::Registry::Get().Local();
            if (p && R::IsLive(p) && E::GetController(p)) { *rsv = p; d.store(1); } else d.store(2);
        });
        if (r == 1) break; ::Sleep(1000);
    }
    if (!*rsv) { UE_LOGW("director/ctake-race: VERDICT no possessed local player -- ABORT (role=%s)", role.c_str()); return; }

    // Deterministic SHARED target + capture X (both peers read the SAME container slice[0] -> same X sig).
    // RETRY until a shared container with contents is resolvable: on a client the joined world + the
    // container-contents sync land AFTER the player is possessed, so an early pick sees an empty/absent
    // world (measured 2026-07-23: client "could not pick" while the host had already picked).
    DirectorGoal goal; goal.reachCm = kReachCm;
    auto pb = std::make_shared<Probe>();
    bool picked = false;
    for (int tries = 0; tries < 40 && !picked; ++tries) {
        const int r = RunGT([rsv, &goal, pb](std::atomic<int>& d) {
            ue_wrap::FVector pos{}; int32_t cnt = 0; std::wstring key;
            void* c = PickSharedContainer(*rsv, cnt, pos, key);
            if (!c) { d.store(2); return; }
            goal.targetActor = c; goal.targetPos = pos; pb->container = c; pb->countBefore = cnt;
            pb->fname = R::ToString(R::NameOf(c)); pb->targetKey = key; pb->slotId = 0;
            pb->xSig = CaptureContainerSlotSig(c, pb->slotId);
            UE_LOGI("director/ctake-race: SHARED target key=%ls pos=(%.0f,%.0f,%.0f) items=%d slotID=%d "
                    "X(cls=%ls key=%ls) -- both peers must pick the SAME save-key (by construction)",
                    key.c_str(), pos.X, pos.Y, pos.Z, cnt, pb->slotId,
                    pb->xSig.className.c_str(), pb->xSig.key.c_str());
            d.store(1);
        });
        if (r == 1) picked = true; else ::Sleep(2000);   // world/contents not ready yet -- wait + retry
    }
    if (!picked) { UE_LOGW("director/ctake-race: VERDICT could not pick shared container (world not ready / none reachable) -- ABORT (role=%s)", role.c_str()); return; }

    // Count X BEFORE (each peer): the pre-race per-peer count. Control's solo sum before/after both == 1.
    RunGT([pb](std::atomic<int>& d) { pb->phaseA = CountItemInstances(pb->xSig, /*print=*/false); d.store(1); });
    UE_LOGI("director/ctake-race: PRECOUNT role=%s localCountBefore=%d", role.c_str(), pb->phaseA);

    // Walk to the shared container (generous deadline: the smallest-key container may be a longer route
    // than a nearest pick, and the client's route can be harder than the host's -- never-give-up grinds).
    { ControlManager mgr; AddWalkToProcesses(mgr, goal); mgr.Run(goal, /*maxSeconds=*/120);
      if (!goal.reached) { UE_LOGW("director/ctake-race: VERDICT did NOT reach shared container (role=%s reason=%s) -- ABORT", role.c_str(), goal.failReason); return; } }

    // Open + resolve the bound slot, then log ARRIVED (the orchestrator waits for BOTH before GO).
    RunGT([rsv, pb](std::atomic<int>& d) {
        E::WriteMainPlayerLookAtActor(*rsv, pb->container);
        pb->openCalled = CallNoArg(pb->container, ContainerClass(), L"openContainer");
        d.store(1);
    });
    for (int i = 0; i < kUiWaitTicks && !pb->slotFound; ++i) {
        RunGT([pb](std::atomic<int>& d) { pb->slotFound = (FirstLiveOfClass(L"uicomp_playerInvContainerSlot_C") != nullptr); d.store(1); });
        if (!pb->slotFound) ::Sleep(30);
    }
    UE_LOGI("director/ctake-race: ARRIVED role=%s key=%ls open=%d slot=%d -- waiting for GO",
            role.c_str(), pb->targetKey.c_str(), pb->openCalled ? 1 : 0, pb->slotFound ? 1 : 0);

    // Barrier: wait on the orchestrator GO (future-timestamp), then fire the faithful take AT the GO instant.
    const bool go = goFile.empty() ? true : WaitForGo(goFile, /*timeoutMs=*/60000);
    if (go && shouldTake) {
        RunGT([pb](std::atomic<int>& d) {
            void* ui = FirstLiveOfClass(L"ui_playerInventory_C");
            void* slot = nullptr;
            if (ui) { const int32_t off = R::FindPropertyOffset(R::ClassOf(ui), L"slots_prop");
                      if (off >= 0) { const SR::Arr arr = SR::ReadArr(ui, off); if (arr.num > 0 && arr.data) std::memcpy(&slot, arr.data, sizeof(slot)); } }
            if (ui && slot && R::IsLive(slot)) {
                void* hoverFn = R::FindFunction(R::ClassOf(ui), L"setHoverContainerSlot");
                if (hoverFn) { ue_wrap::ParamFrame pf(hoverFn); pf.Set<void*>(L"containerSlot", slot); ue_wrap::Call(ui, pf); }
                pb->pressCalled = CallNoArg(slot, R::ClassOf(slot), L"pressButton");
            }
            UE_LOGI("director/ctake-race: FIRED take press=%d (GO reached)", pb->pressCalled ? 1 : 0);
            d.store(1);
        });
    } else {
        UE_LOGI("director/ctake-race: role=%s did NOT take (go=%d shouldTake=%d)", role.c_str(), go ? 1 : 0, shouldTake ? 1 : 0);
    }

    ::Sleep(1500);   // let the take + the host CAS + any re-publish settle across peers
    RunGT([pb](std::atomic<int>& d) { pb->phaseB = CountItemInstances(pb->xSig, /*print=*/true); d.store(1); });
    // The per-peer RESULT. mp.py sums localCountAfter across peers -> FULL matrix (1 correct / 2 dup /
    // 0 vanished / >2 worse). A single peer's count is NOT the verdict -- the CROSS-PEER sum is.
    UE_LOGI("director/ctake-race: RESULT role=%s mode=%s took=%d localCountBefore=%d localCountAfter=%d "
            "| X(cls=%ls key=%ls) | mp.py sums across peers: 1=correct 2=DUP(R11b) 0=VANISHED >2=worse",
            role.c_str(), mode.c_str(), (go && shouldTake) ? 1 : 0, pb->phaseA, pb->phaseB,
            pb->xSig.className.c_str(), pb->xSig.key.c_str());
}

DWORD WINAPI ContainerRaceThread(LPVOID /*arg*/) {
    RunContainerRace();
    return 0;
}

}  // namespace coop::director
