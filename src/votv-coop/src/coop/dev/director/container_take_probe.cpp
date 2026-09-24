// coop/dev/director/container_take_probe.cpp -- the container-take input probe and the two-peer
// concurrent-take race. The take verbs the game runs are Blueprint-internal verbs, not
// reflection-callable, but the take is drivable one layer up through callable BP verbs: the
// container's openContainer opens the UI, and a container slot's pressButton takes the hovered
// item. Whether a called verb's body ran is never inferred from callability; the probe measures
// the container's item-count decrement. A ladder (open, select, take) reporting the highest
// rung reached, with the container's extract verb as a non-faithful diagnostic fallback. Dev
// only, env-gated (VOTVCOOP_RUN_CTAKE_PROBE=1); greppable "director/ctake: VERDICT".

#include "coop/dev/director/director.h"
#include "coop/dev/director/dup_verifier.h"   // the no-dup verifier + its positive control

#include "coop/config/config.h"               // ReadEnv
#include "coop/player/players_registry.h"
#include "coop/props/prop_element_tracker.h"  // CollectKeyIndexEntries -- the stable save-key index
#include "ue_wrap/actors/inventory.h"      // ResolveSaveSlot
#include "ue_wrap/actors/prop.h"           // WalksToBase
#include "ue_wrap/actors/save_record.h"    // ReadArr, kMxStride
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"             // ParamFrame, Call
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <array>
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

// A step of the probe on the game thread (GT::RunAndWait: it waits out a stall and cannot outlive a
// task that faulted). The first step whose task faulted is kept, so the probe ends naming it instead of
// judging values that step never wrote. Reset at each probe's start; the probe thread only.
const char* g_faultedStep = nullptr;

template <class Fn>
int Step(const char* name, Fn&& body) {
    const int r = GT::RunAndWait(std::forward<Fn>(body));
    if (r == GT::kTaskFaulted && !g_faultedStep) g_faultedStep = name;
    return r;
}

float HorizDist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y;
    return std::sqrt(dx * dx + dy * dy);
}

// Container reflection helpers.
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
// The propInventory component of a container actor, or null.
void* InventoryOf(void* container) {
    static int32_t off = -2;
    if (off == -2) off = R::FindPropertyOffset(R::ClassOf(container), L"propInventory");
    if (off < 0) return nullptr;
    void* inv = nullptr;
    std::memcpy(&inv, reinterpret_cast<const uint8_t*>(container) + off, sizeof(inv));
    return (inv && R::IsLive(inv)) ? inv : nullptr;
}
// A world container inventory (Player 0), never a personal one, which shares the global stack.
bool IsWorldContainerInv(void* inv) {
    static int32_t off = -2;
    if (off == -2) off = inv ? R::FindPropertyOffset(R::ClassOf(inv), L"Player") : -1;
    if (off < 0) return false;
    uint8_t player = 1;
    std::memcpy(&player, reinterpret_cast<const uint8_t*>(inv) + off, 1);
    return player == 0;
}
// The container's item count through its GObjStack slice, the read container_contents_sync
// uses: the save's GObjStack[inv.Index] wraps a TArray of save structs. -1 when unresolvable,
// distinct from 0 (empty).
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

// Call a no-arg UFunction on `obj`, resolved on the given declaring class; FindFunction is
// exact-owner.
bool CallNoArg(void* obj, void* cls, const wchar_t* fnName) {
    void* fn = cls ? R::FindFunction(cls, fnName) : nullptr;
    if (!fn) { UE_LOGW("director/ctake: verb %ls NOT FOUND on the class -- cannot drive", fnName); return false; }
    ue_wrap::ParamFrame pf(fn);
    if (!pf.valid()) return false;
    return ue_wrap::Call(obj, pf);
}

// The first live non-default instance whose class walks to `className`. One array pass; only on
// the take rung.
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

// The probe's measurements.
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
    // The no-dup verifier's positive control: the taken item's signature, its global instance count
    // before the take (in the container) and after a solo take (moved to the player). Both must be
    // 1, or a count of 1 on a real race is ambiguous.
    ItemSig  xSig;
    int32_t  phaseA = -1;
    int32_t  phaseB = -1;
    std::wstring targetKey;   // the shared container's stable SAVE KEY (the by-construction race key)
    // The pick, written by a game-thread task.
    ue_wrap::FVector targetPos{};
    bool     playerUnread = false;   // the race pick: the route start could not be read, which a retry does not change
    // The race pick's candidates whose read failed: a retry skips them, since a failed read of a live actor
    // faults again, and still counts them among those it could not place.
    std::vector<ue_wrap::CachedObjRef> unreadable;
};

}  // namespace

// The verifier's known positive for its blind branch, fired before the world settles. A failed
// inventory read once counted as "read fine, found nothing"; a branch that has never fired is
// indistinguishable from one that cannot, so the blind result is provoked on purpose. ReadAll
// fails whenever the save slot is unresolvable, which is the state at scenario start; if the
// world is already up the control did not get its chance, reported as inconclusive.
void RunVerifierBlindControl() {
    // The signature must be valid but unmatchable: an invalid one is refused by the counter's entry
    // guard, so the walk never runs and the blind branch is never reached.
    ItemSig dummy{};
    dummy.className   = L"__blind_control_no_such_class__";
    dummy.key         = L"__blind_control_no_such_key__";
    dummy.contentHash = 0;
    dummy.valid       = true;   // valid so the walk RUNS; unmatchable so it can never find a row
    auto ran = std::make_shared<std::atomic<int>>(0);
    g_faultedStep = nullptr;
    Step("the blind control", [ran, dummy](std::atomic<int>& d) {
        UE_LOGI("dup_verifier[BLIND-CONTROL]: counting BEFORE world load -- expecting "
                "'GObjStack count is BLIND' + 'player=BLIND(READ-FAILED)'");
        const int n = CountItemInstances(dummy, /*print=*/true);
        ran->store(n < 0 ? 1 : 2);
        d.store(1);
    });
    if (g_faultedStep) {
        UE_LOGW("director/ctake: BLIND-CONTROL INVALID -- the game thread's task faulted at %s", g_faultedStep);
        return;
    }
    UE_LOGI("director/ctake: BLIND-CONTROL fired (saveSlot %s at scenario start) -- read the two "
            "dup_verifier lines above; BLIND on both = the instrument's failure branch is OBSERVABLE, "
            "READ-OK = INCONCLUSIVE (world already up, control never got its chance)",
            ran->load() == 1 ? "UNRESOLVABLE" : "resolvable");
}

void RunContainerTakeProbe() {
    // First, before any settle: the instrument's own known positive.
    RunVerifierBlindControl();
    g_faultedStep = nullptr;

    UE_LOGI("director/ctake: container-take input probe -- +20 s settle for the world to load");
    ::Sleep(20000);

    // A possessed local player, the body that walks.
    struct Rsv { void* player = nullptr; };
    auto rsv = std::make_shared<Rsv>();
    for (int waited = 0; waited < 60 && !rsv->player; ++waited) {
        const int r = Step("the player", [rsv](std::atomic<int>& d) {
            void* p = coop::players::Registry::Get().Local();
            if (p && R::IsLive(p) && E::GetController(p)) { rsv->player = p; d.store(1); }
            else d.store(2);
        });
        if (r == 1 || g_faultedStep) break;
        ::Sleep(1000);
    }
    if (g_faultedStep) {
        UE_LOGW("director/ctake: VERDICT the game thread's task faulted at %s -- ABORT", g_faultedStep);
        return;
    }
    if (!rsv->player) { UE_LOGW("director/ctake: VERDICT no possessed local player -- ABORT"); return; }

    // A placed, non-empty, nav-reachable world container, by the shortest reachable route.
    DirectorGoal goal;
    goal.reachCm = kReachCm;
    auto pb = std::make_shared<Probe>();
    const int pick = Step("the pick", [rsv, pb](std::atomic<int>& d) {
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(rsv->player, at)) {
            UE_LOGW("director/ctake: the player's location could not be read"); d.store(2); return; }
        struct Cand { void* c; ue_wrap::FVector pos; int32_t count; };
        std::vector<Cand> cands;
        int unread = 0;
        const int32_t n = R::NumObjects();
        for (int32_t i = 0; i < n; ++i) {
            void* o = R::ObjectAt(i);
            if (!IsPlacedContainer(o)) continue;
            const int32_t cnt = ContainerItemCount(o);
            if (cnt <= 0) continue;                       // need a NON-EMPTY container to take from
            ue_wrap::FVector p{};
            if (!E::TryGetActorLocation(o, p)) { ++unread; continue; }   // no distance: no candidate
            const float dist = HorizDist(p, at);
            if (dist < kMinCm || dist > kMaxCm) continue;
            cands.push_back({ o, p, cnt });
        }
        if (unread > 0)
            UE_LOGW("director/ctake: %d non-empty container(s) could not be placed, so none of them is a candidate",
                    unread);
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
            if (HorizDist(path.back(), c.pos) > kReachCm) continue;         // route ends short of reach
            float len = 0.f;
            for (size_t i = 1; i < path.size(); ++i) len += HorizDist(path[i - 1], path[i]);
            if (len < bestLen) { bestLen = len; pb->container = c.c; pb->targetPos = c.pos; pb->countBefore = c.count; }
        }
        if (!pb->container) {
            UE_LOGW("director/ctake: no nav-reachable non-empty container (all off-mesh / behind locked doors) -- ABORT");
            d.store(2); return;
        }
        pb->fname = R::ToString(R::NameOf(pb->container));
        UE_LOGI("director/ctake: target container=%p fname=%ls items=%d pos=(%.0f,%.0f,%.0f) routeLen=%.0fcm",
                pb->container, pb->fname.c_str(), pb->countBefore,
                pb->targetPos.X, pb->targetPos.Y, pb->targetPos.Z, bestLen);
        d.store(1);
    });
    if (g_faultedStep) {
        UE_LOGW("director/ctake: VERDICT the game thread's task faulted at %s -- ABORT", g_faultedStep);
        return;
    }
    if (pick != 1) { UE_LOGW("director/ctake: VERDICT could not pick a container -- ABORT"); return; }
    goal.targetActor = pb->container;
    goal.targetPos = pb->targetPos;

    // Walk to it with the director (clear the hand, go to, reach).
    {
        ControlManager mgr;
        AddWalkToProcesses(mgr, goal);
        mgr.Run(goal, /*maxSeconds=*/110);
        if (!goal.reached) {
            UE_LOGW("director/ctake: VERDICT walk did NOT reach the container (reason=%s) -- ABORT "
                    "(container=%ls)", goal.failReason, pb->fname.c_str());
            return;
        }
    }

    // The take ladder: each rung verified at runtime, the highest reached and the count delta
    // reported.
    Step("rung 1", [rsv, pb](std::atomic<int>& d) {
        // openContainer and extract are declared on the base container class, and the target is a
        // subclass; FindFunction is exact-owner, so the verbs are resolved on the declaring class
        // and dispatched on the instance.
        void* contBaseCls = ContainerClass();   // prop_container_C -- where openContainer/extract live
        // The count re-read now.
        pb->countBefore = ContainerItemCount(pb->container);
        // Aim at the container; the interaction reads the player's look target.
        E::WriteMainPlayerLookAtActor(rsv->player, pb->container);
        // Rung 1: openContainer, the UI a person's interact opens.
        pb->openCalled = CallNoArg(pb->container, contBaseCls, L"openContainer");
        UE_LOGI("director/ctake: RUNG1 openContainer call=%d (countBefore=%d)", pb->openCalled ? 1 : 0, pb->countBefore);
        d.store(1);
    });
    // The UI widget spawns. The container slot is the real "a container UI is open" signal; a bare
    // inventory widget can pre-exist, pooled and closed.
    for (int i = 0; i < kUiWaitTicks && !pb->slotFound; ++i) {
        Step("the UI wait", [pb](std::atomic<int>& d) {
            pb->uiOpened  = (FirstLiveOfClass(L"ui_playerInventory_C") != nullptr);
            pb->slotFound = (FirstLiveOfClass(L"uicomp_playerInvContainerSlot_C") != nullptr);
            d.store(1);
        });
        if (!pb->slotFound) ::Sleep(30);
    }
    UE_LOGI("director/ctake: RUNG2 container UI: ui_playerInventory=%d containerSlot=%d",
            pb->uiOpened ? 1 : 0, pb->slotFound ? 1 : 0);

    // Rung 3, the faithful take, isolated to pressButton so the delta is attributed. The slot's
    // click handler calls setHoverContainerSlot on its owner UI, so the take is keyed on which slot
    // the UI considers hovered, and it must be the UI's own bound slot, not a stray instance. A
    // person's click produces setHoverContainerSlot then pressButton; only that is driven, and a
    // clean decrement of one proves pressButton takes exactly the hovered item. em_take is
    // player-side and would confound the attribution. First the bound slot and the item it maps to
    // are resolved and the item's signature captured while it is still in the container; its
    // global count is the control's phase A.
    Step("the bound slot", [pb](std::atomic<int>& d) {
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
    Step("phase A", [pb](std::atomic<int>& d) { pb->phaseA = CountItemInstances(pb->xSig, /*print=*/true); d.store(1); });

    // The faithful take: the bound slot re-resolved, the hover set, pressButton fired.
    Step("rung 3", [pb](std::atomic<int>& d) {
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
    ::Sleep(200);   // let the take + the container_contents watch edge settle
    Step("the count after the press", [pb](std::atomic<int>& d) { pb->countAfterPress = ContainerItemCount(pb->container); d.store(1); });
    // The item counted again after the solo take, phase B: both phases at 1 mean the instrument
    // sees the item in the source and the destination store, counts each once, and the item is
    // unique, so 2 on a race is a duplicate.
    Step("phase B", [pb](std::atomic<int>& d) { pb->phaseB = CountItemInstances(pb->xSig, /*print=*/true); d.store(1); });
    const int pressDelta = (pb->countBefore >= 0 && pb->countAfterPress >= 0) ? (pb->countBefore - pb->countAfterPress) : -1;
    UE_LOGI("director/ctake: RUNG3 pressButton delta = %d (before=%d after=%d) -- %s",
            pressDelta, pb->countBefore, pb->countAfterPress,
            pressDelta == 1 ? "EXACTLY ONE (take-exactly-X proven)"
                            : pressDelta > 1 ? "MORE THAN ONE (pressButton took multiple -- not single-item)"
                                             : "ZERO (pressButton did not take)");

    // Rung 5, diagnostic and non-faithful, only if pressButton took nothing: extract(0) on the
    // container, to prove the mechanism can be driven at the effect seam. A multi-item press is not
    // a drivability failure, so extract does not run for it.
    if (pressDelta <= 0) {
        Step("rung 5", [pb](std::atomic<int>& d) {
            // Resolved on the declaring class.
            void* fn = R::FindFunction(ContainerClass(), L"extract");
            if (fn) {
                ue_wrap::ParamFrame pf(fn);
                if (pf.valid()) { pf.Set<int32_t>(L"Index", 0); pb->extractCalled = ue_wrap::Call(pb->container, pf); }
            }
            UE_LOGI("director/ctake: RUNG5 (diagnostic, NON-FAITHFUL) extract(0) call=%d", pb->extractCalled ? 1 : 0);
            d.store(1);
        });
        ::Sleep(200);
        Step("the count after the extract", [pb](std::atomic<int>& d) { pb->countAfterExtract = ContainerItemCount(pb->container); d.store(1); });
    }
    if (g_faultedStep) {
        UE_LOGW("director/ctake: VERDICT the game thread's task faulted at %s -- ABORT (the ladder's values are "
                "not all written)", g_faultedStep);
        return;
    }
    const bool extractWorked = (pb->extractCalled && pb->countBefore > 0 && pb->countAfterExtract >= 0 &&
                                pb->countAfterExtract < pb->countBefore);

    // The race needs take-exactly-one: only a press delta of 1 proves it; more is drivable but
    // multi-item, none falls through to the extract diagnostic.
    const char* verdict = (pressDelta == 1) ? "DRIVABLE-FAITHFUL-SINGLE (take-exactly-X)"
                        : (pressDelta > 1)  ? "DRIVABLE-FAITHFUL-MULTI (took >1 -- not single-item)"
                        : extractWorked     ? "DRIVABLE-EFFECT-SEAM-ONLY (extract; not race-faithful)"
                                            : "NOT-DRIVABLE";
    // The verifier's positive control: a solo take cannot duplicate, so the item counts 1 before
    // and after.
    const bool controlPass = (pb->phaseA == 1 && pb->phaseB == 1);
    UE_LOGI("director/ctake: VERDICT %s | container=%ls itemsBefore=%d "
            "| open=%d uiOpened=%d boundSlot=%d slotID=%d hover+press=%d pressDelta=%d(after=%d) "
            "| [diag] extract=%d extractAfter=%d "
            "| (cross-check the container_contents watch edge line in the log for the same eid)",
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

// The two-peer concurrent-take race.
namespace {

std::string EnvStr(const char* k) { return coop::config::ReadEnv(k); }

uint64_t NowUnixMs() {   // system wall-clock (shared across peers on one box) as Unix ms
    FILETIME ft; ::GetSystemTimeAsFileTime(&ft);
    uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;  // 100ns since 1601
    return (t - 116444736000000000ULL) / 10000ULL;   // -> Unix ms
}

// The deterministic shared target, identical on both peers. `playerUnread`: the route start could not be read,
// which a retry does not change. `unreadOut`: keyed non-empty containers that could not be placed; one the other
// peer could place makes the two picks differ, so the count is printed with the pick.
void* PickSharedContainer(void* player, int32_t& outCount, ue_wrap::FVector& outPos, std::wstring& outKey,
                          bool& playerUnread, int& unreadOut, std::vector<ue_wrap::CachedObjRef>& unreadable) {
    unreadOut = 0;
    ue_wrap::FVector at{};
    playerUnread = !E::TryGetActorLocation(player, at);
    if (playerUnread) return nullptr;
    // The shared key is the persistent save key, identical on every peer (the same save) and
    // unchanged by spawn order or geometry, through the mod's keyed-element index. Not the position
    // (a property of this save's geometry) and not the runtime name, whose number is assigned by
    // spawn order at load and diverges across peers. The lexicographically smallest key among
    // non-empty keyed containers wins; nav-reachability is a feasibility filter, not the identity,
    // and the orchestrator checks both peers picked the same key.
    std::vector<PT::KeyIndexEntry> keyed;
    PT::CollectKeyIndexEntries(keyed);
    struct Cand { void* o; ue_wrap::FVector p; int32_t cnt; std::wstring key; };
    std::vector<Cand> cs;
    for (const auto& ke : keyed) {
        if (!ke.actor || ke.key.empty() || !R::IsLiveByIndex(ke.actor, ke.internalIdx)) continue;
        if (!IsPlacedContainer(ke.actor)) continue;
        const int32_t cnt = ContainerItemCount(ke.actor);
        if (cnt <= 0) continue;
        bool latched = false;
        for (const ue_wrap::CachedObjRef& u : unreadable)
            if (u.Is(ke.actor)) { latched = true; break; }
        ue_wrap::FVector p{};
        if (latched || !E::TryGetActorLocation(ke.actor, p)) {   // no route to judge: no candidate
            if (!latched) {
                unreadable.emplace_back();
                unreadable.back().Set(ke.actor);
            }
            ++unreadOut;
            continue;
        }
        cs.push_back({ ke.actor, p, cnt, ke.key });
    }
    std::sort(cs.begin(), cs.end(), [](const Cand& a, const Cand& b){ return a.key < b.key; });  // BY CONSTRUCTION
    for (const Cand& c : cs) {
        std::vector<ue_wrap::FVector> path;
        if (!E::FindNavPath(player, at, c.p, path)) continue;                  // feasibility: reachable
        if (path.empty() || HorizDist(path.back(), c.p) > kReachCm) continue;
        outCount = c.cnt; outPos = c.p; outKey = c.key; return c.o;            // smallest-key reachable
    }
    outCount = 0; outPos = {}; outKey.clear();
    return nullptr;
}

// The orchestrator's GO sentinel: mp.py writes a future Unix time in ms into the file once both
// peers have logged ARRIVED, and each bot spins to that instant, sub-millisecond simultaneity
// on one box. False on timeout.
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
    // Control mode: only one peer takes, so the sum across peers must be 1. Run in both directions:
    // a host take proves the client sees the container, a client take proves the client's own
    // store walk finds the item (where a losing client's optimistic copy would live), or a real
    // duplicate on the client would read 0 and the sum would falsely say no duplicate.
    const bool shouldTake = (mode == "race") || (mode == "control" && role == taker);
    UE_LOGI("director/ctake-race: START mode=%s role=%s taker=%s shouldTake=%d goFile=%s",
            mode.c_str(), role.c_str(), taker.c_str(), shouldTake ? 1 : 0, goFile.c_str());

    ::Sleep(20000);   // settle for the world to load

    g_faultedStep = nullptr;
    auto rsv = std::make_shared<void*>(nullptr);
    for (int waited = 0; waited < 60 && !*rsv; ++waited) {
        const int r = Step("the player", [rsv](std::atomic<int>& d) {
            void* p = coop::players::Registry::Get().Local();
            if (p && R::IsLive(p) && E::GetController(p)) { *rsv = p; d.store(1); } else d.store(2);
        });
        if (r == 1 || g_faultedStep) break;
        ::Sleep(1000);
    }
    if (g_faultedStep) {
        UE_LOGW("director/ctake-race: VERDICT the game thread's task faulted at %s -- ABORT (role=%s)", g_faultedStep,
                role.c_str());
        return;
    }
    if (!*rsv) { UE_LOGW("director/ctake-race: VERDICT no possessed local player -- ABORT (role=%s)", role.c_str()); return; }

    // The shared target and the item's signature (both peers read the same slice, so the same
    // signature). Retried until a shared container with contents resolves: on a client the
    // contents sync lands after the player is possessed.
    DirectorGoal goal; goal.reachCm = kReachCm;
    auto pb = std::make_shared<Probe>();
    auto unread = std::make_shared<int>(0);   // the last try's unplaceable candidates
    bool picked = false;
    for (int tries = 0; tries < 40 && !picked && !pb->playerUnread; ++tries) {
        const int r = Step("the pick", [rsv, pb, unread](std::atomic<int>& d) {
            ue_wrap::FVector pos{}; int32_t cnt = 0; std::wstring key;
            void* c = PickSharedContainer(*rsv, cnt, pos, key, pb->playerUnread, *unread, pb->unreadable);
            if (!c) { d.store(2); return; }
            pb->container = c; pb->targetPos = pos; pb->countBefore = cnt;
            pb->fname = R::ToString(R::NameOf(c)); pb->targetKey = key; pb->slotId = 0;
            pb->xSig = CaptureContainerSlotSig(c, pb->slotId);
            UE_LOGI("director/ctake-race: SHARED target key=%ls pos=(%.0f,%.0f,%.0f) items=%d slotID=%d "
                    "X(cls=%ls key=%ls) unreadAnywhere=%d -- both peers must pick the SAME save-key (by construction)",
                    key.c_str(), pos.X, pos.Y, pos.Z, cnt, pb->slotId,
                    pb->xSig.className.c_str(), pb->xSig.key.c_str(), *unread);
            d.store(1);
        });
        if (r == 1) picked = true;
        else if (g_faultedStep) break;
        else if (!pb->playerUnread) ::Sleep(2000);   // world/contents not ready yet -- wait + retry
    }
    if (g_faultedStep) {
        UE_LOGW("director/ctake-race: VERDICT the game thread's task faulted at %s -- ABORT (role=%s)", g_faultedStep,
                role.c_str());
        return;
    }
    if (pb->playerUnread) {
        UE_LOGW("director/ctake-race: VERDICT the player's location could not be read, so no route starts -- ABORT "
                "(role=%s)", role.c_str());
        return;
    }
    if (!picked) {
        UE_LOGW("director/ctake-race: VERDICT could not pick shared container (world not ready / none reachable; "
                "unreadAnywhere=%d on the last try) -- ABORT (role=%s)", *unread, role.c_str());
        return;
    }
    goal.targetActor = pb->container;
    goal.targetPos = pb->targetPos;

    // The item counted before, per peer.
    Step("phase A", [pb](std::atomic<int>& d) { pb->phaseA = CountItemInstances(pb->xSig, /*print=*/false); d.store(1); });
    UE_LOGI("director/ctake-race: PRECOUNT role=%s localCountBefore=%d", role.c_str(), pb->phaseA);

    // Walk to the shared container; a generous deadline, since the smallest-key container may be a
    // long route.
    { ControlManager mgr; AddWalkToProcesses(mgr, goal); mgr.Run(goal, /*maxSeconds=*/300);
      if (!goal.reached) { UE_LOGW("director/ctake-race: VERDICT did NOT reach shared container (role=%s reason=%s) -- ABORT", role.c_str(), goal.failReason); return; } }

    // Open, resolve the bound slot, then log ARRIVED; the orchestrator waits for both before GO.
    Step("the open", [rsv, pb](std::atomic<int>& d) {
        E::WriteMainPlayerLookAtActor(*rsv, pb->container);
        pb->openCalled = CallNoArg(pb->container, ContainerClass(), L"openContainer");
        d.store(1);
    });
    for (int i = 0; i < kUiWaitTicks && !pb->slotFound; ++i) {
        Step("the UI wait", [pb](std::atomic<int>& d) { pb->slotFound = (FirstLiveOfClass(L"uicomp_playerInvContainerSlot_C") != nullptr); d.store(1); });
        if (!pb->slotFound) ::Sleep(30);
    }
    UE_LOGI("director/ctake-race: ARRIVED role=%s key=%ls open=%d slot=%d -- waiting for GO",
            role.c_str(), pb->targetKey.c_str(), pb->openCalled ? 1 : 0, pb->slotFound ? 1 : 0);

    // The barrier: wait on the GO instant, then fire the faithful take at it.
    const bool go = goFile.empty() ? true : WaitForGo(goFile, /*timeoutMs=*/60000);
    if (go && shouldTake) {
        Step("the take", [pb](std::atomic<int>& d) {
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
    Step("phase B", [pb](std::atomic<int>& d) { pb->phaseB = CountItemInstances(pb->xSig, /*print=*/true); d.store(1); });
    if (g_faultedStep) {
        UE_LOGW("director/ctake-race: VERDICT the game thread's task faulted at %s -- ABORT (role=%s)", g_faultedStep,
                role.c_str());
        return;
    }
    // The per-peer result; mp.py sums the after-counts across peers (1 correct, 2 a duplicate, 0
    // vanished). A single peer's count is not the verdict.
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
