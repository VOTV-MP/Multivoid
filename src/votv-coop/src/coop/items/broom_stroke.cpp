// coop/items/broom_stroke.cpp -- see coop/items/broom_stroke.h.

#include "coop/items/broom_stroke.h"

#include "coop/element/intent_authority.h"   // may this sender aim there
#include "coop/items/broom_push.h"           // the dispense window a `broomed` body opens
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"           // the sender's broom, as this peer mirrors it
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/active_drive.h"         // NowMs
#include "coop/props/trash_pile_sync.h"      // NoteAuthoredDeath

#include "ue_wrap/actors/broom.h"
#include "ue_wrap/actors/prop.h"             // IsTrashBitsPile / GetInteractableKeyString
#include "ue_wrap/core/game_thread.h"      // IsGameThread
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"           // arm, and the holder's heading and velocity

#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>

namespace coop::broom_stroke {
namespace {

namespace P  = ue_wrap::profile;
namespace UP = ue_wrap::prop;
namespace E  = ue_wrap::engine;
namespace sg = ue_wrap::script_gate;
namespace UH = ue_wrap::ufunction_hook;

// The session the role tests and the sender read; the verdicts carry none of their own.
std::atomic<coop::net::Session*> g_session{nullptr};

// This lane's watch tags, echoed back by the gate.
constexpr int kTagStroke  = 1;
constexpr int kTagArm     = 2;
constexpr int kTagBroomed = 3;

// A stroke's reach: the arm's default length and the sphere the stroke sweeps at the hit point. The
// arm is the held item's own row at runtime, so this is nominal; the authorizer's pose pad covers
// the difference, as it does for every intent.
constexpr float kBroomReachUU = 250.f;

// The push multiplies the heading by the broom's force, so a heading longer than a unit vector
// would push harder than any stroke can. It need not be level: a seated player's actor takes its
// seat's pitch and roll, and a broom swings seated. No pawn's velocity passes its physics volume's
// terminal velocity, UE's default 4000 cm/s.
constexpr float kHeadingUnitTolerance = 0.01f;
constexpr float kMaxHolderVelocityCmS = 4000.f;

// A client's strokes wait their turn in a queue per slot, and a slot runs at most one a tick, no
// faster than a token bucket refills: three in hand and three a second after. A held button
// notifies once a second, and each stroke costs the host a trace, three sphere overlaps and the
// spawns. A stall on either end delivers a backlog in one reliable drain; run there, it stalls a
// frame, and refused past a burst, it drops strokes the client swung honestly. So a backlog waits,
// ten strokes of it at most, and only a stroke past a full queue is refused, said at most every ten
// seconds per slot.
constexpr float    kStrokeBurst       = 3.f;
constexpr float    kStrokesPerSecond  = 3.f;
constexpr size_t   kMaxPendingStrokes = 10;
constexpr uint64_t kRefusalSayMs      = 10000;
struct StrokeBucket { float tokens = kStrokeBurst; uint64_t lastMs = 0; uint64_t nextSayMs = 0; };
StrokeBucket g_rate[coop::net::kMaxPeers];
std::deque<coop::net::BroomStrokePayload> g_pending[coop::net::kMaxPeers];

// Each watch registers once: a refusal is permanent (a full table, or a gate that did not
// install), so it is said once and never retried. The stroke and `arm` watches are the ones a
// stroke needs on either role; `broomed` only matters on the host, and its absence costs nothing
// else.
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_regStroke = Reg::Pending, g_regArm = Reg::Pending, g_regBroomed = Reg::Pending;
bool g_live = false;              // the stroke and arm watches have resolved their names
bool g_liveAbsent = false;        // registered, and the gate never made them live
int  g_nameTries = 0, g_liveTries = 0;
bool g_namesAbsent = false;
constexpr int kMaxTries = 600;    // ten seconds of pump ticks: a name the engine can build resolves at once

// The native seams on a holder's heading and velocity reads, installed disarmed on the first stroke
// a host runs for a client and armed only for the length of each such run.
bool  g_readsInstalled = false;
bool  g_readsRefused   = false;
void* g_fwdFn = nullptr;
void* g_velFn = nullptr;

// What a host answers while it runs a client's stroke: open only for that call, and matched to the
// reads the stroke makes of its holder -- the holder's, from the broom's own graph.
struct Substitution {
    bool             open = false;
    void*            broom = nullptr;
    void*            holder = nullptr;
    ue_wrap::FVector start{}, end{}, heading{}, velocity{};
    int              aims = 0, headings = 0, velocities = 0, unwritable = 0;
};
Substitution g_sub;

// What the lane did this session. Game-thread serial, so plain ints.
int g_refused = 0, g_sent = 0, g_unaimed = 0, g_received = 0, g_run = 0, g_denied = 0,
    g_misaimed = 0, g_pushesAnswered = 0;

bool IsClientSession(coop::net::Session* s) {
    return s && s->connected() && s->role() != coop::net::Role::Host;
}

bool IsHostSession(coop::net::Session* s) {
    return s && s->connected() && s->role() == coop::net::Role::Host;
}

// A stroke's entry, on every peer. The broom's layout is read here, at the entry of the first
// stroke, before its loops -- the host's sweep seam reads the loop's pile through it. On a client
// the stroke is refused and what it read of its holder is sent; the notify's other names reach the
// same body and do nothing, so refusing them changes nothing.
sg::Verdict OnStrokeNotify(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !ue_wrap::broom::IsBroom(call.object)) return sg::Verdict::Run;
    const bool layout = ue_wrap::broom::ResolveLayout(call.object);
    if (!IsClientSession(s)) return sg::Verdict::Run;
    // With no layout the stroke cannot be told from the other names, and running it would author
    // the host's world; the resolve has said why once.
    if (!layout || !ue_wrap::broom::IsStrokeNotify(call.locals)) return sg::Verdict::Cancel;
    ++g_refused;
    void* holder = ue_wrap::broom::ReadHolder(call.object);
    ue_wrap::FVector start{}, end{};
    if (!holder || !E::ReadMainPlayerArm(holder, start, end)) {
        // The stroke would have aimed from nowhere and hit nothing, so nothing is lost by not
        // sending it.
        if (++g_unaimed <= 3)
            UE_LOGW("[BROOM-STROKE] CLIENT stroke on %p refused with no aim to send (holder=%p)",
                    call.object, holder);
        return sg::Verdict::Cancel;
    }
    const ue_wrap::FVector heading = E::GetActorForwardVector(holder);
    const ue_wrap::FVector velocity = E::GetActorVelocity(holder);
    coop::net::BroomStrokePayload p{};
    p.startX = start.X;     p.startY = start.Y;     p.startZ = start.Z;
    p.endX = end.X;         p.endY = end.Y;         p.endZ = end.Z;
    p.fwdX = heading.X;     p.fwdY = heading.Y;     p.fwdZ = heading.Z;
    p.velX = velocity.X;    p.velY = velocity.Y;    p.velZ = velocity.Z;
    if (s->SendReliable(coop::net::ReliableKind::BroomStroke, &p, sizeof(p))) ++g_sent;
    // The first three only: a held broom swings as often as its montage notifies.
    if (g_refused <= 3)
        UE_LOGI("[BROOM-STROKE] CLIENT stroke #%d refused and sent -- (%.0f,%.0f,%.0f) -> (%.0f,%.0f,%.0f), "
                "heading (%.3f,%.3f,%.3f), velocity (%.0f,%.0f,%.0f)", g_refused, start.X, start.Y,
                start.Z, end.X, end.Y, end.Z, heading.X, heading.Y, heading.Z, velocity.X, velocity.Y,
                velocity.Z);
    return sg::Verdict::Cancel;
}

// The host's answer to the stroke's `arm`: the client's segment instead of the host's camera.
// Anything else that calls `arm` -- the host's own look-at trace every frame -- runs as written.
sg::Verdict OnArm(const sg::Call& call) {
    if (!g_sub.open || call.object != g_sub.holder || call.callerObject != g_sub.broom)
        return sg::Verdict::Run;
    int32_t startOff = -1, endOff = -1;
    std::uint8_t* start = nullptr;
    std::uint8_t* end = nullptr;
    if (E::MainPlayerArmOutOffsets(call.function, startOff, endOff)) {
        start = sg::OutParamPtr(call, startOff);
        end = sg::OutParamPtr(call, endOff);
    }
    if (!start || !end) {
        // Never Run here: the body would aim the client's stroke with the host's camera.
        ++g_sub.unwritable;
        return sg::Verdict::Cancel;
    }
    std::memcpy(start, &g_sub.start, sizeof(g_sub.start));
    std::memcpy(end, &g_sub.end, sizeof(g_sub.end));
    ++g_sub.aims;
    return sg::Verdict::Cancel;
}

// The host's answers to the push's reads of its holder: the client's heading and velocity, written
// into the result the broom's graph reads when the call returns.
void OnHolderForward(void* context, void* source, void* /*result*/) {
    // A worker thread's animation update reaches these natives too, and is never the stroke.
    if (!ue_wrap::game_thread::IsGameThread()) return;
    if (!g_sub.open || context != g_sub.holder || source != g_sub.broom) return;
    if (void* r = UH::CurrentResult()) {
        std::memcpy(r, &g_sub.heading, sizeof(g_sub.heading));
        ++g_sub.headings;
    }
}

void OnHolderVelocity(void* context, void* source, void* /*result*/) {
    if (!ue_wrap::game_thread::IsGameThread()) return;
    if (!g_sub.open || context != g_sub.holder || source != g_sub.broom) return;
    if (void* r = UH::CurrentResult()) {
        std::memcpy(r, &g_sub.velocity, sizeof(g_sub.velocity));
        ++g_sub.velocities;
    }
}

// A dispenser pile the host strikes may be the pile's last stroke, and when the stroke is a
// client's the pile dies beside that client, wherever the host's camera is. The depletion watch
// judges a death it cannot see by that camera, so the host names the pile first. The trash the body
// then knocks out of the pile is caught while it runs.
sg::Verdict OnBroomed(const sg::Call& call) {
    if (!IsHostSession(g_session.load(std::memory_order_acquire)) || !UP::IsTrashBitsPile(call.object))
        return sg::Verdict::Run;
    const std::wstring key = UP::GetInteractableKeyString(call.object);   // a raw field read
    if (!key.empty() && key != L"None") coop::trash_pile_sync::NoteAuthoredDeath(key);
    coop::broom_push::OnDispenseBegin(call.object);
    return sg::Verdict::Run;
}

void OnBroomedDone(const sg::Call& /*call*/) {
    coop::broom_push::OnDispenseEnd();
}

void RegisterOnce(Reg& reg, const wchar_t* name, int tag, sg::PreFn pre, sg::PostFn post = nullptr) {
    if (reg != Reg::Pending) return;
    if (sg::WatchName(name, tag, pre, post)) { reg = Reg::Registered; return; }
    reg = Reg::Refused;
    UE_LOGE("[BROOM-STROKE] the script-body gate refused the watch on '%ls' -- for the rest of this "
            "process a client's broom stroke stays its own, and a host runs none for a client", name);
}

void InstallReadSeams() {
    g_fwdFn = E::ActorForwardVectorFunction();
    g_velFn = E::ActorVelocityFunction();
    if (!g_fwdFn || !g_velFn) return;   // the Actor class resolves at boot; asked again next stroke
    const bool a = UH::InstallPostHook(g_fwdFn, &OnHolderForward, /*armed=*/false);
    const bool b = UH::InstallPostHook(g_velFn, &OnHolderVelocity, /*armed=*/false);
    if (!a || !b) {
        g_readsRefused = true;
        UE_LOGE("[BROOM-STROKE] the heading/velocity seams did not install (forward=%d velocity=%d, "
                "native hook table full) -- the host runs no client's stroke for the rest of this process",
                a ? 1 : 0, b ? 1 : 0);
        return;
    }
    g_readsInstalled = true;
}

// A token for `slot`'s next stroke, when its bucket holds one.
bool TakeToken(uint8_t slot) {
    StrokeBucket& b = g_rate[slot];
    const uint64_t now = coop::active_drive::NowMs();
    if (b.lastMs != 0) {
        const float refill = static_cast<float>(now - b.lastMs) * kStrokesPerSecond / 1000.f;
        b.tokens = b.tokens + refill < kStrokeBurst ? b.tokens + refill : kStrokeBurst;
    }
    b.lastMs = now;
    if (b.tokens < 1.f) return false;
    b.tokens -= 1.f;
    return true;
}

float Length(const ue_wrap::FVector& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }

void Deny(uint8_t slot, const char* why) {
    if (++g_denied <= 3) UE_LOGW("[BROOM-STROKE] DENIED slot=%u -- %s", slot, why);
}

// Run `p`, a stroke the client in `senderSlot` sent, on the host's mirror of that client's broom.
void RunStroke(coop::net::Session& session, const coop::net::BroomStrokePayload& p, uint8_t senderSlot) {
    const ue_wrap::FVector start{p.startX, p.startY, p.startZ}, end{p.endX, p.endY, p.endZ};
    const ue_wrap::FVector heading{p.fwdX, p.fwdY, p.fwdZ}, velocity{p.velX, p.velY, p.velZ};
    void* broom = coop::hand_item::MirrorActorForSlot(senderSlot);
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(senderSlot);
    void* holder = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    // MTA answers a client's refused request to act (VEHICLE_ATTEMPT_FAILED and its siblings,
    // reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3042), because a denied request
    // there leaves the client holding a wrong belief. This lane deliberately stays silent: the client
    // cancelled its own stroke before sending, so a denial leaves nothing diverged, only a swing
    // that did nothing.
    // The heading and velocity seams go in on the first stroke a host runs for a client, so a peer
    // that never hosts one never patches the two natives.
    if (!g_readsInstalled && !g_readsRefused) InstallReadSeams();
    const char* why = nullptr;
    if (!g_live || !sg::IsEnabled()) why = "the stroke's watches are not live here";
    else if (!g_readsInstalled) why = "the holder's heading and velocity cannot be answered here";
    else if (!broom || !ue_wrap::broom::IsBroom(broom)) why = "the sender holds no broom here";
    else if (!holder) why = "the sender has no body here";
    else if (!ue_wrap::broom::ResolveLayout(broom)) why = "the broom's layout is not in this build";
    if (why) { Deny(senderSlot, why); return; }
    const auto tok = coop::element::IntentTarget::ForClientIntent(session, senderSlot, kBroomReachUU);
    const coop::element::IntentSubject sub = tok.AuthorizeSegment(start, end);
    if (!sub) {
        if (++g_denied <= 3)
            UE_LOGW("[BROOM-STROKE] DENIED slot=%u -- REASON=%s (dist=%.0f allowed=%.0f)", senderSlot,
                    coop::element::OutcomeName(sub.outcome), sub.distUU, sub.reachUU);
        return;
    }
    // The holder the stroke reads is the broom's own field; the mirror has none of its own, and the
    // stroke is over when the call returns, so the field goes back to what it held and the read
    // seams are disarmed again.
    void* prevHolder = ue_wrap::broom::ReadHolder(broom);
    ue_wrap::broom::WriteHolder(broom, holder);
    g_sub = Substitution{};
    g_sub.open = true;
    g_sub.broom = broom;
    g_sub.holder = holder;
    g_sub.start = start;
    g_sub.end = end;
    g_sub.heading = heading;
    g_sub.velocity = velocity;
    UH::SetArmed(g_fwdFn, &OnHolderForward, true);
    UH::SetArmed(g_velFn, &OnHolderVelocity, true);
    const bool dispatched = ue_wrap::broom::FireStroke(broom);
    UH::SetArmed(g_fwdFn, &OnHolderForward, false);
    UH::SetArmed(g_velFn, &OnHolderVelocity, false);
    const Substitution done = g_sub;
    g_sub = Substitution{};
    ue_wrap::broom::WriteHolder(broom, prevHolder);
    ++g_run;
    // Every body the push moves reads the heading and the velocity once each.
    if (!dispatched || done.aims != 1 || done.unwritable > 0 || done.headings != done.velocities) {
        if (++g_misaimed <= 3)
            UE_LOGW("[BROOM-STROKE] slot=%u stroke did not run as sent (dispatched=%d aims=%d "
                    "unwritable=%d headings=%d velocities=%d)", senderSlot, dispatched ? 1 : 0,
                    done.aims, done.unwritable, done.headings, done.velocities);
        return;
    }
    g_pushesAnswered += done.headings;
    if (g_run <= 3)
        UE_LOGI("[BROOM-STROKE] EXEC slot=%u on broom %p (dist=%.0f allowed=%.0f) -- aimed as sent, %d "
                "push(es) given the client's heading and velocity", senderSlot, broom, sub.distUU,
                sub.reachUU, done.headings);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);  // re-cache every call (reconnect)
    if (g_namesAbsent || g_liveAbsent || (g_live && g_regBroomed != Reg::Pending)) return;
    if (!ue_wrap::broom::ResolveNames()) {
        if (++g_nameTries >= kMaxTries) {
            g_namesAbsent = true;
            UE_LOGE("[BROOM-STROKE] the broom's names did not resolve after %d tries -- for the rest of "
                    "this process no stroke is refused or run for a remote player", g_nameTries);
        }
        return;
    }
    RegisterOnce(g_regStroke, P::name::BroomStrokeNotifyFn, kTagStroke, &OnStrokeNotify);
    RegisterOnce(g_regArm, P::name::MainPlayerArmFn, kTagArm, &OnArm);
    RegisterOnce(g_regBroomed, P::name::PileBroomedFn, kTagBroomed, &OnBroomed, &OnBroomedDone);
    if (g_live || g_regStroke != Reg::Registered || g_regArm != Reg::Registered) return;
    // A name watch is inert until the gate turns its literal into a name, and the resolve is shared,
    // so this lane drives it rather than waiting on another consumer's tick.
    sg::ResolvePendingNames();
    if (sg::NameWatchLive(P::name::BroomStrokeNotifyFn, kTagStroke) &&
        sg::NameWatchLive(P::name::MainPlayerArmFn, kTagArm)) {
        g_live = true;
        UE_LOGI("[BROOM-STROKE] watching the stroke, arm and '%ls' by name -- on a client every "
                "stroke is the host's", P::name::PileBroomedFn);
    } else if (++g_liveTries >= kMaxTries) {
        g_liveAbsent = true;
        UE_LOGE("[BROOM-STROKE] the stroke and arm watches registered and never went live after %d "
                "tries -- for the rest of this process no stroke is refused or run for a remote player",
                g_liveTries);
    }
}

void OnBroomStroke(coop::net::Session& session, const coop::net::BroomStrokePayload& p,
                   uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host || senderSlot >= coop::net::kMaxPeers) return;
    ++g_received;
    const ue_wrap::FVector heading{p.fwdX, p.fwdY, p.fwdZ}, velocity{p.velX, p.velY, p.velZ};
    const char* why = nullptr;
    if (std::fabs(Length(heading) - 1.f) > kHeadingUnitTolerance) why = "a heading that is not a unit vector";
    else if (Length(velocity) > kMaxHolderVelocityCmS) why = "a velocity past terminal";
    if (why) { Deny(senderSlot, why); return; }
    std::deque<coop::net::BroomStrokePayload>& q = g_pending[senderSlot];
    if (q.size() < kMaxPendingStrokes) {
        q.push_back(p);
        return;
    }
    StrokeBucket& b = g_rate[senderSlot];
    const uint64_t now = coop::active_drive::NowMs();
    ++g_denied;
    if (now >= b.nextSayMs) {
        b.nextSayMs = now + kRefusalSayMs;
        UE_LOGW("[BROOM-STROKE] slot %u is sending strokes faster than the host runs them, %.0f a second "
                "-- refusing strokes past a queue of %zu", static_cast<unsigned>(senderSlot),
                kStrokesPerSecond, kMaxPendingStrokes);
    }
}

void Tick(coop::net::Session& session) {
    if (session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 0; slot < coop::net::kMaxPeers; ++slot) {
        std::deque<coop::net::BroomStrokePayload>& q = g_pending[slot];
        if (q.empty() || !TakeToken(slot)) continue;
        const coop::net::BroomStrokePayload p = q.front();
        q.pop_front();
        RunStroke(session, p, slot);
    }
}

void OnPeerLeft(uint8_t slot) {
    if (slot >= coop::net::kMaxPeers) return;
    g_rate[slot] = StrokeBucket{};   // the next occupant's rate and queue are its own
    g_pending[slot].clear();
}

void OnSessionStart() {
    g_refused = g_sent = g_unaimed = g_received = g_run = g_denied = g_misaimed = g_pushesAnswered = 0;
    for (StrokeBucket& b : g_rate) b = StrokeBucket{};
    for (auto& q : g_pending) q.clear();
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    for (auto& q : g_pending) q.clear();
    UE_LOGI("[BROOM-STROKE] session tally -- client: %d refused, %d sent, %d with no aim; host: %d "
            "received, %d run, %d denied, %d not run as sent, %d push(es) answered", g_refused, g_sent,
            g_unaimed, g_received, g_run, g_denied, g_misaimed, g_pushesAnswered);
}

}  // namespace coop::broom_stroke
