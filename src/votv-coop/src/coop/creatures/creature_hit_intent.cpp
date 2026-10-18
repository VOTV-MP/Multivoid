// coop/creatures/creature_hit_intent.cpp -- see coop/creatures/creature_hit_intent.h.

#include "coop/creatures/creature_hit_intent.h"

#include "coop/creatures/npc_mirror_brain.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/intent_queue.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hit_judge.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"

#include "ue_wrap/actors/int_objects.h"
#include "ue_wrap/actors/zombie.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/obj_keyed_cache.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/weapon_catalog.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>

namespace coop::creature_hit_intent {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace EL = coop::element;
namespace P  = ue_wrap::profile;
namespace sg = ue_wrap::script_gate;
using ue_wrap::FVector;

UE_FACT_FN(kRowAddDamage, "creature_hit_intent", nullptr, L"addDamage", AnyClass);
constexpr int kTag = 0x43484954;  // 'CHIT'

std::atomic<coop::net::Session*> g_session{nullptr};
coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---- client ---------------------------------------------------------------------------------------------------

// The functions the zombie family's prediction played, said once each per session.
constexpr int kMaxPredicted = 4;
const wchar_t* g_predicted[kMaxPredicted] = {};
int            g_predictedCount = 0;
bool           g_paramsWarned = false;

void SayPredicted(const wchar_t* function) {
    for (int i = 0; i < g_predictedCount; ++i)
        if (std::wcscmp(g_predicted[i], function) == 0) return;
    if (g_predictedCount < kMaxPredicted) g_predicted[g_predictedCount++] = function;
    UE_LOGI("creature_hit_intent: predicted %ls", function);
}

// The cosmetic half of a hit: the family's feedback at once, so the swing does not wait on the host. Only the zombie
// family has one; the fossilhound's single hit sound sits in a refused resume.
// Source plays a melee swing's impact effect on both sides and its damage on the server alone
// (reference/source-sdk-2013/src/game/shared/hl2mp/weapon_hl2mpbasebasebludgeon.cpp:149-172), and withholds the
// server's copy of the shooter's own sounds from the shooter (reference/source-sdk-2013/src/game/shared/
// basecombatweapon_shared.cpp:1925-1930). This takes the first half: the swinger's feedback is local, health and
// death are the host's. The second half is not taken: another peer sees no feedback for this swing, and a swing the
// host refuses has already shown its feedback.
void Predict(void* mirror, const FVector& hitLoc) {
    void* zombie = ue_wrap::object_index::ClassByName(P::name::NpcClass_Zombie);
    if (!zombie || !R::IsDescendantOfAny(R::ClassOf(mirror), &zombie, 1)) return;
    if (const wchar_t* played = ue_wrap::zombie::PlayHitFeedback(mirror, hitLoc)) SayPredicted(played);
}

// A parameter's storage in the call: the caller's, for an out or reference parameter, else the frame's own slot.
const uint8_t* ParamBytes(const sg::Call& c, int32_t offset) {
    if (offset < 0 || !c.locals) return nullptr;
    const uint8_t* out = sg::OutParamPtr(c, offset);
    return out ? out : c.locals + offset;
}

// Where addDamage's parameters sit in one class's function. The watch is on every class's addDamage, and the
// families' functions alternate, so each is held on its own.
struct ParamLayout {
    int32_t actor = -1, damage = -1, impact = -1, hit = -1, hitSize = -1;
};

const ParamLayout& LayoutOf(void* function) {
    static ue_wrap::ObjKeyedCache<ParamLayout, 4> sLayouts;
    if (const ParamLayout* held = sLayouts.Find(function)) return *held;
    ParamLayout l;
    l.actor = R::FindParamOffset(function, L"actor");
    l.damage = R::FindParamOffset(function, L"damage");
    l.impact = R::FindParamOffset(function, L"impact");
    l.hit = R::FindParamOffset(function, L"hit");
    l.hitSize = R::FindParamSize(function, L"hit");
    return sLayouts.Insert(function, l);
}

// The point the swing struck: the call's `hit` parameter's impact point, else the mirror's own location.
FVector HitPoint(const sg::Call& c, const ParamLayout& layout) {
    const uint8_t* bytes = ParamBytes(c, layout.hit);
    ue_wrap::hit_result::Hit hit;
    if (bytes && layout.hitSize > 0 && ue_wrap::hit_result::Parse(bytes, layout.hitSize, &hit)) {
        const FVector& p = hit.location;
        if (std::isfinite(p.X) && std::isfinite(p.Y) && std::isfinite(p.Z) && (p.X != 0.f || p.Y != 0.f || p.Z != 0.f))
            return p;
    }
    FVector at{};
    E::TryGetActorLocation(c.object, at);
    return at;
}

sg::Verdict OnAddDamagePre(const sg::Call& c) {
    coop::net::Session* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    if (!c.object || !coop::npc_mirror_brain::IsMarkedMirror(c.object) ||
        !coop::npc_mirror_brain::IsParkedClass(R::ClassOf(c.object)))
        return sg::Verdict::Run;

    const ParamLayout& layout = LayoutOf(c.function);
    const uint8_t* actorAt = ParamBytes(c, layout.actor);
    const uint8_t* damageAt = ParamBytes(c, layout.damage);
    const uint8_t* impactAt = ParamBytes(c, layout.impact);
    if (!actorAt || !damageAt || !impactAt) {
        if (!g_paramsWarned) {
            g_paramsWarned = true;
            UE_LOGW("creature_hit_intent: the addDamage parameters did not resolve -- a client's hit on a creature "
                    "is not sent");
        }
        return sg::Verdict::Run;
    }
    // Only this machine's own player authors a hit here, whoever called: the drill's reflected call too.
    void* actor = nullptr;
    std::memcpy(&actor, actorAt, sizeof(actor));
    if (!coop::players::Registry::Get().IsLocal(actor)) return sg::Verdict::Run;

    const EL::ElementId eid = EL::Registry::Get().EidForActor(c.object);
    if (eid == EL::kInvalidId) return sg::Verdict::Run;

    const FVector hitLoc = HitPoint(c, layout);
    float damage = 0.f;
    FVector impact{};
    std::memcpy(&damage, damageAt, sizeof(damage));
    std::memcpy(&impact, impactAt, sizeof(impact));

    coop::net::CreatureHitIntentPayload p{};
    p.creatureEid = static_cast<uint32_t>(eid);
    p.verb = coop::net::kCreatureHitVerbAddDamage;
    p.damage = damage;
    p.hitLoc[0] = hitLoc.X;
    p.hitLoc[1] = hitLoc.Y;
    p.hitLoc[2] = hitLoc.Z;
    p.impact[0] = impact.X;
    p.impact[1] = impact.Y;
    p.impact[2] = impact.Z;

    Predict(c.object, hitLoc);
    if (!s->SendReliableToSlot(0, coop::net::ReliableKind::CreatureHitIntent, &p, sizeof(p)))
        UE_LOGW("creature_hit_intent: the hit on creature %u was not sent", p.creatureEid);
    return sg::Verdict::Cancel;
}

// ---- host -----------------------------------------------------------------------------------------------------

// A sender's intents run in arrival order from a bounded queue at a bounded rate (remover_request's shape). The budget
// bounds a client's swing rate; the held item's own reload is not read.
constexpr coop::net::IntentBudget kBudget{4.0f, 2.0f};
constexpr size_t kMaxPending = 8;

// An intent with the sender's eye as the host held it when the intent arrived: it is judged against the pose the
// swing was made at, not the pose of the tick that serves it.
struct Queued {
    coop::net::CreatureHitIntentPayload req;
    FVector eye{};
    bool haveEye = false;
};

// One refusal reason of one sender: counted, said on its first.
struct Reason {
    const char* why = nullptr;
    uint32_t count = 0;
};
constexpr int kMaxReasons = 24;   // the serve's own six and the judge's eleven, with room

struct Slot {
    coop::net::IntentQueue<Queued> queue;
    Reason reasons[kMaxReasons];
};
coop::roster_ledger::PerSlotState<Slot> g_slots;
uint32_t g_applied = 0;
ue_wrap::ObjKeyedCache<bool, 24> g_appliedSaid;   // the classes whose first applied hit has been said

void Refuse(int slot, const char* why) {
    if (slot < 1 || slot >= coop::players::kMaxPeers) return;
    Slot& sl = g_slots[slot];
    for (Reason& r : sl.reasons) {
        if (r.why && std::strcmp(r.why, why) != 0) continue;
        if (!r.why) {
            r.why = why;
            UE_LOGI("creature_hit_intent: slot=%d refused %s", slot, why);
        }
        ++r.count;
        return;
    }
}

// The creature the intent names: a live host Npc element of a parked family, else null.
void* CreatureOf(uint32_t eid) {
    void* actor = EL::LiveActorOfType(static_cast<EL::ElementId>(eid), EL::ElementType::Npc);
    return actor && coop::npc_mirror_brain::IsParkedClass(R::ClassOf(actor)) ? actor : nullptr;
}

void Serve(coop::net::Session& s, int slot, const Queued& q) {
    const coop::net::CreatureHitIntentPayload& p = q.req;
    if (!s.IsSlotWorldReady(slot)) return Refuse(slot, "slot not world-ready");
    if (!q.haveEye) return Refuse(slot, "no eye");
    if (p.verb != coop::net::kCreatureHitVerbAddDamage) return Refuse(slot, "unjudged verb");
    void* creature = CreatureOf(p.creatureEid);
    if (!creature) return Refuse(slot, "no such creature");

    const FVector hitLoc{p.hitLoc[0], p.hitLoc[1], p.hitLoc[2]};
    const FVector impact{p.impact[0], p.impact[1], p.impact[2]};
    float damage = 0.f;
    const char* why = nullptr;
    if (!coop::hit_judge::JudgeHit(slot, q.eye, hitLoc, creature, p.damage, damage, why))
        return Refuse(slot, why);

    // The first applied hit of a class says what it measured; the rest are counted (g_applied). Read before the
    // hit, which may kill the creature.
    void* const cls = R::ClassOf(creature);
    const bool say = !g_appliedSaid.Find(cls);
    float reach = 0.f, target = 0.f;
    std::wstring name;
    if (say) {
        FVector at{};
        E::TryGetActorLocation(creature, at);
        reach = Dist(q.eye, hitLoc);
        target = Dist(hitLoc, at);
        name = R::ClassNameOf(creature);
    }
    if (!ue_wrap::int_objects::InvokeAddDamage(creature, nullptr, damage, hitLoc, impact))
        return Refuse(slot, "addDamage did not run");
    ++g_applied;
    if (say) {
        g_appliedSaid.Insert(cls, true);
        UE_LOGI("creature_hit_intent: slot=%d creature=%ls damage=%.1f reach=%.0f target=%.0f applied (first of its "
                "class; the rest are counted)", slot, name.c_str(), damage, reach, target);
    }
}

void ServeQueues(coop::net::Session& s) {
    // The weapon table is built once the host's session runs, not on a client's first hit: its build finds the table
    // by walking every object.
    static bool sWeaponsReady = false;
    if (!sWeaponsReady) sWeaponsReady = ue_wrap::weapon_catalog::Ready();
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
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
    if (!session || session->role() != coop::net::Role::Client || g_reg != Reg::Pending) return;
    // A client cancels the hit by this watch; the gate refuses a watch it cannot keep for the process.
    g_reg = sg::WatchName(kRowAddDamage, kTag, &OnAddDamagePre, nullptr) ? Reg::Registered : Reg::Refused;
}

void Tick() {
    if (!g_settled && g_reg != Reg::Pending) {
        if (g_reg == Reg::Registered) sg::ResolvePendingNames();
        if (g_reg == Reg::Registered && sg::NameWatchLive(kRowAddDamage, kTag)) {
            g_settled = true;
            UE_LOGI("creature_hit_intent: the watch on addDamage is live");
        } else if (g_reg == Reg::Refused || sg::NameWatchSettled(kRowAddDamage, kTag)) {
            g_settled = true;
            UE_LOGE("creature_hit_intent: the watch on addDamage is dead -- a client's hit on a creature reaches the "
                    "host's creature by no route");
        }
    }
    coop::net::Session* s = LoadSession();
    if (s && s->running() && s->role() == coop::net::Role::Host) ServeQueues(*s);
}

void OnIntent(int senderSlot, const coop::net::CreatureHitIntentPayload& payload) {
    coop::net::Session* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGW("creature_hit_intent: CreatureHitIntent received on a non-host -- dropping");
        return;
    }
    // Slot 0 is the host, whose own swings never arrive over the wire.
    if (senderSlot < 1 || senderSlot >= coop::players::kMaxPeers) return;
    // A full queue refuses before anything is read: a flood must not pay the eye read per message.
    if (g_slots[senderSlot].queue.size() >= kMaxPending) return Refuse(senderSlot, "queue full");
    Queued q;
    q.req = payload;
    // Only a puppet that has had its first pose has an eye: before it, it stands at its spawn point.
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(senderSlot);
    q.haveEye = rp.valid() && rp.HasPose() && rp.TryGetEyePosition(q.eye);
    g_slots[senderSlot].queue.Push(std::move(q), kMaxPending);   // room was checked above
}

void OnDisconnect() {
    g_predictedCount = 0;
    g_paramsWarned = false;
    g_appliedSaid.Clear();
}

uint32_t AppliedCount() { return g_applied; }

uint32_t RefusedCount(int slot, const char* reason) {
    if (slot < 1 || slot >= coop::players::kMaxPeers || !reason) return 0;
    for (const Reason& r : g_slots[slot].reasons)
        if (r.why && std::strcmp(r.why, reason) == 0) return r.count;
    return 0;
}

}  // namespace coop::creature_hit_intent
