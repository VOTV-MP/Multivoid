// coop/props/prop_pry_sync.cpp -- see coop/props/prop_pry_sync.h for the design.

#include "coop/props/prop_pry_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"  // the pry drill's red arms
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
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

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
    bool freed = false;   // this copy's flags are cleared (a client: by a state or by a row)
    bool cued = false;    // the moment's cue has played here: a plank's sound, a hanging prop's break effect
    ue_wrap::CachedObjRef prop;   // the host's: the prop the record was made for, so a row finds it with no key read
};
std::unordered_map<std::wstring, Rec> g_rec;

// Inside RunOwn: the host's copy is running a pry it granted, which the second-pry cancel does not stop. A depth,
// held around that one call alone; game thread, as the gate's callbacks are.
int g_grantDepth = 0;

Stats g_stats{};
std::vector<std::wstring> g_keylessSaid;  // classes whose keyless pry was cancelled and said

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
// pry opens nothing. A prop with no key cannot be named to the host, so its pry is cancelled too, said once per class:
// running it here would free this copy alone.
sg::Verdict ClientPre(coop::net::Session& s, const sg::Call& c, const std::wstring& key) {
    // The pried drill's client red arm: the client runs its own pry and asks nobody. Resolved once.
    static const bool s_askOff = coop::config::ResolveFlag(coop::config_registry::rows::pry_ask_off);
    if (s_askOff) return sg::Verdict::Run;
    // A call on an object of a world this client has not announced ready is that world's load, the host's save: it runs
    // natively, and the host's state of what the lane owns follows the announce.
    if (!coop::net_pump::IsInAnnouncedWorld(c.object)) {
        ++g_stats.loadNative;
        return sg::Verdict::Run;
    }
    if (key.empty()) {
        const std::wstring cls = R::ClassNameOf(c.object);
        if (std::find(g_keylessSaid.begin(), g_keylessSaid.end(), cls) == g_keylessSaid.end()) {
            g_keylessSaid.push_back(cls);
            UE_LOGW("prop_pry_sync: a pry of a '%ls' with no key is cancelled -- the host cannot be asked to pry what it "
                    "cannot name (said once a class)", cls.c_str());
        }
        return sg::Verdict::Cancel;
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
    // The pried drill's twice red arm: a second pry of a pried prop runs its body again. Resolved once.
    static const bool s_againOff = coop::config::ResolveFlag(coop::config_registry::rows::pry_again_off);
    if (!s_againOff) ForgetIfStale(c.object);
    if (s_againOff || key.empty() || g_grantDepth > 0 || !IsPried(key)) return sg::Verdict::Run;
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

void ForgetIfStale(void* prop) {
    if (g_rec.empty()) return;
    const auto it = g_rec.find(KeyOf(prop));
    // Static again only: a pried prop the toolgun froze is still pried, not another world's.
    if (it != g_rec.end() && ue_wrap::prop::IsStatic(prop)) g_rec.erase(it);
}

namespace {

// The whole state of `key`, to every client.
void PublishState(coop::net::Session& s, const std::wstring& key, uint8_t mask) {
    coop::net::PryStatePayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.flags = coop::net::pry_flags::kPried;
    p.brokenMask = mask;
    if (!s.SendReliable(coop::net::ReliableKind::PryState, &p, sizeof(p))) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGW("prop_pry_sync: the session refused PryState for '%ls' -- the clients are not told (said once)", key.c_str());
        }
        return;
    }
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
    const auto made = g_rec.try_emplace(key);
    if (!made.second) return;
    made.first->second.prop.Set(prop);
    PublishState(*s, key, 0);
    // The freed body falls on the host alone: every copy parks on the host's first pose and follows its stream, and
    // snaps to its rest at the stream's end, so no peer simulates the fall (MTA: an element's motion is the server's).
    // The pried drill's coast red arm hands the body to no stream: each peer simulates the fall. Resolved once.
    static const bool s_coastOff = coop::config::ResolveFlag(coop::config_registry::rows::pry_coast_off);
    if (!s_coastOff) coop::prop_drive_host::CoastLetGo(prop, "pry");
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
    if (!r.prop.Alive()) r.prop.Set(prop);
    const uint8_t bit = static_cast<uint8_t>(1u << (joint - 1));
    if (r.mask & bit) return;
    r.mask |= bit;
    PublishState(*s, key, r.mask);
}

void StampRow(void* prop, uint8_t& physFlags, uint8_t& pryMask) {
    if (g_rec.empty()) return;  // a snapshot's rows are many and a pried prop is rare: no key read for each
    // The pried drill's join red arm: the row carries no pry. Resolved once.
    static const bool s_rowOff = coop::config::ResolveFlag(coop::config_registry::rows::pry_row_off);
    if (s_rowOff) return;
    auto* s = LaneSession();
    if (!s || s->role() != coop::net::Role::Host) return;
    // Found by the prop itself, no key read: a record made for another world's prop, or for a prop that is static again,
    // matches none, so its row carries nothing.
    for (const auto& kv : g_rec) {
        const Rec& r = kv.second;
        if (!r.prop.Alive() || r.prop.Raw() != prop || ue_wrap::prop::IsStatic(prop)) continue;
        physFlags |= coop::net::propspawn_flags::kPried;
        pryMask = r.mask;
        return;
    }
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
    // The row is the whole of the pry this copy missed: a copy still static or frozen (its blob was cut before the pry)
    // is freed as the state would free it, silently, and the record says what was done, so a later PryState does not skip
    // the cue the row left out.
    if (!r.freed) {
        if (ue_wrap::prop::IsStatic(actor) || ue_wrap::prop::IsFrozen(actor))
            r.freed = ue_wrap::prop::CallBaseSetPropProps(actor, false, false, false);
        else
            r.freed = true;
    }
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
    struct Depth {
        Depth() { ++g_grantDepth; }
        ~Depth() { --g_grantDepth; }
    } depth;
    return ue_wrap::prop::CallCrowbarOpen(prop);
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
    const auto it = g_rec.try_emplace(key).first;
    Rec& rec = it->second;
    if (!rec.freed || !rec.cued) {
        // The deterministic part of the pry, which every peer runs alike: the flags, and what the prop's own body does
        // at that moment outside the shuffle and the joint loop -- a plank's sound, a hanging prop's break effect. The
        // plank's kick and the loop's random spin are the host's body alone; this copy takes the fall from the host. No
        // peer calls SetConstrainedComponents or the shuffle, so a joint once broken is never revived. A step a row already
        // did (the flags) is not done twice; one it left out (the cue) is done now.
        if (!rec.freed) rec.freed = ue_wrap::prop::CallBaseSetPropProps(prop, false, false, false);
        if (!rec.cued) {
            rec.cued = true;
            if (kind == PP::Kind::Plank) coop::prop_sound::PlayDamagedSound(prop);
            else PP::CallBreakEffect(prop);
        }
        ++g_stats.applied;
        UE_LOGI("prop_pry_sync: PRY applied '%ls' (%s; #%llu)", key.c_str(),
                rec.freed ? "flags cleared" : "setPropProps did not run", static_cast<unsigned long long>(g_stats.applied));
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
    // The host's record is its world's, not a client's: it outlives the last client so a rejoiner's row carries it. A record
    // left by another world is dropped where it is met (ForgetIfStale: a pried prop is never static).
    auto* s = g_session.load(std::memory_order_acquire);
    if (!(s && s->running() && s->role() == coop::net::Role::Host)) g_rec.clear();
    g_grantDepth = 0;
}

}  // namespace coop::prop_pry_sync
