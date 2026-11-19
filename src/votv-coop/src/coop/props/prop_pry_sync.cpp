// coop/props/prop_pry_sync.cpp -- see coop/props/prop_pry_sync.h for the design.

#include "coop/props/prop_pry_sync.h"

#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/props/prop_drive_host.h"  // CoastLetGo: the freed body is the host's coast
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_sound.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld: a client's own world load runs natively
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_events.h"
#include "ue_wrap/actors/prop_pry.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>

namespace coop::prop_pry_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace PP = ue_wrap::prop_pry;

// Watched by name: every class's crowbarOpen fires, so the callbacks gate on the object's class first.
UE_FACT_FN(kRowCrowbarOpen, "prop_pry_sync", nullptr, L"crowbarOpen", AnyClass);
constexpr int kTagPry = 0x50505259;  // 'PPRY'

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchAsked = false;

// The keys this peer holds as pried, each with the joints (bit N-1 for PhysicsConstraintN) it has broken: the host's own
// record of what it ran, a client's of what the host told it.
struct Rec {
    uint8_t mask = 0;
};
std::unordered_map<std::wstring, Rec> g_rec;

// Inside RunOwn: the host's copy is running a pry it granted, which the second-pry cancel does not stop. A depth,
// held around that one call alone; game thread, as the gate's callbacks are.
int g_grantDepth = 0;

Stats g_stats{};
uint64_t g_nextNoKeySayMs = 0;

// The session this lane works in: the host's runs with no client yet, a client's is connected to the host.
coop::net::Session* LaneSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return nullptr;
    return (s->role() == coop::net::Role::Host || s->connected()) ? s : nullptr;
}

// The crowbar the event names: the prying crowbar actor, whose right vector a plank's pry kicks along.
void* CrowbarOf(const sg::Call& c) {
    static R::ParamOffset sOff(L"pryingCrowbar");
    const int32_t off = c.locals && c.function ? sOff.Of(c.function) : -1;
    return off < 0 ? nullptr : *reinterpret_cast<void* const*>(c.locals + off);
}

// The pry a client would run is asked of the host instead. The body is cancelled whatever the answer: it never moves
// this copy, and the host's PryState does. Cancelling leaves the minigame's end() to drop the crowbar, as it does when a
// pry opens nothing. A prop with no key has nothing to name it by, so its pry runs here alone, said.
sg::Verdict ClientPre(coop::net::Session& s, const sg::Call& c, const std::wstring& key) {
    // A call on an object of a world this client has not announced ready is that world's load, the host's save: it runs
    // natively, and the host's state of what the lane owns follows the announce.
    if (!coop::net_pump::IsInAnnouncedWorld(c.object)) {
        ++g_stats.loadNative;
        return sg::Verdict::Run;
    }
    if (key.empty()) {
        const uint64_t now = coop::net::NowMs();
        if (now >= g_nextNoKeySayMs) {
            g_nextNoKeySayMs = now + 3000;
            UE_LOGW("prop_pry_sync: a pry of %p, which has no key the host could resolve, runs here alone", c.object);
        }
        return sg::Verdict::Run;
    }
    if (IsPried(key)) {
        ++g_stats.twiceCancelled;
        UE_LOGI("prop_pry_sync: a second pry of '%ls' cancelled -- the host already has it pried", key.c_str());
        return sg::Verdict::Cancel;
    }
    coop::net::PryAskPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    float right[3] = {0.f, 0.f, 0.f};
    PP::ReadRightVector(CrowbarOf(c), right);  // unread: no kick, which the host's plank takes as none
    p.kickX = right[0]; p.kickY = right[1]; p.kickZ = right[2];
    if (!s.SendReliable(coop::net::ReliableKind::PryAsk, &p, sizeof(p))) {
        UE_LOGW("prop_pry_sync: the pry of '%ls' was not sent (the session refused it); the prop stays as the host "
                "has it", key.c_str());
        return sg::Verdict::Cancel;
    }
    ++g_stats.asked;
    UE_LOGI("prop_pry_sync: ASKED the host to pry '%ls' (#%llu)", key.c_str(),
            static_cast<unsigned long long>(g_stats.asked));
    return sg::Verdict::Cancel;
}

sg::Verdict OnCrowbarPre(const sg::Call& c) {
    auto* s = LaneSession();
    if (!s || PP::KindOf(c.object) == PP::Kind::None) return sg::Verdict::Run;
    const std::wstring key = KeyOf(c.object);
    if (s->role() == coop::net::Role::Client) return ClientPre(*s, c, key);
    // The host runs its own pry, and cancels only a second one of a pried key: a minigame that ends after the host
    // granted another player's pry of the same prop.
    if (key.empty() || g_grantDepth > 0 || !IsPried(key)) return sg::Verdict::Run;
    ++g_stats.twiceCancelled;
    UE_LOGI("prop_pry_sync: a second pry of '%ls' cancelled -- it is already pried here", key.c_str());
    return sg::Verdict::Cancel;
}

// The host's own pry has run on its copy.
void OnCrowbarPost(const sg::Call& c) {
    auto* s = LaneSession();
    if (!s || s->role() != coop::net::Role::Host || PP::KindOf(c.object) == PP::Kind::None) return;
    MarkPried(c.object);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watchAsked) return;
    g_watchAsked = true;
    if (!sg::WatchName(kRowCrowbarOpen, kTagPry, &OnCrowbarPre, &OnCrowbarPost))
        UE_LOGE("prop_pry_sync: the script gate refused the watch on crowbarOpen -- for the rest of this process a "
                "pried plank changes only where it is pried");
}

std::wstring KeyOf(void* prop) {
    std::wstring key = ue_wrap::prop::GetInteractableKeyString(prop);
    return key == L"None" ? std::wstring() : key;
}

bool IsPried(const std::wstring& key) { return g_rec.find(key) != g_rec.end(); }

namespace {

// The whole state of `key`, to every client.
void PublishState(coop::net::Session& s, const std::wstring& key, uint8_t mask) {
    coop::net::PryStatePayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.flags = coop::net::pry_flags::kPried;
    p.brokenMask = mask;
    s.SendReliable(coop::net::ReliableKind::PryState, &p, sizeof(p));
    ++g_stats.sent;
    UE_LOGI("prop_pry_sync: PRIED '%ls' mask=%02x -- told the clients (#%llu)", key.c_str(),
            static_cast<unsigned>(mask), static_cast<unsigned long long>(g_stats.sent));
}

}  // namespace

void MarkPried(void* prop) {
    UE_ASSERT_GAME_THREAD("prop_pry_sync::MarkPried");
    auto* s = LaneSession();
    if (!s || s->role() != coop::net::Role::Host) return;
    const std::wstring key = KeyOf(prop);
    if (key.empty()) {
        UE_LOGW("prop_pry_sync: pried prop %p has no key -- the clients are not told", prop);
        return;
    }
    if (!g_rec.try_emplace(key).second) return;
    PublishState(*s, key, 0);
    // The freed body falls on the host alone: every copy parks on the host's first pose and follows its stream, and
    // snaps to its rest at the stream's end, so no peer simulates the fall (MTA: an element's motion is the server's).
    coop::prop_drive_host::CoastLetGo(prop, "pry");
}

void NoteBroken(void* prop, int joint) {
    UE_ASSERT_GAME_THREAD("prop_pry_sync::NoteBroken");
    auto* s = LaneSession();
    if (!s || s->role() != coop::net::Role::Host || joint < 1 || joint > ue_wrap::prop_pry::kJoints) return;
    const std::wstring key = KeyOf(prop);
    if (key.empty()) {
        UE_LOGW("prop_pry_sync: pried prop %p has no key -- its broken joint %d is not told", prop, joint);
        return;
    }
    Rec& r = g_rec[key];
    const uint8_t bit = static_cast<uint8_t>(1u << (joint - 1));
    if (r.mask & bit) return;
    r.mask |= bit;
    PublishState(*s, key, r.mask);
}

void StampRow(void* prop, uint8_t& physFlags, uint8_t& pryMask) {
    if (g_rec.empty()) return;  // a snapshot's rows are many and a pried prop is rare: no key read for each
    auto* s = LaneSession();
    if (!s || s->role() != coop::net::Role::Host) return;
    const auto it = g_rec.find(KeyOf(prop));
    if (it == g_rec.end()) return;
    physFlags |= coop::net::propspawn_flags::kPried;
    pryMask = it->second.mask;
}

void ConvergePried(void* actor, uint8_t physFlags, uint8_t pryMask, int senderSlot) {
    UE_ASSERT_GAME_THREAD("prop_pry_sync::ConvergePried");
    if (!(physFlags & coop::net::propspawn_flags::kPried) || senderSlot != 0) return;
    auto* s = LaneSession();
    if (!s || s->role() != coop::net::Role::Client) return;
    const PP::Kind kind = PP::KindOf(actor);
    const std::wstring key = KeyOf(actor);
    if (kind == PP::Kind::None || key.empty()) {
        UE_LOGW("prop_pry_sync: a row says '%ls' is pried, but %p is no prop this lane pries (class '%ls')", key.c_str(),
                actor, R::ClassNameOf(actor).c_str());
        return;
    }
    Rec& r = g_rec[key];
    if (kind != PP::Kind::Hanging) return;
    for (int n = 1; n <= PP::kJoints; ++n) {
        const uint8_t bit = static_cast<uint8_t>(1u << (n - 1));
        if (!(pryMask & bit) || (r.mask & bit)) continue;
        r.mask |= bit;
        if (!PP::BreakJoint(actor, n, false))
            UE_LOGW("prop_pry_sync: joint %d of '%ls' did not break from its row -- the copy hangs on it", n, key.c_str());
        ++g_stats.jointsBroken;
    }
}

bool RunOwn(void* prop) {
    UE_ASSERT_GAME_THREAD("prop_pry_sync::RunOwn");
    ++g_grantDepth;
    const bool ran = ue_wrap::prop::CallCrowbarOpen(prop);
    --g_grantDepth;
    return ran;
}

void OnState(const coop::net::PryStatePayload& payload) {
    UE_ASSERT_GAME_THREAD("prop_pry_sync::OnState");
    const std::wstring key = coop::net::StringFromWireKey(payload.key);
    void* prop = coop::prop_key_index::Find(key);
    if (!prop || !R::IsLive(prop)) {
        const uint64_t n = ++g_stats.unresolved;
        if (n <= 3 || (n & (n - 1)) == 0)
            UE_LOGW("prop_pry_sync: PryState for '%ls' names no prop here -- dropped (#%llu)", key.c_str(),
                    static_cast<unsigned long long>(n));
        return;
    }
    const PP::Kind kind = PP::KindOf(prop);
    if (kind == PP::Kind::None) {
        UE_LOGW("prop_pry_sync: PryState for '%ls' names a prop of class '%ls', which this lane does not pry -- dropped",
                key.c_str(), R::ClassNameOf(prop).c_str());
        return;
    }
    // A joint in the mask is a pry in progress, whatever the flag says.
    if (!(payload.flags & coop::net::pry_flags::kPried) && payload.brokenMask == 0) return;
    const auto [it, first] = g_rec.try_emplace(key);
    if (first) {
        // The deterministic part of the pry, which every peer runs alike: the flags, and what the prop's own body does
        // at that moment outside the shuffle and the joint loop -- a plank's sound, a hanging prop's break effect. The
        // plank's kick and the loop's random spin are the host's body alone; this copy takes the fall from the host. No
        // peer calls SetConstrainedComponents or the shuffle, so a joint once broken is never revived.
        const bool freed = ue_wrap::prop::CallBaseSetPropProps(prop, false, false, false);
        if (kind == PP::Kind::Plank) coop::prop_sound::PlayDamagedSound(prop);
        else PP::CallBreakEffect(prop);
        ++g_stats.applied;
        UE_LOGI("prop_pry_sync: PRY applied '%ls' (%s; #%llu)", key.c_str(),
                freed ? "flags cleared" : "setPropProps did not run", static_cast<unsigned long long>(g_stats.applied));
    }
    if (kind != PP::Kind::Hanging) return;
    for (int n = 1; n <= PP::kJoints; ++n) {
        const uint8_t bit = static_cast<uint8_t>(1u << (n - 1));
        if (!(payload.brokenMask & bit) || (it->second.mask & bit)) continue;
        it->second.mask |= bit;
        if (!PP::BreakJoint(prop, n, true))
            UE_LOGW("prop_pry_sync: joint %d of '%ls' did not break here -- the copy hangs on it", n, key.c_str());
        ++g_stats.jointsBroken;
    }
}

Stats GetStats() { return g_stats; }

void OnDisconnect() {
    if (g_stats.sent || g_stats.applied || g_stats.jointsBroken || g_stats.asked || g_stats.twiceCancelled || g_stats.loadNative ||
        g_stats.unresolved)
        UE_LOGI("prop_pry_sync: session end -- sent=%llu applied=%llu joints broken=%llu asked=%llu second-pry cancelled=%llu own load "
                "run natively=%llu unresolved states=%llu",
                static_cast<unsigned long long>(g_stats.sent), static_cast<unsigned long long>(g_stats.applied),
                static_cast<unsigned long long>(g_stats.jointsBroken),
                static_cast<unsigned long long>(g_stats.asked), static_cast<unsigned long long>(g_stats.twiceCancelled),
                static_cast<unsigned long long>(g_stats.loadNative), static_cast<unsigned long long>(g_stats.unresolved));
    g_stats = Stats{};
    g_rec.clear();
    g_grantDepth = 0;
    g_nextNoKeySayMs = 0;
}

}  // namespace coop::prop_pry_sync
