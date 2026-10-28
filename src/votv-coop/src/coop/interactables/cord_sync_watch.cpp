// coop/interactables/cord_sync_watch.cpp -- the cord lane's watches on cord_C's own script bodies, and the rules that
// read them. See coop/interactables/cord_sync.h.
//
// connect and disconnect are the edges of a link, whoever calls them: the host broadcasts what they leave, a client
// claims it when its player's own verbs ran them. actionOptionIndex (the E press that takes the cord in hand) and used
// (its release) say whose verb that is, and bound the drag the lane never applies a canonical inside. ReceiveTick
// tells the stretch break, ReceiveDestroyed a cord on its way out. A body is "running" when the gate's chain holds
// it (script_gate::IsBodyActive, which takes the UFunction, recorded by the body's own PRE).

#include "cord_sync_detail.h"

#include "coop/net/session.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld: a client's own world load runs natively

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>

namespace coop::cord_sync::detail {
namespace {

namespace sg = ue_wrap::script_gate;
namespace R = ue_wrap::reflection;

UE_FACT_FN(kRowConnect, "cord_sync", L"cord_C", L"connect", Declared);
UE_FACT_FN(kRowDisconnect, "cord_sync", L"cord_C", L"disconnect", Declared);
UE_FACT_FN(kRowAction, "cord_sync", L"cord_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowUsed, "cord_sync", L"cord_C", L"used", Declared);
UE_FACT_FN(kRowTick, "cord_sync", L"cord_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowDestroyed, "cord_sync", L"cord_C", L"ReceiveDestroyed", Declared);

// The bodies whose running IsBodyActive is asked of, recorded as each begins.
enum Body : int { kBodyAction, kBodyUsed, kBodyTick, kBodyDestroyed, kBodyCount };
ue_wrap::CachedObjRef g_bodyFn[kBodyCount];

void Record(int body, void* fn) {
    if (!g_bodyFn[body].Is(fn)) g_bodyFn[body].Set(fn);
}

bool Running(int body) {
    void* const fn = g_bodyFn[body].Get();
    return fn && sg::IsBodyActive(fn);
}

// The local player's own verbs on a cord: the press that takes it in hand and the release.
bool InPlayersVerb() { return Running(kBodyAction) || Running(kBodyUsed); }

bool g_applying = false;
// All six watches live; and the release's `pressed` parameter not found missing (it is read at the first release).
bool g_allLive = false;
bool g_pressedUnresolved = false;

// The cords the local player holds in hand. One hand, so a few slots are plenty; a latch past them is not set and the
// drag then ends at the release as if nothing were held.
constexpr int kMaxLatches = 4;
ue_wrap::CachedObjRef g_latch[kMaxLatches];

int LatchIndex(void* cord) {
    for (int i = 0; i < kMaxLatches; ++i)
        if (g_latch[i].Is(cord)) return i;
    return -1;
}

void SetLatch(void* cord) {
    if (LatchIndex(cord) >= 0) return;
    for (int i = 0; i < kMaxLatches; ++i) {
        if (g_latch[i].Get()) continue;
        g_latch[i].Set(cord);
        return;
    }
}

bool ClearLatch(void* cord) {
    const int i = LatchIndex(cord);
    if (i < 0) return false;
    g_latch[i].Reset();
    return true;
}

// ---- the bodies' PREs: each records its UFunction ----

sg::Verdict OnActionPre(const sg::Call& c) {
    Record(kBodyAction, c.function);
    return sg::Verdict::Run;
}

sg::Verdict OnUsedPre(const sg::Call& c) {
    Record(kBodyUsed, c.function);
    return sg::Verdict::Run;
}

sg::Verdict OnTickPre(const sg::Call& c) {
    Record(kBodyTick, c.function);
    return sg::Verdict::Run;
}

sg::Verdict OnDestroyedPre(const sg::Call& c) {
    Record(kBodyDestroyed, c.function);
    return sg::Verdict::Run;
}

// A client's copy of a cord that stretched past its length breaks itself on its tick: the tick's one disconnect, and
// neither of the player's verbs is running. The host decides when a cord breaks; the copy stays as the host has it. A
// cord ticks only while held or plugged, so a stretched copy repeats this until the host's next canonical.
sg::Verdict OnDisconnectPre(const sg::Call& c) {
    auto* s = Connected();
    if (!IsClient(s) || g_applying || !c.object) return sg::Verdict::Run;
    if (!coop::net_pump::IsInAnnouncedWorld(c.object)) return sg::Verdict::Run;
    if (!Running(kBodyTick) || InPlayersVerb()) return sg::Verdict::Run;
    if (++Stat().stretchRefused == 1)
        UE_LOGI("cord_sync: this client refuses its own copy's stretch break -- the host decides when a cord breaks");
    return sg::Verdict::Cancel;
}

// ---- the edges ----

// The host's cord changed by any caller; a client's changed by its player's verb, outside the lane's own apply.
void OnLinkPost(void* cord) {
    auto* s = Connected();
    if (!s || !cord) return;
    if (IsHost(s)) {
        OnHostLinkEdge(cord);
        return;
    }
    if (g_applying || !InPlayersVerb()) return;
    OnClientLinkEdge(cord);
}

void OnConnectPost(const sg::Call& c) { OnLinkPost(c.object); }

void OnDisconnectPost(const sg::Call& c) {
    void* const cord = c.object;
    auto* s = Connected();
    if (!s || !cord) return;
    // A cord on its way out unplugs as it goes: not an edge, and nothing it held applies.
    if (Running(kBodyDestroyed)) {
        ClearLatch(cord);
        OnCordDying(cord);
        return;
    }
    OnLinkPost(cord);
    if (!g_applying && ClearLatch(cord)) OnDragEnded(cord);
}

// The press took the cord in hand: the drag begins.
void OnActionPost(const sg::Call& c) {
    if (LaneLive() && Connected() && c.object) SetLatch(c.object);
}

// The release ends the drag; the press also fires used, with pressed true. A `pressed` that does not resolve would
// leave every latch unreleasable: the lane goes off as a unit instead, said once.
void OnUsedPost(const sg::Call& c) {
    static R::ParamOffset sPressed{L"pressed"};
    const int32_t off = sPressed.Of(c.function);
    if (off < 0) {
        if (!g_pressedUnresolved) {
            g_pressedUnresolved = true;
            ClearLatches();
            UE_LOGE("cord_sync: cord_C::used has no `pressed` parameter here -- the cord lane is off, each peer's "
                    "cord link stays its own");
        }
        return;
    }
    if (!c.locals || *(c.locals + off) != 0 || !c.object) return;
    if (ClearLatch(c.object)) OnDragEnded(c.object);
}

struct WatchDef {
    const ue_wrap::facts::Function* row;
    int tag;
    sg::PreFn pre;
    sg::PostFn post;
};
constexpr WatchDef kWatches[] = {
    { &kRowConnect,    0x43534E43 /*'CSNC'*/, nullptr,             &OnConnectPost },
    { &kRowDisconnect, 0x43534443 /*'CSDC'*/, &OnDisconnectPre,    &OnDisconnectPost },
    { &kRowAction,     0x4353414F /*'CSAO'*/, &OnActionPre,        &OnActionPost },
    { &kRowUsed,       0x43535553 /*'CSUS'*/, &OnUsedPre,          &OnUsedPost },
    { &kRowTick,       0x4353544B /*'CSTK'*/, &OnTickPre,          nullptr },
    { &kRowDestroyed,  0x43534453 /*'CSDS'*/, &OnDestroyedPre,     nullptr },
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));
bool g_registered = false;
bool g_refused[kWatchCount] = {};
int g_refusedCount = 0;
bool g_settled = false;

}  // namespace

ApplyScope::ApplyScope() : previous_(g_applying) { g_applying = true; }
ApplyScope::~ApplyScope() { g_applying = previous_; }

bool Dragging(void* cord) { return LatchIndex(cord) >= 0; }

bool LaneLive() { return g_allLive && !g_pressedUnresolved; }

void ClearLatches() {
    for (auto& l : g_latch) l.Reset();
}

void InstallWatches() {
    if (g_registered) return;
    g_registered = true;
    for (int i = 0; i < kWatchCount; ++i) {
        const WatchDef& w = kWatches[i];
        if (sg::WatchName(*w.row, w.tag, w.pre, w.post)) continue;
        g_refused[i] = true;
        ++g_refusedCount;
        UE_LOGE("cord_sync: the script-body gate refused the watch on %ls::%ls -- that part of a cord's link is "
                "each peer's own", w.row->Class(), w.row->Name());
    }
}

// A refused watch is settled dead from the start (Install said so once); the rest settle at the gate's name resolve.
void TickWatches() {
    if (g_settled || !g_registered) return;
    sg::ResolvePendingNames();
    int live = 0, settled = g_refusedCount;
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_refused[i]) continue;
        if (sg::NameWatchLive(*kWatches[i].row, kWatches[i].tag)) ++live;
        if (sg::NameWatchSettled(*kWatches[i].row, kWatches[i].tag)) ++settled;
    }
    if (live == kWatchCount) {
        g_settled = true;
        g_allLive = true;
        UE_LOGI("cord_sync: the cord gates are live (connect, disconnect, the press, the release, the tick, the "
                "destroy)");
    } else if (settled == kWatchCount) {
        g_settled = true;
        UE_LOGE("cord_sync: %d of %d cord gates are dead -- the cord lane is off, each peer's cord link stays its own",
                kWatchCount - live, kWatchCount);
    }
}

}  // namespace coop::cord_sync::detail
