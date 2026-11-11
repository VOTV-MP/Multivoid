// coop/dev/hostile_drill_ignite_client.cpp -- the ignite family's sender: see coop/dev/hostile_drill_ignite.h.
// The client records the rockets and lighters it holds and says so in the lobby chat, takes the tool the arm needs into
// its hand, waits for the host to say it holds the matching state, finds the host's three targets, then sends raw
// PropIgniteIntent payloads through the session's own send, bypassing its ignite lane.

#include "hostile_drill_ignite_detail.h"  // co-located private header (src tree, not include/)

#include "explosion_drill_source.h"  // HeldOf, ResolveClass (co-located private header)

#include "coop/dev/announce_hold.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_hand.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::hostile_drill::ignite::detail {
namespace {

namespace DC = coop::dev::drill_chat;
namespace XD = coop::dev::explosion_drill::detail;
namespace E  = ue_wrap::engine;
namespace EL = coop::element;
namespace R  = ue_wrap::reflection;
using Kind = coop::net::ReliableKind;
using Payload = coop::net::PropIgniteIntentPayload;

constexpr uint32_t kFloodCount = 40;   // the flood arm's messages, sent in one tick
constexpr int      kScanKeep = 4;      // the instances a scan keeps (the drill expects one or two)
constexpr float    kTargetRadiusCm = 8000.f;   // the host's targets stand within this of the sender (the far rocket ~3000)

enum class CStep : uint8_t { Run, Done };
CStep    g_step = CStep::Run;
bool     g_idleSaid = false, g_started = false, g_took = false, g_ready = false;
Clock::time_point g_since{};
const char* g_waiting = nullptr;
// The rockets and lighters this world held when the client's join ended: the drill's targets are the ones outside it.
std::vector<ue_wrap::CachedObjRef> g_start;

// A target the client names on the wire: its key as the real lane writes it, and its eid.
struct Target {
    coop::net::WireKey key{};
    uint32_t           eid = 0;
};
struct Targets {
    Target nearRocket, farRocket, lighter;
};

void Inconclusive(const Spec& sp, const std::string& why) {
    Verdict(sp.arm, "INCONCLUSIVE", why);
    g_step = CStep::Done;
}

// True once the wait named `what` has run past the failure bound; a different wait restarts the clock.
bool Late(const char* what) {
    if (!g_waiting || std::strcmp(g_waiting, what) != 0) {
        g_waiting = what;
        g_since = Clock::now();
    }
    return Clock::now() - g_since > kFailBound;
}

bool OwnJoinOver() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

Payload Make(const Target& t, uint8_t verb) {
    Payload p{};
    p.key = t.key;
    p.elementId = t.eid;
    p.verb = verb;
    return p;
}

// One forged run, counting what it sent; a send that fails stops the run.
struct Out {
    coop::net::Session* s;
    uint32_t            n = 0;
    bool                ok = true;
    void Send(const Payload& p) {
        ok = ok && s->SendReliable(Kind::PropIgniteIntent, &p, sizeof(p));
        ++n;
    }
};

// The arm's messages at the targets. The malformed ones each break one rule of the form; the flood is all one verb at
// the lighter, which no rocket check passes.
void SendArm(Out& o, Arm a, const Targets& t) {
    switch (a) {
    case Arm::Malformed: {
        o.Send(Make(t.nearRocket, 7));                           // a verb the wire does not have
        Payload pad = Make(t.nearRocket, 0);
        pad.pad[0] = 1;                                          // a pad byte set
        o.Send(pad);
        o.Send(Make(Target{}, 0));                               // no key and no eid
        Payload longKey = Make(t.nearRocket, 0);
        longKey.key.len = static_cast<uint8_t>(sizeof(longKey.key.data) + 1);   // a key longer than the wire holds
        o.Send(longKey);
        break;
    }
    case Arm::NoTool:
    case Arm::TorchAttempt:
        o.Send(Make(t.nearRocket, 0));
        break;
    case Arm::TorchIgnite:
        o.Send(Make(t.nearRocket, 1));
        break;
    case Arm::Reach:
        o.Send(Make(t.farRocket, 0));
        break;
    case Arm::NotRocket:
        o.Send(Make(t.lighter, 0));
        break;
    case Arm::Flood:
        for (uint32_t i = 0; i < kFloodCount; ++i) o.Send(Make(t.lighter, 0));
        break;
    default:
        break;
    }
}

// not_ready: the light goes out while this client's own announce is held, then the hold is let go. The edge is the
// one the explosion family uses: the hold set, the connection up, the slot assigned.
void TickNotReady(coop::net::Session* s, const Spec& sp) {
    if (coop::net_pump::HasAnnouncedWorldReady()) {
        Inconclusive(sp, "the hold did not take: the announce was already out before the light could be sent");
        return;
    }
    if (!coop::dev::announce_hold::Held()) return;   // Install sets the hold at the session's start
    const bool late = Late("the connection and the slot");
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    if (!s->connected() || self == 0 || self >= coop::players::kMaxPeers) {
        if (late) {
            coop::dev::announce_hold::Set(false);
            Inconclusive(sp, "the connection or the slot did not resolve before the light could be sent");
        }
        return;
    }
    if (self != kSlot) {
        UE_LOGI("[HOSTILE-DRILL] %s idles: this client is slot %u, the family acts on slot %u", sp.name,
                static_cast<unsigned>(self), static_cast<unsigned>(kSlot));
        coop::dev::announce_hold::Set(false);
        g_step = CStep::Done;
        return;
    }
    Target none{};
    coop::net::WireKeyFromString(L"none", none.key);
    Out o{s};
    o.Send(Make(none, 0));
    coop::dev::announce_hold::Set(false);
    g_step = CStep::Done;
    if (!o.ok) {
        Verdict(sp.arm, "FAIL", "client: the forged light was not sent");
        return;
    }
    UE_LOGI("[HOSTILE-DRILL] %s sent one light with the announce held, and let it go", sp.name);
}

// The instances of one class the drill's scan found outside the start set (the class default and `skip` left out).
struct Scan {
    const std::vector<ue_wrap::CachedObjRef>* start;
    void*                                     skip;
    void*                                     hits[kScanKeep];
    int                                       n;
    ue_wrap::FVector                          from;   // the sender's place: only instances near it are the host's targets
};

float DistTo(void* actor, const ue_wrap::FVector& from);

void ScanClass(const wchar_t* cls, void* skip, Scan& sc) {
    sc.skip = skip;
    sc.n = 0;
    void* c = ue_wrap::object_index::ClassByName(cls);
    if (!c) return;
    ue_wrap::object_index::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
        auto* sn = static_cast<Scan*>(ctx);
        if (obj == sn->skip || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
        for (const ue_wrap::CachedObjRef& old : *sn->start)
            if (old.Is(obj)) return;
        // The host spawns its targets round the sender's puppet. A rocket or lighter born elsewhere (0.10's host
        // world has a lighter far from the base that reaches a client's world with the connect snapshot, after the
        // start set was taken) is not one of the three.
        if (DistTo(obj, sn->from) > kTargetRadiusCm) return;
        if (sn->n < kScanKeep) sn->hits[sn->n] = obj;
        ++sn->n;
    }, &sc);
}

float DistTo(void* actor, const ue_wrap::FVector& from) {
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(actor, at)) return 1e30f;
    const float dx = at.X - from.X, dy = at.Y - from.Y, dz = at.Z - from.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The target as the real lane names it: a key the prop holds, and the eid the registry gave the actor. False while the
// actor has no eid yet (its announce is not in).
bool NameTarget(void* actor, Target& out) {
    const EL::ElementId eid = EL::Registry::Get().EidForActor(actor);
    if (eid == EL::kInvalidId) return false;
    out.eid = static_cast<uint32_t>(eid);
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
    if (!key.empty() && key != L"None") coop::net::WireKeyFromString(key, out.key);
    return true;
}

enum class Found : uint8_t { Wait, Ok, Bad };

// The host's three targets: two rockets outside the start set (the nearer to this player is the near one) and the
// world lighter outside it too, the lighter in this client's own hand left out. Wait while one is missing.
Found FindTargets(const Spec& sp, void* player, Targets& out) {
    ue_wrap::FVector from{};
    if (!E::TryGetActorLocation(player, from)) return Found::Wait;
    Scan rockets{&g_start, nullptr, {}, 0, from};
    ScanClass(kRocketCls, nullptr, rockets);
    void* held = sp.hand == Hand::Lighter ? XD::HeldOf(player, kLighterCls) : nullptr;
    Scan lighters{&g_start, nullptr, {}, 0, from};
    ScanClass(kLighterCls, held, lighters);
    if (rockets.n > 2 || lighters.n > 1) return Found::Bad;
    if (rockets.n < 2 || lighters.n < 1) return Found::Wait;
    void* nearR = rockets.hits[0];
    void* farR = rockets.hits[1];
    if (DistTo(farR, from) < DistTo(nearR, from)) std::swap(nearR, farR);
    if (!NameTarget(nearR, out.nearRocket) || !NameTarget(farR, out.farRocket) ||
        !NameTarget(lighters.hits[0], out.lighter))
        return Found::Wait;
    return Found::Ok;
}

void RecordStart() {
    g_start.clear();
    for (const wchar_t* cls : {kRocketCls, kLighterCls}) {
        void* c = ue_wrap::object_index::ClassByName(cls);
        if (!c) continue;
        ue_wrap::object_index::ForEachInstance(c, [](void*, void* obj, int32_t) {
            if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            ue_wrap::CachedObjRef ref;
            ref.Set(obj);
            g_start.push_back(ref);
        }, nullptr);
    }
}

// The tool the arm needs, in the hand; true once held.
bool Prepare(const Spec& sp, void* player) {
    if (sp.hand == Hand::Empty) return true;
    if (!g_took) {
        if (!XD::ResolveClass(HandClass(sp.hand), HandPath(sp.hand))) {
            Inconclusive(sp, "the tool's class is neither loaded nor loadable");
            return false;
        }
        if (!coop::dev::drill_hand::TakeIntoHandNow(HandItem(sp.hand), HandClass(sp.hand))) {
            Inconclusive(sp, "the hand could not take the tool");
            return false;
        }
        g_took = true;
    }
    if (!XD::HeldOf(player, HandClass(sp.hand))) {
        if (Late("the hand to hold the tool")) Inconclusive(sp, "the hand never held the tool");
        return false;
    }
    return true;
}

// The host's ready line, latched on sight from the arm's first tick, whatever the client is doing: a live chat row
// retires after seconds, so a line is read when it lands. Polled a few times a second.
void Observe(const Spec& sp) {
    if (g_ready || !DC::PollDue()) return;
    g_ready = DC::ChatHasLive(ReadyLine(sp.arm));
}

}  // namespace

void ClientTick(coop::net::Session* s, const Spec& sp) {
    if (g_step == CStep::Done) return;
    if (sp.mode == Mode::NotReady) return TickNotReady(s, sp);
    Observe(sp);
    if (!OwnJoinOver()) return;
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    void* player = coop::players::Registry::Get().Local();
    if (!player || self == 0 || self >= coop::players::kMaxPeers) return;
    if (self != kSlot) {
        if (!g_idleSaid) {
            g_idleSaid = true;
            UE_LOGI("[HOSTILE-DRILL] %s idles: this client is slot %u, the family acts on slot %u", sp.name,
                    static_cast<unsigned>(self), static_cast<unsigned>(kSlot));
        }
        g_step = CStep::Done;
        return;
    }
    if (!g_started) {   // before the tool is taken, so the held lighter is not mistaken for the world's
        RecordStart();
        g_started = true;
        // The host spawns the targets only after this line: a target born before the set is recorded would sit in it.
        DC::Say(ClientReadyLine(sp.arm));
    }
    if (!Prepare(sp, player)) return;
    if (!g_ready) {
        if (Late("the host's ready line")) Inconclusive(sp, "the host never said it was ready");
        return;
    }
    Targets t;
    switch (FindTargets(sp, player, t)) {
    case Found::Wait:
        if (Late("the host's targets")) Inconclusive(sp, "the host's targets never all appeared, each with its eid");
        return;
    case Found::Bad:
        Inconclusive(sp, "more rockets or lighters stand outside the start set than the host spawned");
        return;
    case Found::Ok:
        break;
    }
    Out o{s};
    SendArm(o, sp.arm, t);
    g_step = CStep::Done;
    if (!o.ok) return Verdict(sp.arm, "FAIL", "client: a forged message was not sent");
    UE_LOGI("[HOSTILE-DRILL] %s sent %u messages", sp.name, static_cast<unsigned>(o.n));
}

void ClientReset() {
    g_step = CStep::Run;
    g_idleSaid = g_started = g_took = g_ready = false;
    g_since = {};
    g_waiting = nullptr;
    g_start.clear();
}

}  // namespace coop::dev::hostile_drill::ignite::detail
