// coop/world/creature_event_relay.cpp -- see coop/world/creature_event_relay.h. The watch on lib_C::setEvent, its
// resolve, the per-viewer test, Install, OnDisconnect and the one primitive that calls setEvent by reflection; the
// host's half is creature_event_relay_host.cpp, the client's creature_event_relay_client.cpp.
//
// setEvent(isEventActive, deactivateAmbientTrack, __WorldContext) adds 1 to mainGamemode.activeEvents and appends
// the context to activeEvents_senders (true), or subtracts 1 and removes the context (false); a creature registers
// itself. The allowlisted creature classes that call it all pass deactivateAmbientTrack = false, the frame's zero.

#include "coop/world/creature_event_relay.h"
#include "creature_event_relay_internal.h"  // co-located private header (src tree, not include/)

#include "coop/net/session.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/active_events.h"

#include <atomic>
#include <cstdint>

namespace coop::creature_event_relay {
namespace detail {
void* g_applyingCtx = nullptr;
}  // namespace detail

namespace {

namespace R  = ue_wrap::reflection;
namespace AE = ue_wrap::active_events;
namespace sg = ue_wrap::script_gate;
namespace EL = coop::element;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr int kTagCevr = 0x43455652;  // 'CEVR'
UE_FACT_FN(kRowSetEvent, "creature_event_relay", nullptr, L"setEvent", AnyClass);
bool g_watchInstalled = false;
bool g_watchLive = false;
bool g_subscribed = false;
bool g_announced = false;
int  g_subscribeRetry = 0;
bool g_subscribeWarned = false;
int  g_libCdoRetry = 0;
bool g_libCdoWarned = false;

void*   g_setEventFn = nullptr;   // lib_C::setEvent, resolved from its class once it has loaded
int32_t g_offActive = -1;         // isEventActive
int32_t g_offContext = -1;        // __WorldContext
ue_wrap::CachedObjRef g_libCdo;   // the library's default object, which outlives every world

// lib_C::setEvent and its two read parameters, from the library's class once it has loaded: a class that has not
// loaded costs an index lookup, never a walk.
bool ResolveSetEvent() {
    static bool s_failed = false;  // the class loaded without the function or its parameters: said once
    if (g_offContext >= 0) return true;
    if (s_failed) return false;
    void* cls = ue_wrap::object_index::ClassByName(L"lib_C");
    if (!cls) return false;
    g_setEventFn = R::FindFunction(cls, L"setEvent");
    g_offActive = g_setEventFn ? R::FindParamOffset(g_setEventFn, L"isEventActive") : -1;
    const int32_t ctx = g_setEventFn ? R::FindParamOffset(g_setEventFn, L"__WorldContext") : -1;
    if (g_offActive < 0 || ctx < 0) {
        UE_LOGW("creature_event_relay: lib_C::setEvent did not resolve (fn=%p isEventActive=%d __WorldContext=%d) -- "
                "no creature event is seen or applied", g_setEventFn, g_offActive, ctx);
        g_setEventFn = nullptr;
        s_failed = true;
        return false;
    }
    g_offContext = ctx;
    return true;
}

// The four classes whose event is computed from the local camera, render state or player 0 (each peer's own
// perception), compared by FName; resolved on the game thread on first use. MTA marks sync per datum
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:1024-1035); here the class
// decides, from its bytecode.
struct PerViewerClass {
    const wchar_t* name;
    R::FName       fname;
};
PerViewerClass g_perViewer[] = {
    { L"killerwisp_C" },     // rendered = player 0 within 5000 and not avoidPlayer (killerwisp.cpp:225-267)
    { L"fossilhound_C" },    // rendered = camera 0 within 10000 (the Blueprint listing of fossilhound_C, lines 252-281)
    { L"antibreather_C" },   // rendered = camera 0 within 10000 (antibreather.cpp:944-952)
    { L"eg_C" },             // rendered = WasRecentlyRendered(0.5) (eg.cpp:124-136)
};
bool g_perViewerResolved = false;

bool ResolvePerViewerNames() {
    if (g_perViewerResolved) return true;
    if (!ue_wrap::fname_utils::Resolved()) return false;
    for (PerViewerClass& c : g_perViewer) {
        c.fname = ue_wrap::fname_utils::StringToFName(c.name);
        if (c.fname.ComparisonIndex == 0 && c.fname.Number == 0) return false;
    }
    g_perViewerResolved = true;
    return true;
}

void* ReadContext(const sg::Call& call) { return *reinterpret_cast<void* const*>(call.locals + g_offContext); }

// Before the body runs: a client refuses its bound mirror's own call. There is no session test here: the disconnect
// drain destroys the mirrors after the session stopped and before this gate is released (subsystems.cpp
// DisconnectAll runs the NPC drain first), so a mirror's own false must still be refused while the relay retracts its
// entry once. ClientPre cancels only a call the relay holds a bound entry for; every other call runs natively.
sg::Verdict OnSetEventPre(const sg::Call& call) {
    if (!ResolveSetEvent() || call.function != g_setEventFn) return sg::Verdict::Run;
    return detail::ClientPre(ReadContext(call), call.locals[g_offActive] != 0) ? sg::Verdict::Cancel : sg::Verdict::Run;
}

// After the game's own bookkeeping ran: the host counts the creature's call.
void OnSetEventPost(const sg::Call& call) {
    if (!detail::ActiveSession(coop::net::Role::Host)) return;
    if (!ResolveSetEvent() || call.function != g_setEventFn) return;
    detail::HostPost(ReadContext(call), call.locals[g_offActive] != 0);
}

// An element's end of play from the death seam: a mirror's to the client half, a host creature's to the host half.
void OnNpcEnd(const EL::death_seam::Death& death) {
    UE_ASSERT_GAME_THREAD("creature_event_relay::OnNpcEnd");
    if (death.mirror) detail::ClientOnNpcEnd(death);
    else detail::HostOnNpcEnd(death);
}

}  // namespace

namespace detail {

coop::net::Session* ActiveSession(coop::net::Role role) {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->connected() && s->role() == role) ? s : nullptr;
}

bool IsPerViewer(void* actor) {
    if (!actor || !ResolvePerViewerNames()) return false;
    // A creature's class chain is a handful deep; the walk stops after 16 hops as the engine's own chain readers do.
    int hops = 0;
    for (void* cls = R::ClassOf(actor); cls && hops < 16; cls = R::SuperStructOf(cls), ++hops) {
        const R::FName& n = R::NameOf(cls);
        for (const PerViewerClass& c : g_perViewer)
            if (n.ComparisonIndex == c.fname.ComparisonIndex && n.Number == c.fname.Number) return true;
    }
    return false;
}

bool Ready() { return ResolveSetEvent() && AE::EnsureResolved() && g_libCdo.Get() != nullptr; }

bool Call(bool active, void* ctx) {
    if (!ctx || !Ready()) return false;
    // The default object was resolved by Install (Ready holds it); this never walks the object array.
    void* const cdo = g_libCdo.Get();
    int32_t n = 0;
    if (!active) {
        if (!AE::ReadCount(n)) return false;  // no gamemode: nothing to retract from
        if (n <= 0) {
            UE_LOGW("creature_event_relay: a false at count 0 skipped");
            return true;
        }
    }
    ue_wrap::ParamFrame f(g_setEventFn);
    if (!f.valid()) {
        UE_LOGE("creature_event_relay: the setEvent frame is invalid");
        return false;
    }
    f.Set<bool>(L"isEventActive", active);
    f.Set<void*>(L"__WorldContext", ctx);
    void* const prev = g_applyingCtx;
    g_applyingCtx = ctx;
    const bool ok = ue_wrap::Call(cdo, f);
    g_applyingCtx = prev;
    if (!ok) {
        UE_LOGE("creature_event_relay: the setEvent(%d) call on ctx=%p failed", active ? 1 : 0, ctx);
        return false;
    }
    AE::ReadCount(n);
    UE_LOGI("creature_event_relay: %c ctx=%ls count=%d", active ? '+' : '-', R::ClassNameOf(ctx).c_str(), n);
    return true;
}

}  // namespace detail

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // Called every pump tick by the install fanout, which is also the retry until each piece latches.
    if (!g_watchInstalled)
        g_watchInstalled = sg::WatchName(kRowSetEvent, kTagCevr, &OnSetEventPre, &OnSetEventPost);
    if (g_watchInstalled && !g_watchLive) {
        sg::ResolvePendingNames();
        g_watchLive = sg::NameWatchLive(kRowSetEvent, kTagCevr);
    }
    // The library's default object, once setEvent has resolved: finding it walks the whole object array, so it is
    // made once and a failed find is retried on every 120th call.
    if (ResolveSetEvent() && !g_libCdo.Alive()) {
        if (g_libCdoRetry > 0) {
            --g_libCdoRetry;
        } else {
            g_libCdo.Set(R::FindClassDefaultObject(L"lib_C"));
            if (!g_libCdo.Alive()) {
                if (!g_libCdoWarned) {
                    g_libCdoWarned = true;
                    UE_LOGW("creature_event_relay: lib_C's default object is not found -- setEvent cannot be applied");
                }
                g_libCdoRetry = 120;
            }
        }
    }
    // A refused Subscribe is said once, then retried quietly on every 120th call.
    if (!g_subscribed) {
        if (g_subscribeRetry > 0) {
            --g_subscribeRetry;
        } else if (EL::death_seam::Install()) {
            g_subscribed = EL::death_seam::Subscribe(EL::ElementType::Npc, &OnNpcEnd);
            if (!g_subscribed) {
                if (!g_subscribeWarned) {
                    g_subscribeWarned = true;
                    UE_LOGW("creature_event_relay: the death seam took no subscription for creatures -- retried");
                }
                g_subscribeRetry = 120;
            }
        }
    }
    if (g_watchLive && g_subscribed && !g_announced) {
        g_announced = true;
        UE_LOGI("creature_event_relay: every creature's setEvent is seen (a script-gate watch) and every creature's "
                "end is heard (the death seam)");
    }
}

void SendJoinSnapshotForSlot(int slot) {
    if (auto* s = detail::ActiveSession(coop::net::Role::Host)) detail::HostSendJoinSnapshot(s, slot);
}

void OnNpcEnrolled(EL::ElementId eid, void* actor) {
    UE_ASSERT_GAME_THREAD("creature_event_relay::OnNpcEnrolled");
    detail::HostOnNpcEnrolled(eid, actor);
}

void OnMirrorBound(EL::ElementId eid, void* actor) {
    UE_ASSERT_GAME_THREAD("creature_event_relay::OnMirrorBound");
    detail::ClientOnMirrorBound(eid, actor);
}

void OnReliable(const coop::net::CreatureEventActivePayload& payload) {
    UE_ASSERT_GAME_THREAD("creature_event_relay::OnReliable");
    detail::ClientOnReliable(payload);
}

void OnDisconnect() {
    detail::HostOnDisconnect();
    detail::ClientOnDisconnect();
    g_session.store(nullptr, std::memory_order_release);
}

int TargetFor(EL::ElementId eid) { return detail::ClientTargetFor(eid); }
int AppliedFor(EL::ElementId eid) { return detail::ClientAppliedFor(eid); }
uint32_t PerViewerTrues(EL::ElementId eid) { return detail::ClientViewerTrues(eid); }
uint32_t PerViewerFalses(EL::ElementId eid) { return detail::ClientViewerFalses(eid); }

}  // namespace coop::creature_event_relay
