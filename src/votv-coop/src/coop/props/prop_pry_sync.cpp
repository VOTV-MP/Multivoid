// coop/props/prop_pry_sync.cpp -- see coop/props/prop_pry_sync.h for the design.

#include "coop/props/prop_pry_sync.h"

#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
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
#include <unordered_set>

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

// The keys this peer holds as pried: the host's own record of what it ran, a client's of what the host told it.
std::unordered_set<std::wstring> g_pried;
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
    if (key.empty() || !IsPried(key)) return sg::Verdict::Run;
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

bool IsPried(const std::wstring& key) { return g_pried.find(key) != g_pried.end(); }

void MarkPried(void* prop) {
    UE_ASSERT_GAME_THREAD("prop_pry_sync::MarkPried");
    auto* s = LaneSession();
    if (!s || s->role() != coop::net::Role::Host) return;
    const std::wstring key = KeyOf(prop);
    if (key.empty()) {
        UE_LOGW("prop_pry_sync: pried prop %p has no key -- the clients are not told", prop);
        return;
    }
    if (!g_pried.insert(key).second) return;
    coop::net::PryStatePayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.flags = coop::net::pry_flags::kPried;
    s->SendReliable(coop::net::ReliableKind::PryState, &p, sizeof(p));
    ++g_stats.sent;
    UE_LOGI("prop_pry_sync: PRIED '%ls' -- told the clients (#%llu)", key.c_str(),
            static_cast<unsigned long long>(g_stats.sent));
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
    if (!(payload.flags & coop::net::pry_flags::kPried) || !g_pried.insert(key).second) return;
    // The deterministic part of the pry: the flags a plank's crowbarOpen clears and the sound it plays. The kick is the
    // host's body alone; this copy takes the fall from the host.
    const bool freed = ue_wrap::prop::CallBaseSetPropProps(prop, false, false, false);
    coop::prop_sound::PlayDamagedSound(prop);
    ++g_stats.applied;
    UE_LOGI("prop_pry_sync: PRY applied '%ls' (%s; #%llu)", key.c_str(),
            freed ? "flags cleared" : "setPropProps did not run", static_cast<unsigned long long>(g_stats.applied));
}

Stats GetStats() { return g_stats; }

void OnDisconnect() {
    if (g_stats.sent || g_stats.applied || g_stats.asked || g_stats.twiceCancelled || g_stats.loadNative ||
        g_stats.unresolved)
        UE_LOGI("prop_pry_sync: session end -- sent=%llu applied=%llu asked=%llu second-pry cancelled=%llu own load "
                "run natively=%llu unresolved states=%llu",
                static_cast<unsigned long long>(g_stats.sent), static_cast<unsigned long long>(g_stats.applied),
                static_cast<unsigned long long>(g_stats.asked), static_cast<unsigned long long>(g_stats.twiceCancelled),
                static_cast<unsigned long long>(g_stats.loadNative), static_cast<unsigned long long>(g_stats.unresolved));
    g_stats = Stats{};
    g_pried.clear();
    g_nextNoKeySayMs = 0;
}

}  // namespace coop::prop_pry_sync
