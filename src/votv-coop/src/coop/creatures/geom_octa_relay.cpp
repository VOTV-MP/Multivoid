// coop/creatures/geom_octa_relay.cpp -- see coop/creatures/geom_octa_relay.h.

#include "coop/creatures/geom_octa_relay.h"

#include "coop/creatures/npc_mirror_brain.h"
#include "coop/creatures/npc_sync.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/actors/geom_octa.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/facts/function_row.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace coop::geom_octa_relay {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace UH = ue_wrap::ufunction_hook;
namespace GO = ue_wrap::geom_octa;
namespace EL = coop::element;
namespace B  = coop::npc_mirror_brain;
namespace N  = coop::net;

// The "fire" section's finish notify, the body the mirror's brain refuses and this lane replays; and the one native it
// makes that carries the answer: whether minSphere still overlaps the target.
UE_FACT_FN(kRowFireFinish, "geom_octa_relay", L"geomOcta_C", L"OnNotifyBegin_1DFC28904024A8FB71C3ACA9DBD440F9",
           Declared);
UE_FACT_FN(kRowIsOverlappingActor, "geom_octa_relay", L"PrimitiveComponent", L"IsOverlappingActor", Declared);

constexpr int kTagFinish = 0x4F435437;   // 'OCT7'

// The entries of the Octa's event graph a replay names (cooked byte offsets): minSphere's begin-overlap and `aggro`
// for an Aggro, "fire"'s finish notify for a verdict.
constexpr int32_t kEntryOverlap = 3766;
constexpr int32_t kEntryAggro = 3592;
constexpr int32_t kEntryFireFinish = 1092;

// Divergence from MTA, which resends a syncer's ped state every pulse so a lost packet is replaced by the next
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CPedSync.cpp:220, :305): these decisions are one-shot and paired by
// count, so a missing one costs the look of one attack, bounded by the waits below, and never an outcome.
// The "close" section is 2.1667 s, the longest an Octa stays busy after the host's own close finished: an Aggro that
// arrives first waits for the client's close, at most this long plus a margin. A verdict waits that long plus 2 s.
constexpr uint64_t kAggroHoldMs = 2167 + 500;
constexpr uint64_t kVerdictWaitMs = 2167 + 2000;
constexpr uint64_t kResolveMs = 2000;   // a real target that does not resolve (an element still materialising)

constexpr size_t kMaxOctas = 8;
constexpr size_t kMaxKept = 4;      // verdicts kept per Octa
constexpr size_t kMaxWaiting = 16;  // refused notifies awaiting a verdict, per Octa

// Why an event is refused at the boundary. One counter and one line per reason.
enum class Reject : uint8_t {
    None, BadOp, BadTargetKind, BadRefire, VerdictWithTarget, TargetWithoutKind, SlotRange, BadOctaEid, BadTargetEid,
    Count
};
constexpr const char* kReasonName[static_cast<int>(Reject::Count)] = {
    "none", "op outside {0, 1}", "targetKind outside {0, 1, 2}", "refire outside {0, 1} or set on an Aggro",
    "a FireVerdict that names a target", "a target on targetKind 0", "a slot at or past kMaxPeers",
    "an Octa id that is not host-allocated", "an element target that is not host-allocated"};

uint32_t g_rejected[static_cast<int>(Reject::Count)] = {};

// The first reason an honest host never produces, else None. Pure.
Reject Check(const N::GeomOctaEventPayload& p) {
    if (p.op != N::kGeomOctaOpAggro && p.op != N::kGeomOctaOpVerdict) return Reject::BadOp;
    if (p.targetKind > N::kGeomOctaTargetElement) return Reject::BadTargetKind;
    if (p.refire > 1 || (p.op == N::kGeomOctaOpAggro && p.refire != 0)) return Reject::BadRefire;
    if (p.op == N::kGeomOctaOpVerdict && (p.targetKind != N::kGeomOctaTargetNone || p.target != 0))
        return Reject::VerdictWithTarget;
    if (p.targetKind == N::kGeomOctaTargetNone && p.target != 0) return Reject::TargetWithoutKind;
    if (!EL::Registry::IsAllowedHostAllocatedEid(p.octaEid)) return Reject::BadOctaEid;
    if (p.targetKind == N::kGeomOctaTargetSlot && p.target >= static_cast<uint32_t>(N::kMaxPeers))
        return Reject::SlotRange;
    if (p.targetKind == N::kGeomOctaTargetElement && !EL::Registry::IsAllowedHostAllocatedEid(p.target))
        return Reject::BadTargetEid;
    return Reject::None;
}

// ---- per-Octa state, only for a live, marked geomOcta_C mirror ----

struct Kept {
    uint32_t n;
    uint8_t  refire;
};

struct OctaState {
    uint32_t eid = 0;   // 0: free

    // An Aggro waiting to be replayed (the newest; an older one unreplayed is superseded).
    bool     aggroPending = false;
    uint8_t  targetKind = 0;
    uint32_t target = 0;
    uint32_t aggroBase = 0;
    uint64_t heldAt = 0;         // when it reached the lane
    uint64_t unresolvedAt = 0;   // when its target first failed to resolve
    bool     heldSaid = false;

    // The pairing since the last Aggro, latest wins: the pending refused fire-finishes take the NEWEST min(P, K) kept
    // verdicts numbered past `done` (relative to `base`, modular), oldest first; older kept ones retire as superseded. A number
    // d = n - base; `done` is the highest d replayed or timed out.
    bool     haveBase = false;
    uint32_t base = 0;
    uint32_t done = 0;
    uint64_t waitAt[kMaxWaiting] = {};      // the refused notifies awaiting a verdict, oldest first (a ring)
    bool     pendingSaid[kMaxWaiting] = {};
    size_t   waitHead = 0, waitCount = 0;
    bool     supersededSaid = false;        // `verdict superseded` is said once per attack
    Kept     kept[kMaxKept] = {};
    size_t   keptCount = 0;
};
OctaState g_octas[kMaxOctas];
Counters  g_counters{};

bool g_watched = false;
void* g_overlapFn = nullptr;   // IsOverlappingActor, once its post hook is installed
bool  g_hookTried = false;     // tried once, at the first Aggro this client applies; a failure is latched

unsigned long long Now() { return static_cast<unsigned long long>(N::NowMs()); }

// A drop is counted every time and said once per (line, reason): a host that is wrong would otherwise fill the log. The
// reasons are literals, compared by pointer.
bool SaidOnce(const char* line, const char* why) {
    struct Said {
        const char* line;
        const char* why;
    };
    static Said said[16];
    static size_t count = 0;
    for (size_t i = 0; i < count; ++i)
        if (said[i].line == line && said[i].why == why) return true;
    if (count < sizeof(said) / sizeof(said[0])) said[count++] = Said{line, why};
    return false;
}

void LogDroppedVerdict(uint32_t eid, uint32_t n, const char* why) {
    ++g_counters.verdictDropped;
    if (SaidOnce("verdict", why)) return;
    UE_LOGI("[OCTA] verdict dropped eid=%u n=%u why=%s (counted; said once per reason) t=%llu", static_cast<unsigned>(eid),
            static_cast<unsigned>(n), why, Now());
}

OctaState* Find(uint32_t eid) {
    for (OctaState& o : g_octas)
        if (o.eid == eid) return &o;
    return nullptr;
}

OctaState* Take(uint32_t eid) {
    if (OctaState* o = Find(eid)) return o;
    for (OctaState& o : g_octas) {
        if (o.eid == 0) {
            o = OctaState{};
            o.eid = eid;
            return &o;
        }
    }
    return nullptr;
}

void TargetText(uint8_t kind, uint32_t v, char (&out)[40]) {
    if (kind == N::kGeomOctaTargetSlot) std::snprintf(out, sizeof(out), "slot:%u", static_cast<unsigned>(v));
    else if (kind == N::kGeomOctaTargetElement) std::snprintf(out, sizeof(out), "element:%u", static_cast<unsigned>(v));
    else std::snprintf(out, sizeof(out), "none");
}

// Is `actor` a live, marked mirror of the Octa class, by the class chain's names?
bool IsOctaMirror(void* actor) {
    static const B::EntryTable* const table = B::TableNamed(ue_wrap::profile::name::NpcClass_GeomOcta);
    return actor && table && B::IsMarkedMirror(actor) && B::ChainHasTable(R::ClassOf(actor), *table);
}

// The Octa mirror of host element `eid`, or null.
void* OctaMirrorOf(uint32_t eid) {
    void* actor = EL::LiveActorOfType(static_cast<EL::ElementId>(eid), EL::ElementType::Npc);
    return IsOctaMirror(actor) ? actor : nullptr;
}

// The host's target as this client has it: a player's counterpart is the body this client keeps for that slot (its own
// player for its own slot, else a puppet, the host's included), an element's is its mirror or twin. False for a target
// that is not here (yet).
bool ResolveTarget(uint8_t kind, uint32_t v, void*& out) {
    out = nullptr;
    if (kind == N::kGeomOctaTargetNone) return true;
    void* actor = nullptr;
    if (kind == N::kGeomOctaTargetSlot) {
        auto& reg = coop::players::Registry::Get();
        if (static_cast<uint8_t>(v) == reg.LocalPeerId()) {
            actor = reg.Local();
        } else {
            coop::RemotePlayer* rp = reg.Puppet(static_cast<uint8_t>(v));
            actor = rp ? rp->GetActor() : nullptr;
        }
    } else {
        const EL::Element* e = EL::Registry::Get().Get(static_cast<EL::ElementId>(v));
        actor = e ? e->LiveActor() : nullptr;
    }
    if (!actor || !R::IsLive(actor)) return false;
    out = actor;
    return true;
}

// ---- the overwrite of the one overlap read a verdict replay makes ----

struct Overwrite {
    bool    armed = false;
    void*   minSphere = nullptr;
    void*   graph = nullptr;
    uint8_t refire = 0;
    uint32_t writes = 0;   // the answers written during the armed replay
};
Overwrite g_ow;

// After a native IsOverlappingActor: writes the host's answer into the result, one byte (the native's bool), only for
// the call the replayed notify makes on this Octa's minSphere from its own graph in the frame the pass ran. Any other
// call passes untouched, and so does any call off the game thread: natives are reached from other threads.
void OnOverlapPost(void* context, void* /*source*/, void* /*result*/) {
    if (!ue_wrap::game_thread::IsGameThread()) return;
    if (!g_ow.armed || context != g_ow.minSphere) return;
    const UH::CallerFrame frame = UH::CurrentCallerFrame();
    if (frame.function != g_ow.graph || !frame.locals || frame.locals != B::ScopedEntryPass::Frame()) return;
    if (void* r = UH::CurrentResult()) {
        *static_cast<uint8_t*>(r) = g_ow.refire;
        ++g_ow.writes;
        ++g_counters.overwriteWrites;
    }
}

// Arms the overwrite for one replay and disarms it on every way out. With no hook installed there is nothing to arm,
// and the replay's overlap read is the client's own.
class ArmScope {
public:
    ArmScope(void* octa, uint8_t refire) {
        g_ow = Overwrite{true, GO::MinSphere(octa), GO::GraphFunction(octa), refire, 0};
        if (g_overlapFn) UH::SetArmed(g_overlapFn, &OnOverlapPost, true);
    }
    ~ArmScope() {
        if (g_overlapFn) UH::SetArmed(g_overlapFn, &OnOverlapPost, false);
        g_ow = Overwrite{};
    }
    ArmScope(const ArmScope&) = delete;
    ArmScope& operator=(const ArmScope&) = delete;
};

// The disarmed post hook on the overlap read, tried once, at the first Aggro this client applies: a client that never
// sees an Octa pays nothing for it. A failure is latched and said; the verdict replays then run without the answer.
void EnsureHook() {
    if (g_hookTried) return;
    g_hookTried = true;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowIsOverlappingActor);
    if (fn && UH::InstallPostHook(fn, &OnOverlapPost, /*armed=*/false)) g_overlapFn = fn.Fn();
    else UE_LOGE("geom_octa_relay: the IsOverlappingActor hook did not install -- a verdict is replayed without the host's "
                 "answer written into the overlap read");
}

// ---- the refused notify, counted ----

// Before the mirror's own "fire" finish notify: the body is refused by the brain, so the notify is queued, and Tick
// serves it with one of the host's kept verdicts (latest wins) or times it out. The lane's own replay of the same function is not counted, or a re-fire's
// replay would shift every later pairing by one.
sg::Verdict OnFinishPre(const sg::Call& call) {
    if (call.fromOurCode || !GO::IsFinishNotify(call) || !B::IsMarkedMirror(call.object)) return sg::Verdict::Run;
    const EL::ElementId eid = EL::Registry::Get().EidForActor(call.object);
    OctaState* st = eid == EL::kInvalidId ? nullptr : Find(static_cast<uint32_t>(eid));
    if (!st || !st->haveBase) return sg::Verdict::Run;
    if (st->waitCount == kMaxWaiting) {   // sixteen unanswered notifies: unreachable at one a 1.8 s with a 4.2 s wait
        LogDroppedVerdict(st->eid, 0, "too many refused notifies awaiting a verdict");
        return sg::Verdict::Run;
    }
    const size_t i = (st->waitHead + st->waitCount++) % kMaxWaiting;
    st->waitAt[i] = N::NowMs();
    st->pendingSaid[i] = false;
    return sg::Verdict::Run;
}

// ---- the replays ----

void SetBase(OctaState& st, uint32_t base) {
    st.haveBase = true;
    st.base = base;
    EnsureHook();   // the verdicts of this attack will need it, whether or not its own look is replayed
    st.done = 0;
    st.waitHead = st.waitCount = 0;
    for (bool& s : st.pendingSaid) s = false;
    st.supersededSaid = false;
    // Kept verdicts numbered at or below the new base belong to an attack already over.
    size_t w = 0;
    for (size_t i = 0; i < st.keptCount; ++i)
        if (static_cast<int32_t>(st.kept[i].n - base) > 0) st.kept[w++] = st.kept[i];
    st.keptCount = w;
}

// Kept verdicts retired because a newer one exists (latest wins), whether it was applied or only arrived: counted always,
// said once per attack. It is not a fault, so RelayDirty does not read it.
void NoteSuperseded(OctaState& st, uint32_t upto, uint32_t count) {
    g_counters.verdictSuperseded += count;
    if (st.supersededSaid) return;
    st.supersededSaid = true;
    UE_LOGI("[OCTA] verdict superseded eid=%u upto=%u (counted; said once per attack) t=%llu",
            static_cast<unsigned>(st.eid), static_cast<unsigned>(upto), Now());
}

void LogAggro(const char* what, const OctaState& st, const char* why) {
    char text[40];
    TargetText(st.targetKind, st.target, text);
    UE_LOGI("[OCTA] aggro %s eid=%u target=%s base=%u%s%s t=%llu", what, static_cast<unsigned>(st.eid), text,
            static_cast<unsigned>(st.aggroBase), why ? " why=" : "", why ? why : "", Now());
}

// An Aggro whose look is not replayed still sets the base and zeroes the count: its attack's outcomes arrive, and its
// verdicts are kept and then dropped by number at the next Aggro.
void SkipAggro(OctaState& st, const char* why) {
    SetBase(st, st.aggroBase);
    st.aggroPending = false;
    ++g_counters.aggroSkipped;
    LogAggro("skipped", st, why);
}

void ServeAggro(OctaState& st, void* octa, uint64_t now) {
    if (!st.aggroPending) return;
    if (GO::IsAnim(octa)) {   // busy: the previous attack's close has not finished here yet
        if (!st.heldSaid) {
            st.heldSaid = true;
            LogAggro("held", st, nullptr);
        }
        if (now - st.heldAt > kAggroHoldMs) SkipAggro(st, "the Octa stayed busy");
        return;
    }
    void* other = nullptr;
    if (!ResolveTarget(st.targetKind, st.target, other)) {
        if (!st.unresolvedAt) st.unresolvedAt = now;
        if (now - st.unresolvedAt > kResolveMs) SkipAggro(st, "the target did not resolve");
        return;
    }
    SetBase(st, st.aggroBase);
    st.aggroPending = false;
    bool ran = false;
    {
        B::ScopedEntryPass pass(octa, {kEntryOverlap, kEntryAggro});
        ran = GO::CallMinSphereBeginOverlap(octa, other);
    }
    if (ran) {
        ++g_counters.aggroReplayed;
        LogAggro("replayed", st, nullptr);
    } else {
        ++g_counters.aggroSkipped;
        LogAggro("skipped", st, "the overlap handler did not run");
    }
}

// Replays the refused notify with the host's answer in its overlap read: whether the call ran, and how many answers the
// hook wrote into the reads it made (one, when the overwrite works).
struct Replay {
    bool     ran = false;
    uint32_t wrote = 0;
};
Replay ReplayFinish(void* octa, uint8_t refire) {
    ArmScope arm(octa, refire);
    Replay r;
    {
        B::ScopedEntryPass pass(octa, {kEntryFireFinish});
        r.ran = GO::CallFireFinishNotify(octa);
    }
    r.wrote = g_ow.writes;
    return r;
}

void ServeVerdicts(OctaState& st, void* octa, uint64_t now) {
    if (!st.haveBase) return;
    while (st.waitCount > 0) {
        // The kept verdicts numbered past `done` (relative to the base, modular), ascending by that number.
        size_t order[kMaxKept];
        size_t k = 0;
        for (size_t i = 0; i < st.keptCount; ++i) {
            const uint32_t d = st.kept[i].n - st.base;
            if (static_cast<int32_t>(d) <= 0 || d <= st.done) continue;
            size_t j = k++;
            for (; j > 0 && st.kept[order[j - 1]].n - st.base > d; --j) order[j] = order[j - 1];
            order[j] = i;
        }
        const uint32_t want = st.base + st.done + 1;   // what is waited for when none is kept
        const size_t slot = st.waitHead;
        if (k > 0) {
            // Latest wins: the P pending notifies are served oldest first with the NEWEST min(P, K) kept verdicts in
            // ascending order, and the kept verdicts older than those retire as superseded, never as dropped. A
            // joiner's chain reaches its first notify long after the host began to re-fire (P = 1, K > 1: the newest).
            const size_t served = k < st.waitCount ? k : st.waitCount;
            const size_t retire = k - served;
            bool gone[kMaxKept] = {};
            for (size_t i = 0; i < retire; ++i) gone[order[i]] = true;
            const size_t at = order[retire];
            gone[at] = true;
            const Kept v = st.kept[at];
            size_t w = 0;
            for (size_t i = 0; i < st.keptCount; ++i)
                if (!gone[i]) st.kept[w++] = st.kept[i];
            st.keptCount = w;
            if (retire) NoteSuperseded(st, v.n, static_cast<uint32_t>(retire));
            st.done = v.n - st.base;
            st.waitHead = (st.waitHead + 1) % kMaxWaiting;
            --st.waitCount;
            const Replay r = ReplayFinish(octa, v.refire);
            if (r.ran) {
                ++g_counters.verdictApplied;
                UE_LOGI("[OCTA] verdict applied eid=%u n=%u refire=%u wrote=%u t=%llu", static_cast<unsigned>(st.eid),
                        static_cast<unsigned>(v.n), static_cast<unsigned>(v.refire), static_cast<unsigned>(r.wrote),
                        Now());
            } else {
                LogDroppedVerdict(st.eid, v.n, "the notify did not run");
            }
            continue;
        }
        if (!st.pendingSaid[slot]) {
            st.pendingSaid[slot] = true;
            UE_LOGI("[OCTA] verdict pending eid=%u n=%u t=%llu", static_cast<unsigned>(st.eid),
                    static_cast<unsigned>(want), Now());
        }
        if (now - st.waitAt[slot] <= kVerdictWaitMs) return;
        // No verdict in time: the look ends with the Octa idle (refire 0), and the next Aggro starts clean.
        ++st.done;
        st.waitHead = (st.waitHead + 1) % kMaxWaiting;
        --st.waitCount;
        if (ReplayFinish(octa, 0).ran) {
            ++g_counters.verdictTimeout;
            UE_LOGI("[OCTA] verdict timeout eid=%u n=%u t=%llu", static_cast<unsigned>(st.eid),
                    static_cast<unsigned>(want), Now());
        } else {
            LogDroppedVerdict(st.eid, want, "the notify did not run at its timeout");
        }
    }
}

void LogDroppedEvent(const N::GeomOctaEventPayload& p, const char* why) {
    ++g_counters.eventDropped;
    if (SaidOnce("event", why)) return;
    UE_LOGI("[OCTA] event dropped eid=%u op=%u why=%s (counted; said once per reason) t=%llu",
            static_cast<unsigned>(p.octaEid), static_cast<unsigned>(p.op), why, Now());
}

// The watch on the refused notify, once.
void EnsureWatched() {
    if (!g_watched) g_watched = sg::WatchName(kRowFireFinish, kTagFinish, &OnFinishPre, nullptr);
}

}  // namespace

void OnEvent(const N::GeomOctaEventPayload& p) {
    const Reject why = Check(p);
    if (why != Reject::None) {
        const uint32_t n = ++g_rejected[static_cast<int>(why)];
        if (n == 1)
            UE_LOGW("geom_octa_relay: a GeomOctaEvent was refused: %s (counted; said once per reason)",
                    kReasonName[static_cast<int>(why)]);
        return;
    }
    // The host sends none before the Octa's EntitySpawn, which rides the same lane: an Octa that is no live, marked
    // mirror here is destroyed, or the host is wrong. Nothing is reflected on anything else.
    if (!OctaMirrorOf(p.octaEid)) {
        LogDroppedEvent(p, "no live Octa mirror");
        return;
    }
    OctaState* st = Take(p.octaEid);
    if (!st) {
        LogDroppedEvent(p, "more Octas than tracked");
        return;
    }
    const uint64_t now = N::NowMs();
    if (p.op == N::kGeomOctaOpAggro) {
        ++g_counters.aggroReceived;
        g_counters.lastAggroBase = p.verdictNo;
    } else {
        ++g_counters.verdictReceived;
    }
    if (p.op == N::kGeomOctaOpAggro) {
        if (st->aggroPending) SkipAggro(*st, "superseded by a newer Aggro");
        st->aggroPending = true;
        st->targetKind = p.targetKind;
        st->target = p.target;
        st->aggroBase = p.verdictNo;
        st->heldAt = now;
        st->unresolvedAt = 0;
        st->heldSaid = false;
        return;
    }
    // A FireVerdict: kept until a refused notify is served (latest wins: Tick shows the newest, retires the older).
    // One numbered at or below the base, or already shown, is over.
    const uint32_t d = p.verdictNo - st->base;
    if (st->haveBase && (static_cast<int32_t>(d) <= 0 || d <= st->done)) {
        LogDroppedVerdict(p.octaEid, p.verdictNo, "at or below the base");
        return;
    }
    for (size_t i = 0; i < st->keptCount; ++i) {
        if (st->kept[i].n == p.verdictNo) {
            LogDroppedVerdict(p.octaEid, p.verdictNo, "already kept");
            return;
        }
    }
    if (st->keptCount == kMaxKept) {
        // The kept verdict nearest the base goes (the Default lane is ordered, so it is also the oldest: this is defensive).
        size_t ev = 0;
        for (size_t i = 1; i < st->keptCount; ++i)
            if (st->kept[i].n - st->base < st->kept[ev].n - st->base) ev = i;
        NoteSuperseded(*st, p.verdictNo, 1);
        for (size_t i = ev + 1; i < st->keptCount; ++i) st->kept[i - 1] = st->kept[i];
        --st->keptCount;
    }
    st->kept[st->keptCount++] = Kept{p.verdictNo, p.refire};
}

void Tick() {
    N::Session* s = coop::npc_sync::GetSession();
    if (!s || s->role() != N::Role::Client) return;
    EnsureWatched();
    uint64_t now = 0;   // read once, and only when an Octa is live
    for (OctaState& st : g_octas) {
        if (st.eid == 0) continue;
        void* octa = OctaMirrorOf(st.eid);
        if (!octa) {   // the Octa ended: its holds and counts go with it
            st = OctaState{};
            continue;
        }
        if (!now) now = N::NowMs();
        ServeAggro(st, octa, now);
        ServeVerdicts(st, octa, now);
    }
}

Counters GetCounters() { return g_counters; }

void OnDisconnect() {
    for (OctaState& o : g_octas) o = OctaState{};
    g_counters = Counters{};
}

}  // namespace coop::geom_octa_relay
