// coop/dev/director/container_take_probe.cpp -- the container-take input probe and the two-peer
// concurrent-take race. The take verbs the game runs are Blueprint-internal verbs, not
// reflection-callable, but the take is drivable one layer up through callable BP verbs: the
// container's openContainer opens the UI, and a container slot's pressButton takes the hovered
// item. Whether a called verb's body ran is never inferred from callability; the probe measures
// the container's item-count decrement. A ladder (open, select, take) reporting the highest
// rung reached, with the container's extract verb as a non-faithful diagnostic fallback. Dev
// only, env-gated (VOTVCOOP_RUN_CTAKE_PROBE=1); greppable "director/ctake: VERDICT".

#include "coop/dev/container_pick.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/dup_verifier.h"
#include "coop/dev/director/routes.h"   // the no-dup verifier + its positive control
#include "container_take_judge.h"          // co-located private header: the cross-peer counts file and verdict

#include "coop/config/config.h"               // ReadEnv
#include "coop/element/registry.h"            // LivePropActor
#include "coop/player/players_registry.h"
#include "coop/props/container_act_request.h"     // dev::LastTake, LastRefusal, PendingId
#include "coop/props/container_contents_sync.h"   // ContentsDigest
#include "ue_wrap/actors/inventory.h"      // ResolveSaveSlot
#include "ue_wrap/actors/prop.h"           // WalksToBase
#include "ue_wrap/actors/save_record.h"    // ReadArr, kMxStride
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
// The container's GObjStack slice (the read container_contents_sync uses): the save's GObjStack[inv.Index] wraps a
// TArray of save structs. False when unresolvable.
bool ContainerSliceArr(void* container, SR::Arr& out) {
    void* inv = InventoryOf(container);
    if (!inv || !IsWorldContainerInv(inv)) return false;
    void* save = INV::ResolveSaveSlot();
    if (!save) return false;
    static int32_t offStack = -2, offIndex = -2;
    if (offStack == -2) offStack = R::FindPropertyOffset(R::ClassOf(save), L"GObjStack");
    if (offIndex == -2) offIndex = R::FindPropertyOffset(R::ClassOf(inv), L"Index");
    if (offStack < 0 || offIndex < 0) return false;
    int32_t idx = -1;
    std::memcpy(&idx, reinterpret_cast<const uint8_t*>(inv) + offIndex, sizeof(idx));
    if (idx < 0) return false;
    const SR::Arr stack = SR::ReadArr(save, offStack);
    if (idx >= stack.num) return false;
    const uint8_t* slot = stack.data + static_cast<size_t>(idx) * SR::kMxStride;
    out = SR::ReadArr(slot, 0);
    return true;
}
// The record count, -1 when unresolvable, distinct from 0 (empty).
int32_t ContainerItemCount(void* container) {
    SR::Arr slice;
    return ContainerSliceArr(container, slice) ? slice.num : -1;
}
// The prop name (names[0][0]) of the container's record at `slotIdx`: with the class, what the pocket count matches.
bool ContainerRecordName(void* container, int32_t slotIdx, std::wstring& name) {
    SR::Arr slice;
    if (!ContainerSliceArr(container, slice) || slotIdx < 0 || slotIdx >= slice.num) return false;
    SR::SaveRecord rec;
    SR::ReadSaveRecord(slice.data + static_cast<size_t>(slotIdx) * SR::kSaveStride, rec);
    return SR::RecordName(rec, name);
}
// The local player's own carried records that are X by class and name (not the container's, not another peer's copy);
// -1 when the inventory does not read.
int32_t PocketCount(const ItemSig& x, const std::wstring& name) {
    INV::PlayerInventory pinv;
    if (!INV::ReadAll(pinv)) return -1;
    int32_t n = 0;
    std::wstring nm;
    for (const SR::SaveRecord& r : pinv.inventory) n += (r.className == x.className && SR::RecordName(r, nm) && nm == name);
    return n;
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
constexpr float kReachCm    = 150.f;   // inside the act's 200 uu reach, or the view closes on arrival
constexpr int   kMaxCand    = 12;
constexpr int   kUiWaitTicks = 30;     // ~poll after openContainer for the UI widget to spawn
constexpr uint64_t kGoWaitMs     = 240000;   // the orchestrator's own bound on both peers arriving
constexpr uint64_t kActCloseMs   = 30000;    // a client's act open at the press, until it commits, aborts or is refused
constexpr uint64_t kCountsWaitMs = 60000;    // the other peer's counts file, after this peer's own act is over

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
    uint32_t targetEid = 0;   // the shared container's element id (the race key, equal on both peers)
    // The race's counts per peer, before (A) and after (B): the container's records and the own pocket's X.
    std::wstring xName;       // X's prop name (names[0][0]); with its class, what the pocket count matches
    int32_t  contA = -1, contB = -1, pockA = -1, pockB = -1;
    uint32_t takesA = 0;      // the take commits this peer had sent before the GO
    // The pick, written by a game-thread task.
    ue_wrap::FVector targetPos{};
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
            float len = 0.f;
            if (!RouteInLegs(rsv->player, at, c.pos, kReachCm, &len)) continue;   // no route ends within reach
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

// The target sentinel: the host's pick, shared. It sits beside the GO file; the host writes the picked container's
// element id into it (a temp file, renamed over it), the client waits for it and takes that container, so both peers
// race for one container however far apart they stand.
// The host's picked eid, polled until the file reads; 0 on timeout.
uint32_t WaitForTarget(const std::string& file, uint64_t timeoutMs) {
    const uint64_t start = NowUnixMs();
    while (NowUnixMs() - start < timeoutMs) {
        std::ifstream f(file, std::ios::binary);
        unsigned long long v = 0;
        if (f && f >> v && v > 0) return static_cast<uint32_t>(v);
        ::Sleep(200);
    }
    UE_LOGW("director/ctake-race: target sentinel timed out (%llums) file=%s", (unsigned long long)timeoutMs, file.c_str());
    return 0;
}

// Removes the sentinel when the host's probe ends, on whichever path.
struct TargetFileGuard {
    std::string file;
    ~TargetFileGuard() { if (!file.empty()) ::DeleteFileA(file.c_str()); }
};

// The container the host picked, as the pick would have found it: the live actor, its place and its record count.
bool ResolveHostPick(uint32_t eid, coop::dev::container_pick::Pick& out) {
    out = coop::dev::container_pick::Pick{};
    void* const actor = coop::element::LivePropActor(eid);
    ue_wrap::FVector at{};
    int32_t n = 0;
    float vol = 0.f;
    if (!actor || !E::TryGetActorLocation(actor, at) || !coop::props::container_contents_sync::ContentsDigest(eid, n, vol))
        return false;
    out.eid = eid;
    out.actor = actor;
    out.pos = at;
    out.records = n;
    return true;
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
    const std::string targetFile = BesideGo(goFile, "multivoid_race_target.txt");
    const bool hostPicks = role == "host";
    TargetFileGuard targetGuard;
    if (hostPicks && !targetFile.empty()) {
        ::DeleteFileA(targetFile.c_str());   // a sentinel left by an earlier run names that run's container
        targetGuard.file = targetFile;
    }
    const std::string countsFile = CountsFile(goFile, role);   // an earlier run's file would be read as the peer's
    if (!countsFile.empty()) ::DeleteFileA(countsFile.c_str());
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

    // The client takes the host's pick (the sentinel beside the GO file); the host picks and writes it.
    uint32_t hostEid = 0;
    if (!hostPicks && !targetFile.empty()) {
        hostEid = WaitForTarget(targetFile, /*timeoutMs=*/60000);
        if (!hostEid) { UE_LOGW("director/ctake-race: VERDICT no host target -- ABORT (role=%s)", role.c_str()); return; }
    }

    // The shared target and the item's signature (both peers read the same slice, so the same
    // signature). The host's target is the container pick (coop/dev/container_pick): near, a nav
    // route reaches it within the walk budget, two records or more, ranked by element id. The
    // client resolves that element id. Retried until one resolves: on a client the contents sync
    // lands after the player is possessed.
    DirectorGoal goal; goal.reachCm = kReachCm;
    auto pb = std::make_shared<Probe>();
    bool picked = false;
    for (int tries = 0; tries < 40 && !picked; ++tries) {
        const int r = Step("the pick", [pb, hostEid](std::atomic<int>& d) {
            coop::dev::container_pick::Pick pick;
            if (hostEid ? !ResolveHostPick(hostEid, pick)
                        : !coop::dev::container_pick::PickContainer(coop::dev::container_pick::Want{}, pick)) {
                d.store(2);
                return;
            }
            pb->container = pick.actor; pb->targetPos = pick.pos; pb->countBefore = pick.records;
            pb->fname = R::ToString(R::NameOf(pick.actor)); pb->targetEid = pick.eid; pb->slotId = 0;
            pb->xSig = CaptureContainerSlotSig(pick.actor, pb->slotId);
            ContainerRecordName(pick.actor, pb->slotId, pb->xName);
            UE_LOGI("director/ctake-race: SHARED target eid=%u pos=(%.0f,%.0f,%.0f) items=%d slotID=%d "
                    "X(cls=%ls key=%ls) fallback=%d -- both peers must pick the SAME eid",
                    pick.eid, pick.pos.X, pick.pos.Y, pick.pos.Z, pick.records, pb->slotId,
                    pb->xSig.className.c_str(), pb->xSig.key.c_str(), pick.fallback ? 1 : 0);
            d.store(1);
        });
        if (r == 1) picked = true;
        else if (g_faultedStep) break;
        else ::Sleep(2000);   // world/contents not ready yet -- wait + retry
    }
    if (g_faultedStep) {
        UE_LOGW("director/ctake-race: VERDICT the game thread's task faulted at %s -- ABORT (role=%s)", g_faultedStep,
                role.c_str());
        return;
    }
    if (!picked) {
        UE_LOGW("director/ctake-race: VERDICT could not pick shared container (world not ready / none near and "
                "reachable) -- ABORT (role=%s)", role.c_str());
        return;
    }
    if (hostPicks && !targetFile.empty() && !WriteSentinel(targetFile, std::to_string(pb->targetEid))) {
        UE_LOGW("director/ctake-race: VERDICT could not write the target sentinel %s -- ABORT (role=%s)", targetFile.c_str(),
                role.c_str());
        return;
    }
    goal.targetActor = pb->container;
    goal.targetPos = pb->targetPos;

    // The item counted before, per peer.
    Step("phase A", [pb](std::atomic<int>& d) {
        pb->phaseA = CountItemInstances(pb->xSig, /*print=*/false);
        pb->contA = ContainerItemCount(pb->container); pb->pockA = PocketCount(pb->xSig, pb->xName);
        pb->takesA = coop::props::container_act_request::dev::LastTake().count; d.store(1);
    });
    UE_LOGI("director/ctake-race: PRECOUNT role=%s localCountBefore=%d containerBefore=%d pocketBefore=%d X(name=%ls)",
            role.c_str(), pb->phaseA, pb->contA, pb->pockA, pb->xName.c_str());

    // Walk to the shared container; the pick takes one within 5000 cm of the player, so 300 s is generous.
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
    // mp.py ctakerace compares the key= field of this line across the peers: it is the target's element id.
    UE_LOGI("director/ctake-race: ARRIVED role=%s key=%u open=%d slot=%d -- waiting for GO",
            role.c_str(), pb->targetEid, pb->openCalled ? 1 : 0, pb->slotFound ? 1 : 0);

    // The barrier: wait on the GO instant, then fire the faithful take at it. The orchestrator writes GO once BOTH peers
    // arrived, and waits up to its own probe timeout (240 s) for the slower one, so this wait is that long: a shorter
    // wait lets the first peer judge before the other has even pressed.
    const bool go = goFile.empty() ? true : WaitForGo(goFile, kGoWaitMs);
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
    // A client that was to take reports how its act ended; the host takes natively and runs no act.
    const bool actExpected = go && shouldTake && !hostPicks;
    auto act = std::make_shared<int32_t>(kActIdle);
    // One reading of the counts and of the act, a step of the game thread; `print` lists the item's rows.
    const auto measure = [pb, act, actExpected](bool print) {
        Step("phase B", [pb, act, actExpected, print](std::atomic<int>& d) {
            namespace AR = coop::props::container_act_request;
            pb->phaseB = CountItemInstances(pb->xSig, print);
            pb->contB = ContainerItemCount(pb->container); pb->pockB = PocketCount(pb->xSig, pb->xName);
            if (actExpected) {
                const AR::dev::Take t = AR::dev::LastTake();
                *act = AR::dev::PendingId() ? kActPending
                       : t.count != pb->takesA ? (t.committed ? kActDone : kActAborted)
                       : AR::dev::LastRefusal() ? kActRefused : kActNone;
            }
            d.store(1);
        });
    };
    const auto mineNow = [pb, act] { return PeerCounts{pb->contA, pb->contB, pb->pockA, pb->pockB, *act}; };
    const auto faulted = [&role] {
        if (!g_faultedStep) return false;
        UE_LOGW("director/ctake-race: VERDICT the game thread's task faulted at %s -- ABORT (role=%s)", g_faultedStep,
                role.c_str());
        return true;
    };
    // The counts cross through files beside the GO file, and the order is the point. The client writes its counts once
    // its act closed (done, aborted or refused). The host reads ITS container only after those arrived: the client's press
    // can come long after the host reached GO, and a container read before it is the count from before the take.
    PeerCounts theirs;
    bool paired = false;
    if (hostPicks) {
        const bool got = WaitForCounts(CountsFile(goFile, "client"), kCountsWaitMs, theirs);
        measure(true);
        if (faulted()) return;
        paired = WriteCounts(countsFile, mineNow()) && got;
    } else {
        const uint64_t start = NowUnixMs();
        do { measure(false); if (g_faultedStep || *act != kActPending) break; ::Sleep(200); }
        while (NowUnixMs() - start < kActCloseMs);
        measure(true);
        if (faulted()) return;
        paired = WriteCounts(countsFile, mineNow()) &&
                 WaitForCounts(CountsFile(goFile, "host"), kCountsWaitMs, theirs);
    }
    // Both RESULT lines carry the same total and verdict. mp.py's sum of localCountAfter is informational (it counts the
    // client's mirror as a second copy).
    const PeerCounts mine = mineNow();
    const Judged j = paired ? (hostPicks ? Judge(mine, theirs) : Judge(theirs, mine))
                            : Judged{-1, "aborted", "the-other-peers-counts-did-not-arrive"};
    UE_LOGI("director/ctake-race: RESULT role=%s mode=%s took=%d localCountBefore=%d localCountAfter=%d "
            "containerBefore=%d containerAfter=%d pocketBefore=%d pocketAfter=%d act=%d total=%d verdict=%s%s%s "
            "| X(cls=%ls key=%ls name=%ls) | the verdict is the one to read; mp.py's sum of localCountAfter is informational",
            role.c_str(), mode.c_str(), (go && shouldTake) ? 1 : 0, pb->phaseA, pb->phaseB, pb->contA, pb->contB,
            pb->pockA, pb->pockB, *act, j.total, j.verdict, *j.why ? " why=" : "", j.why, pb->xSig.className.c_str(),
            pb->xSig.key.c_str(), pb->xName.c_str());
}

DWORD WINAPI ContainerRaceThread(LPVOID /*arg*/) {
    RunContainerRace();
    return 0;
}

}  // namespace coop::director
