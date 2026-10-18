// coop/props/remover_request.cpp -- see coop/props/remover_request.h.

#include "coop/props/remover_request.h"

#include "coop/comms/chat_feed.h"
#include "coop/element/element.h"
#include "coop/element/intent_authority.h"  // kPoseStalenessUU
#include "coop/element/registry.h"
#include "coop/creatures/kerfur_entity.h"
#include "coop/net/intent_queue.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/refusal_tally.h"
#include "coop/net/session.h"
#include "coop/permissions/gameplay_nodes.h"
#include "coop/permissions/permission_host.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/remote_prop.h"
#include "coop/session/net_pump.h"

#include "l10n/l10n.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace coop::remover_request {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace EL = coop::element;
namespace sg = ue_wrap::script_gate;
using coop::net::RemoverReason;
using ue_wrap::FVector;

UE_FACT_FN(kRowInit, "remover_request", L"tool_remover_C", L"init", Declared);
constexpr int kTag = 0x524D5631;  // 'RMV1'

std::atomic<coop::net::Session*> g_session{nullptr};
coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

constexpr size_t kReasons = static_cast<size_t>(RemoverReason::Count);

// ---- client ---------------------------------------------------------------------------------------------------

// A notice is shown at most once per reason per window; the slot after the host's reasons is the client's own
// "cannot place this press" line.
constexpr uint64_t kShowEveryMs = 5000;
constexpr size_t   kNotRemovable = kReasons;
uint64_t g_shownAt[kReasons + 1] = {};
bool     g_shown[kReasons + 1] = {};

void Show(size_t slot, const std::string& line) {
    const uint64_t now = ::GetTickCount64();
    if (g_shown[slot] && now - g_shownAt[slot] < kShowEveryMs) return;
    g_shown[slot] = true;
    g_shownAt[slot] = now;
    coop::chat_feed::Push(line, coop::chat_feed::Keep::Transient);
}

void ShowReason(RemoverReason r) {
    switch (r) {
    case RemoverReason::NoRight:
        Show(static_cast<size_t>(r), std::string(l10n::T("The host does not allow the remover")));
        return;
    case RemoverReason::Unsupported:
        Show(static_cast<size_t>(r), std::string(l10n::T("The remover cannot remove that in multiplayer")));
        return;
    case RemoverReason::Held:
        Show(static_cast<size_t>(r), std::string(l10n::T("That is being held")));
        return;
    case RemoverReason::OutOfSight:
        Show(static_cast<size_t>(r), std::string(l10n::T("You can't see that from there")));
        return;
    case RemoverReason::Failed:
        Show(static_cast<size_t>(r), std::string(l10n::T("Not removable right now")));
        return;
    default:
        return;
    }
}

// The press is cancelled and the tool destroyed (the body's own end would have done it): the toolgun is left as after
// a native miss.
sg::Verdict CancelPress(const sg::Call& c) {
    E::DestroyActor(c.object);
    return sg::Verdict::Cancel;
}

// A press this client cannot place, or whose target the session shares and this client cannot name: nothing is
// removed locally and the player is told.
sg::Verdict Unplaceable(const sg::Call& c, const char* why) {
    UE_LOGW("remover_request: a press is cancelled: %s", why);
    Show(kNotRemovable, std::string(l10n::T("Not removable right now")));
    return CancelPress(c);
}

sg::Verdict OnInitPre(const sg::Call& c) {
    coop::net::Session* s = LoadSession();
    if (!s || !s->running() || s->role() != coop::net::Role::Client || !coop::net_pump::IsInAnnouncedWorld(c.object))
        return sg::Verdict::Run;
    static R::ParamOffset sToolgun{L"toolgun"};
    const int32_t toolgunOff = sToolgun.Of(c.function);
    void* toolgun = nullptr;
    if (toolgunOff >= 0 && c.locals) std::memcpy(&toolgun, c.locals + toolgunOff, sizeof(toolgun));
    ue_wrap::hit_result::Hit hit;
    if (!toolgun || !R::IsLive(toolgun) || !ue_wrap::hit_result::ReadField(toolgun, L"outHit", &hit))
        return Unplaceable(c, "the toolgun's outHit did not read");
    // The game's body does nothing for a miss, and for a hit that names no component: both run natively.
    if (!hit.blocking || !hit.component) return sg::Verdict::Run;
    if (!hit.actor) return Unplaceable(c, "a blocking hit names no live actor");
    const EL::ElementId eid = EL::Registry::Get().EidForActor(hit.actor);
    if (eid == EL::kInvalidId) {
        if (ue_wrap::prop::IsKeyedInteractable(hit.actor))
            return Unplaceable(c, "a keyed prop with no element id");
        return sg::Verdict::Run;   // an actor this session does not share: native, local
    }
    FVector camera{};
    void* player = coop::players::Registry::Get().Local();
    if (!player || !E::ReadMainPlayerCameraLocation(player, camera))
        return Unplaceable(c, "this player's camera did not read");
    coop::net::RemoverRequestPayload p{};
    p.elementId = static_cast<uint32_t>(eid);
    p.origin[0] = camera.X;
    p.origin[1] = camera.Y;
    p.origin[2] = camera.Z;
    p.hit[0] = hit.location.X;
    p.hit[1] = hit.location.Y;
    p.hit[2] = hit.location.Z;
    if (!s->SendReliableToSlot(0, coop::net::ReliableKind::RemoverRequest, &p, sizeof(p)))
        return Unplaceable(c, "the request was not sent");
    UE_LOGI("remover_request: this client's press on element %u sent to the host", p.elementId);
    return CancelPress(c);
}

// ---- host -----------------------------------------------------------------------------------------------------

// A sender's requests run in arrival order from a bounded queue at a bounded rate (kerfur_convert_host's shape). A
// request past the cap, or from an unknown sender, is refused into the tally. Per-slot state: a leaver's queue goes
// with its occupant. Game thread.
constexpr coop::net::IntentBudget kBudget{4.0f, 2.0f};
constexpr size_t kMaxPending = 8;

// A request with the sender's eye as the host held it when the request arrived: queued presses are judged against the
// pose they were made at, not the pose of the tick that serves them.
struct Queued {
    coop::net::RemoverRequestPayload req;
    FVector eye{};
    bool haveEye = false;
};

struct Slot {
    coop::net::IntentQueue<Queued> queue;
    uint32_t answered[kReasons] = {};
    const char* lastWhy = nullptr;   // the branch of the last refusal tallied for this sender
};
coop::roster_ledger::PerSlotState<Slot> g_slots;
coop::roster_ledger::PerSlotState<uint32_t> g_runs;
coop::roster_ledger::PerSlotState<uint32_t> g_refused;
coop::net::RefusalTally g_tally{"remover"};

void Refuse(uint8_t slot, const char* why) {
    if (slot >= 1 && slot < coop::players::kMaxPeers) ++g_refused[slot];
    g_tally.Note(slot, why, coop::net::RefusalTally::Clock::now());
}

inline bool Finite3(const float v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

inline float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// How far the shot's hit point may lie beyond the prop's bounding sphere, and how far short of the hit point a
// blocker may stand before the segment counts as blocked: a prop's surface is not its bounds, and the hit point is
// on a surface.
constexpr float kOnPropSlackCm = 50.f;
constexpr float kBlockedSlackCm = 25.f;

// What the judge decided and, for a reason that is not Removed or Gone, the exact branch it took: a static string the
// tally and the log carry, so a refusal says which check stopped it.
struct Verdict {
    RemoverReason reason;
    const char*   why;
};

constexpr Verdict Decided(RemoverReason r, const char* why) { return {r, why}; }

// What stopped a segment, said once per tally window at INFO and not per message: the blocker's class and how far it
// stood from the eye, how far the hit point was, and how far the client's own origin lay from the eye. A diagnostic
// for an honest shot refused for sight, bounded like the tally's lines.
std::wstring g_lastBlockerClass;   // the class that stopped the last blocked segment, for the hostile drill

void NoteBlocked(void* blocker, float blockerCm, float hitCm, float originCm) {
    const std::wstring cls = blocker ? R::ClassNameOf(blocker) : std::wstring(L"(no actor)");
    g_lastBlockerClass = cls;
    static coop::net::RefusalTally::Clock::time_point sLast{};
    const auto now = coop::net::RefusalTally::Clock::now();
    if (sLast != coop::net::RefusalTally::Clock::time_point{} && now - sLast < coop::net::RefusalTally::kWindow) return;
    sLast = now;
    UE_LOGI("remover_request: a segment was blocked by %ls at %.0f uu from the eye; the hit point is %.0f uu away and "
            "the client's origin %.0f uu from the eye", cls.c_str(), blockerCm, hitCm, originCm);
}

// Could the sender have struck `actor` at the request's hit point, from the eye the host holds for it? The strength
// against a forged origin is the pose-staleness ball around the eye, not the segment: a sender that names an origin
// on the hit point leaves a segment of no length. Source takes a shot's origin from the server's own eye and never
// from the client (reference/source-sdk-2013/src/game/shared/baseplayer_shared.cpp:817 and
// reference/source-sdk-2013/src/game/shared/basecombatweapon_shared.cpp:2313); MTA bounds the claimed origin to its
// copy of the player and traces nothing
// (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CBulletsyncPacket.cpp:167-191).
// This lane does both, because the host has the world.
Verdict JudgeSight(void* puppet, void* actor, const Queued& q) {
    const coop::net::RemoverRequestPayload& p = q.req;
    const FVector origin{p.origin[0], p.origin[1], p.origin[2]};
    const FVector hitAt{p.hit[0], p.hit[1], p.hit[2]};
    if (!q.haveEye) return Decided(RemoverReason::NotReady, "no eye (the sender's head position did not read)");
    // The client's origin is only a consistency check against the eye the host captured when the request arrived.
    if (!(Dist(origin, q.eye) <= EL::kPoseStalenessUU))   // written so that a NaN fails
        return Decided(RemoverReason::OutOfSight, "origin far from the eye");
    // The hit point on the prop: inside the sphere of the bounds of ALL its components, plus a margin. A zero extent is
    // no bounds, never a point-sized prop at the world origin.
    FVector centre{}, extent{};
    float radius = 0.f;
    if (E::GetActorBounds(actor, /*onlyColliding=*/false, centre, extent) &&
        !(extent.X == 0.f && extent.Y == 0.f && extent.Z == 0.f)) {
        radius = std::sqrt(extent.X * extent.X + extent.Y * extent.Y + extent.Z * extent.Z);
    } else if (!E::TryGetActorLocation(actor, centre)) {
        return Decided(RemoverReason::Failed, "the prop's bounds and location did not read");
    }
    if (!(Dist(hitAt, centre) <= radius + kOnPropSlackCm))
        return Decided(RemoverReason::OutOfSight, "hit point off the prop");
    // The segment from the eye to the hit point meets nothing the shot itself would stop at (the toolgun traces these
    // four object types) before it, but the players' bodies and the prop.
    const uint8_t types[4] = {0, 1, 2, 3};   // the shot's own set: WorldStatic, WorldDynamic, Pawn, PhysicsBody
    // Players are people, not cover: every player body (this process's own and each puppet) is ignored, with the prop. A
    // body that holds the sender's eye -- two players standing at one spot -- would otherwise block at no distance. The
    // host is deliberately more lenient here than the native shot, which traces the same four types ignoring only its
    // shooter and so stops at another player's capsule. A modified client gains only the sight through a standing player,
    // which is mobile cover rather than geometry; the eye ball and the prop's sphere still bound the claim.
    void* ignore[coop::players::kMaxPeers + 1];
    int nIgnore = coop::players::Registry::Get().LiveBodies(ignore, coop::players::kMaxPeers);
    ignore[nIgnore++] = actor;
    ue_wrap::trace::Hit blocker;
    if (!ue_wrap::trace::LineHitForObjects(puppet, q.eye, hitAt, types, 4, ignore, nIgnore, &blocker))
        return Decided(RemoverReason::Failed, "the sight trace did not run");
    if (blocker.blocked && !(Dist(q.eye, blocker.point) >= Dist(q.eye, hitAt) - kBlockedSlackCm)) {
        NoteBlocked(blocker.actor, Dist(q.eye, blocker.point), Dist(q.eye, hitAt), Dist(origin, q.eye));
        return Decided(RemoverReason::OutOfSight, "segment blocked before the hit point");
    }
    return Decided(RemoverReason::Removed, "");
}

// The host's own hand holds `actor`.
bool HostHolds(void* actor) {
    void* local = coop::players::Registry::Get().Local();
    E::MainPlayerGrabState g{};
    return local && E::ReadMainPlayerGrabState(local, g) && (g.grabbingActor == actor || g.holdingActor == actor);
}

// Judges one request in the lane's order; on Removed `*out` is the prop to remove.
Verdict Judge(coop::net::Session& s, uint8_t slot, const Queued& q, void** out) {
    const coop::net::RemoverRequestPayload& p = q.req;
    if (slot < 1 || slot >= coop::players::kMaxPeers) return Decided(RemoverReason::NotReady, "slot out of range");
    if (!s.IsSlotWorldReady(slot))
        return Decided(RemoverReason::NotReady, "the sender's world is not announced ready");
    const std::string guid = s.ProvedGuidForSlotWithToken(slot, coop::roster_ledger::Get(slot).bornGeneration);
    if (guid.empty()) return Decided(RemoverReason::NotReady, "the sender's id is not proved");
    if (!Finite3(p.origin) || !Finite3(p.hit))
        return Decided(RemoverReason::NotReady, "a non-finite origin or hit point");
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(slot);
    void* puppet = rp.valid() && rp.HasPose() ? rp.GetActor() : nullptr;
    if (!puppet) return Decided(RemoverReason::NotReady, "no posed puppet for the sender");

    const EL::Element* el = p.elementId < EL::kMaxElements ? EL::Registry::Get().Get(p.elementId) : nullptr;
    void* actor = el ? el->LiveActor() : nullptr;
    if (!actor) return Decided(RemoverReason::Gone, "no live element");
    if (!coop::kerfur_entity::KerfurPropBound())
        return Decided(RemoverReason::NotReady, "the kerfur prop class is not bound yet");
    if (el->GetType() != EL::ElementType::Prop)
        return Decided(RemoverReason::Unsupported, "the element is not a prop");
    if (ue_wrap::prop::IsTrashActor(actor)) return Decided(RemoverReason::Unsupported, "a trash pile or clump");
    if (ue_wrap::prop::IsTrashBitsPile(actor)) return Decided(RemoverReason::Unsupported, "a trash bits pile");
    if (coop::kerfur_entity::IsKerfurPropClass(R::ClassOf(actor)))
        return Decided(RemoverReason::Unsupported, "a kerfur prop");

    namespace GN = coop::permissions::gameplay_nodes;
    if (!coop::permissions::host::Allows(guid, GN::kToolgunRemove.node, GN::kToolgunRemove.defaultGranted, false))
        return Decided(RemoverReason::NoRight, "the remover node is withheld from the sender");

    if (coop::remote_prop::IsActorUnderAnyDrive(actor)) return Decided(RemoverReason::Held, "held by a peer's drive");
    if (coop::prop_drive_host::InAHand(actor)) return Decided(RemoverReason::Held, "held in a hand");
    if (HostHolds(actor)) return Decided(RemoverReason::Held, "held by the host's own grab");

    const Verdict sight = JudgeSight(puppet, actor, q);
    if (sight.reason == RemoverReason::Removed) *out = actor;
    return sight;
}

void Answer(coop::net::Session& s, uint8_t slot, uint32_t eid, const Verdict& v) {
    ++g_slots[slot].answered[static_cast<size_t>(v.reason)];
    if (v.reason != RemoverReason::Removed && v.reason != RemoverReason::Gone) {
        g_slots[slot].lastWhy = v.why;
        g_tally.Note(slot, v.why, coop::net::RefusalTally::Clock::now());
        // A request that could not be judged, or a destroy that did not take, is the host failing the sender: said at
        // once with its cause, not left to the window's summed line.
        if (v.reason == RemoverReason::Failed)
            UE_LOGW("remover_request: slot %u's element %u not removed: %s", slot, eid, v.why);
    }
    coop::net::RemoverResultPayload rp{};
    rp.elementId = eid;
    rp.reason = static_cast<uint8_t>(v.reason);
    s.SendReliableToSlot(slot, coop::net::ReliableKind::RemoverResult, &rp, sizeof(rp));
}

// A request judged: the host does what the BP does -- K2_DestroyActor on its own actor -- and the prop seam broadcasts
// the destroy to every world-ready peer, the sender included. Nothing of the receive path's listeners runs: a
// remover's removal is not a spend.
void Serve(coop::net::Session& s, uint8_t slot, const Queued& q) {
    ++g_runs[slot];
    void* actor = nullptr;
    Verdict v = Judge(s, slot, q, &actor);
    if (v.reason == RemoverReason::Removed) {
        ue_wrap::CachedObjRef incarnation;
        incarnation.Set(actor);
        E::DestroyActor(actor);
        if (incarnation.Is(actor)) v = Decided(RemoverReason::Failed, "the destroy left the actor live");
    }
    Answer(s, slot, q.req.elementId, v);
}

void ServeQueues(coop::net::Session& s) {
    for (uint8_t slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        g_slots[slot].queue.Serve(kBudget, coop::net::NowMs, [&](const Queued& q) {
            Serve(s, slot, q);
            return coop::net::Served::Done;
        });
    }
}

// ---- the watch's settle --------------------------------------------------------------------------------------

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg = Reg::Pending;
bool g_settled = false;

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_reg != Reg::Pending) return;
    // A client cancels the press by this watch; the gate refuses a watch it cannot keep for the process.
    g_reg = sg::WatchName(kRowInit, kTag, &OnInitPre, nullptr) ? Reg::Registered : Reg::Refused;
}

void Tick() {
    if (!g_settled && g_reg != Reg::Pending) {
        if (g_reg == Reg::Registered) sg::ResolvePendingNames();
        if (g_reg == Reg::Registered && sg::NameWatchLive(kRowInit, kTag)) {
            g_settled = true;
            UE_LOGI("remover_request: the watch on %ls::%ls is live", kRowInit.Class(), kRowInit.Name());
        } else if (g_reg == Reg::Refused || sg::NameWatchSettled(kRowInit, kTag)) {
            g_settled = true;
            UE_LOGE("remover_request: the watch on %ls::%ls is dead -- a client's remover runs natively and its "
                    "removal reaches the host as a destroy", kRowInit.Class(), kRowInit.Name());
        }
    }
    coop::net::Session* s = LoadSession();
    if (s && s->running() && s->role() == coop::net::Role::Host) {
        ServeQueues(*s);
        g_tally.Flush(coop::net::RefusalTally::Clock::now());
    }
}

void OnRequest(const coop::net::RemoverRequestPayload& payload, uint8_t senderSlot) {
    coop::net::Session* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGW("remover_request: RemoverRequest received on a non-host -- dropping");
        return;
    }
    // Slot 0 is the host, whose own presses never arrive over the wire.
    if (senderSlot == 0 || senderSlot >= coop::players::kMaxPeers) {
        Refuse(senderSlot, "unknown sender");
        return;
    }
    // A full queue refuses before anything is read: a flood must not pay the eye read per message.
    if (g_slots[senderSlot].queue.size() >= kMaxPending) {
        Refuse(senderSlot, "queue full");
        return;
    }
    Queued q;
    q.req = payload;
    // Only a puppet that has had its first pose has an eye: before it, it stands at its spawn point.
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(senderSlot);
    q.haveEye = rp.valid() && rp.HasPose() && rp.TryGetEyePosition(q.eye);
    g_slots[senderSlot].queue.Push(std::move(q), kMaxPending);   // room was checked above
}

void NoteRefused(uint8_t senderSlot, const char* why) { Refuse(senderSlot, why); }

void OnResult(const coop::net::RemoverResultPayload& payload) {
    if (payload.reason >= kReasons) return;
    ShowReason(static_cast<RemoverReason>(payload.reason));
}

void OnDisconnect() {
    g_tally.Clear();
    for (size_t i = 0; i <= kReasons; ++i) {
        g_shown[i] = false;
        g_shownAt[i] = 0;
    }
}

uint32_t RunsFor(int slot) { return slot >= 1 && slot < coop::players::kMaxPeers ? g_runs[slot] : 0; }
uint32_t RefusedFor(int slot) { return slot >= 1 && slot < coop::players::kMaxPeers ? g_refused[slot] : 0; }
const wchar_t* LastBlockerClass() { return g_lastBlockerClass.c_str(); }

const char* LastWhyFor(int slot) {
    return slot >= 1 && slot < coop::players::kMaxPeers ? g_slots[slot].lastWhy : nullptr;
}
uint32_t AnsweredFor(int slot, RemoverReason reason) {
    const size_t i = static_cast<size_t>(reason);
    return slot >= 1 && slot < coop::players::kMaxPeers && i < kReasons ? g_slots[slot].answered[i] : 0;
}

}  // namespace coop::remover_request
