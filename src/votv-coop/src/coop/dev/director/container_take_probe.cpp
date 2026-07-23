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

#include "coop/player/players_registry.h"
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
#include <memory>
#include <string>
#include <vector>

namespace coop::director {
namespace {
namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace GT  = ue_wrap::game_thread;
namespace SR  = ue_wrap::save_record;
namespace INV = ue_wrap::inventory;
namespace PR  = ue_wrap::prop;

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
    bool     slotFound    = false;   // a live uicomp_playerInvContainerSlot_C to press
    bool     pressCalled  = false;
    int32_t  selectedAfterPress = -0x7fffffff;
    bool     takeCalled   = false;
    int32_t  countAfterFaithful = -1;
    bool     extractCalled = false;
    int32_t  countAfterExtract = -1;
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

    // RUNG 3+4: the FAITHFUL take -- press a container slot to set the selection, then em_take.
    RunGT([&goal, pb](std::atomic<int>& d) {
        void* ui   = FirstLiveOfClass(L"ui_playerInventory_C");
        void* slot = FirstLiveOfClass(L"uicomp_playerInvContainerSlot_C");
        pb->slotFound = (slot != nullptr);
        if (slot) {
            pb->pressCalled = CallNoArg(slot, R::ClassOf(slot), L"pressButton");
            pb->selectedAfterPress = ui ? ReadInt32Field(ui, L"selected") : -0x7fffffff;
        }
        if (ui) pb->takeCalled = CallNoArg(ui, R::ClassOf(ui), L"em_take");
        UE_LOGI("director/ctake: RUNG3/4 ui=%p slot=%p press=%d selected=%d em_take=%d",
                ui, slot, pb->pressCalled ? 1 : 0, pb->selectedAfterPress, pb->takeCalled ? 1 : 0);
        d.store(1);
    });
    ::Sleep(200);   // let the take + the container_contents 0x45 edge settle
    RunGT([pb](std::atomic<int>& d) { pb->countAfterFaithful = ContainerItemCount(pb->container); d.store(1); });

    const bool faithfulWorked = (pb->countBefore > 0 && pb->countAfterFaithful >= 0 &&
                                 pb->countAfterFaithful < pb->countBefore);

    // RUNG 5 (DIAGNOSTIC, NON-FAITHFUL): only if the faithful path did not remove an item, prove the
    // mechanism CAN be driven at the effect seam -- extract(0) on the container actor. Flagged.
    if (!faithfulWorked) {
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

    const char* verdict = faithfulWorked ? "DRIVABLE-FAITHFUL"
                        : extractWorked  ? "DRIVABLE-EFFECT-SEAM-ONLY"
                                         : "NOT-DRIVABLE";
    UE_LOGI("director/ctake: VERDICT %s | container=%ls itemsBefore=%d "
            "| open=%d uiOpened=%d slot=%d press=%d selected=%d em_take=%d faithfulAfter=%d "
            "| extract=%d extractAfter=%d "
            "| (cross-check the container_contents 0x45 edge line in the log for the same eid)",
            verdict, pb->fname.c_str(), pb->countBefore,
            pb->openCalled ? 1 : 0, pb->uiOpened ? 1 : 0, pb->slotFound ? 1 : 0, pb->pressCalled ? 1 : 0,
            pb->selectedAfterPress, pb->takeCalled ? 1 : 0, pb->countAfterFaithful,
            pb->extractCalled ? 1 : 0, pb->countAfterExtract);
}

DWORD WINAPI ContainerTakeProbeThread(LPVOID /*arg*/) {
    RunContainerTakeProbe();
    return 0;
}

}  // namespace coop::director
