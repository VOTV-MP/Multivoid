// coop/player/hand_item.cpp -- see coop/player/hand_item.h for the design.

#include "coop/player/hand_item.h"

#include "coop/net/latest_wins.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/player/hand_item_pose.h"  // where the mirror sits: the owner's measure, the per-tick drive
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/prop_lifecycle.h"  // the two prop families never on the wire (ResolveHandClass)
#include "coop/props/trash_collect_sync.h"  // EnsureHeldItemBroadcast (hand-edge world release)
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/actors/inventory.h"   // the hold slot's item name
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"  // the prop base's class name (ResolveHandClass)
#include "ue_wrap/core/types.h"

#include <cmath>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace coop::hand_item {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// Wire caps: class names and item names are short ASCII, one length byte each.
constexpr size_t kMaxStr = 63;
// The largest HandItem: slot, has, three length-prefixed strings at the cap, the hold transform.
static_assert(2 + 3 * (1 + kMaxStr) + 6 * sizeof(float) <= static_cast<size_t>(coop::net::kMaxReliablePayload),
              "a HandItem at its string caps must fit one reliable datagram");

// The fallback placement until the owner's first measure lands (coop/player/hand_item_pose measures
// and drives the view-space hold): camera-front constants.
constexpr float kFallbackRelPos[3] = {45.f, 10.f, -40.f};  // fwd/right/up cm

// Per-slot wire state: what each peer's hand holds.
struct SlotHand {
    bool has = false;
    std::wstring cls;   // BP class name, e.g. L"prop_physgun_C"
    std::wstring name;  // Aprop_C 'name' FName (drives the generic prop mesh), may be empty
    std::wstring item;  // the hold slot's item name, which the game's item tables are keyed by
    std::wstring lastCls;  // the class held last: `cls` while it holds one, the one before once empty
    // The measured view-relative hold transform, owner-authored and wire-carried.
    float relPos[3] = {kFallbackRelPos[0], kFallbackRelPos[1], kFallbackRelPos[2]};
    float relRot[3] = {0.f, 0.f, 0.f};  // item rotator {pitch,yaw,roll} in the view frame
    bool  haveRel = false;              // owner measured (vs the fallback defaults)
};
SlotHand g_hands[coop::players::kMaxPeers];  // GT-only

// The per-slot display mirror. A spawn that failed keeps the item and the puppet it was tried for and
// is not tried again until either changes: a begin or a finish that failed for them fails the same way
// on the next tick, and each attempt is a spawn.
struct Mirror {
    void*        actor = nullptr;
    int32_t      idx   = -1;  // GUObjectArray index (IsLiveByIndex validation)
    std::wstring cls;
    std::wstring name;
    int32_t      puppetIdx = -1;     // the puppet we attached to (re-attach on respawn)...
    int32_t      puppetSerial = 0;   // ...and its slot's serial: a successor in the slot is a new puppet
    bool         failed = false;     // the spawn of cls/name on this puppet failed; actor is null
};
Mirror g_mirrors[coop::players::kMaxPeers];  // GT-only

// The session, cached by TickOwner for the connect-replay entry that only receives a slot.
// Game thread only.
coop::net::Session* g_session = nullptr;

// Owner-side change detection.
bool         g_ownHas = false;
std::wstring g_ownCls;
std::wstring g_ownName;
std::wstring g_ownItem;
// The last-seen holding actor, pointer and captured index, since the game recycles addresses.
// While unchanged, TickOwner skips the class and name renders, which allocate engine-side per
// call.
void*        g_ownHeldPtr = nullptr;
int32_t      g_ownHeldIdx = -1;

// The local player, cached by TickOwner for the fresh holding-actor read. Game thread only.
void*        g_localPlayer    = nullptr;
int32_t      g_localPlayerIdx = -1;

// The hand-edge world release. A drop or quick-slot place is not a spawn: the game releases
// the ex-hand actor into the world (the same actor, with physics and collision re-enabled),
// so no spawn seam fires and the census guard is blind. But the previous hand actor's identity
// is held here, so at the hand edge an ex-hand actor that survived the edge (a stow or switch
// destroys it) is expressed immediately through the canonical untracked-prop broadcast, on
// both roles.
void ExpressReleasedHandActor(coop::net::Session& session, void* prev, int32_t prevIdx) {
    if (!prev || !R::IsLiveByIndex(prev, prevIdx)) {
        // Diagnostic: the previous actor dead at the hand edge means it was destroyed in hand (a
        // stow or switch, or a drop path that destroys the hand actor and spawns a separate world
        // actor), so the same-actor release does not run. The pointer is logged, never
        // dereferenced.
        if (prev) {
            UE_LOGI("[ROCK-DROP] hand-edge: ex-hand actor %p DEAD (destroyed in-hand) -- the "
                    "'release surviving actor' express does NOT run; a fresh world spawn (if any) "
                    "is authored elsewhere (client Aprop spawns are host-auth-skipped)", prev);
        }
        return;
    }
    // The previous actor survived the hand edge: a genuine same-actor release into the world.
    if (coop::trash_collect_sync::EnsureHeldItemBroadcast(prev, &session)) {
        UE_LOGI("hand_item: released ex-hand actor %p expressed as a world prop "
                "(R-drop/place -- peers see it this tick)", prev);
    } else {
        UE_LOGI("[ROCK-DROP] hand-edge: ex-hand actor %p SURVIVED but EnsureHeldItemBroadcast "
                "DECLINED it (see the decline reason above) -- not expressed", prev);
    }
}

// The prop name field's offset, resolved once; the field lives on the prop base class, and
// every hotbar item is a descendant.
int32_t PropNameOffset() {
    static int32_t s_off = -1;
    if (s_off >= 0) return s_off;
    if (void* propCls = R::FindClass(L"prop_C"))
        s_off = R::FindPropertyOffset(propCls, L"name");
    return s_off;
}

// What a class name off the wire is to this lane. The owner announces its holding actor only when it
// is a prop descendant (local_streams), so a hand item is a prop; and not of the two prop families the
// coop layer never puts on the wire (prop_lifecycle): a per-player state actor, of which each peer owns
// exactly one, and a wire-suppressed variant, which a client's own Init destroys. The receipt and the
// spawn both ask it, so the two cannot drift. The index finds a class whatever the letter case of the
// name, and the family predicates compare without case too. Without the prop base this peer cannot
// judge, and says so rather than refusing. `outOwn`, when asked, gets a hand item's class as the engine
// spells it.
enum class HandClass { Hand, NotHand, Unresolved };
HandClass ResolveHandClass(const std::wstring& name, void** outCls, std::wstring* outOwn) {
    if (outCls) *outCls = nullptr;
    // The two families are named classes: refused by name, loaded or not.
    if (coop::prop_lifecycle::IsPerPlayerPropClass(name) || coop::prop_lifecycle::IsWireSuppressedPropClass(name))
        return HandClass::NotHand;
    void* cls = ue_wrap::object_index::ClassByName(name.c_str());
    if (outCls) *outCls = cls;
    if (!cls) return HandClass::Unresolved;
    void* base = ue_wrap::object_index::ClassByName(ue_wrap::profile::name::PropClass);
    if (!base) return HandClass::Unresolved;
    if (!ue_wrap::prop::WalksToBase(cls, base)) return HandClass::NotHand;
    if (outOwn) *outOwn = R::ToString(R::NameOf(cls));
    return HandClass::Hand;
}

// HandItems this peer dropped or refused, per sender, for the log: the first and then each power of two,
// so a sender at the edge budget's rate cannot flood it. The count when this drop is one to log, else 0.
uint32_t g_droppedBySender[coop::players::kMaxPeers] = {};
uint32_t DropToLog(int sender) {
    if (sender < 0 || sender >= static_cast<int>(coop::players::kMaxPeers)) return 1;
    const uint32_t n = ++g_droppedBySender[sender];
    return (n & (n - 1)) == 0 ? n : 0;
}
// The class each slot's mirror last waited on unresolved, and how many such classes it has waited on,
// warned at the first and each power of two, so a slot whose class keeps changing cannot flood the log.
std::wstring g_unresolvedWarned[coop::players::kMaxPeers];
uint32_t     g_unresolvedBySlot[coop::players::kMaxPeers] = {};

std::wstring ReadItemName(void* actor) {
    const int32_t off = PropNameOffset();
    if (off < 0 || !actor) return {};
    R::FName n{};
    std::memcpy(&n, reinterpret_cast<const uint8_t*>(actor) + off, sizeof(n));
    std::wstring s = R::ToString(n);
    if (s == L"None") s.clear();
    return s;
}

// The payload: slot, has, a length-prefixed class, name and item, and when held the measured
// view-relative position and rotation.
std::vector<uint8_t> BuildPayload(uint8_t slot, const SlotHand& h) {
    std::vector<uint8_t> out;
    const size_t cl = h.has ? (h.cls.size() > kMaxStr ? kMaxStr : h.cls.size()) : 0;
    const size_t nl = h.has ? (h.name.size() > kMaxStr ? kMaxStr : h.name.size()) : 0;
    const size_t il = h.has ? (h.item.size() > kMaxStr ? kMaxStr : h.item.size()) : 0;
    out.reserve(5 + cl + nl + il + (h.has ? 24 : 0));
    out.push_back(slot);
    out.push_back(h.has ? 1 : 0);
    out.push_back(static_cast<uint8_t>(cl));
    for (size_t i = 0; i < cl; ++i) out.push_back(static_cast<uint8_t>(h.cls[i]));
    out.push_back(static_cast<uint8_t>(nl));
    for (size_t i = 0; i < nl; ++i) out.push_back(static_cast<uint8_t>(h.name[i]));
    out.push_back(static_cast<uint8_t>(il));
    for (size_t i = 0; i < il; ++i) out.push_back(static_cast<uint8_t>(h.item[i]));
    if (h.has) {
        const float rel[6] = {h.relPos[0], h.relPos[1], h.relPos[2],
                              h.relRot[0], h.relRot[1], h.relRot[2]};
        const size_t base = out.size();
        out.resize(base + sizeof(rel));
        std::memcpy(out.data() + base, rel, sizeof(rel));
    }
    return out;
}

void SendState(coop::net::Session& session, uint8_t slot, const SlotHand& h) {
    const std::vector<uint8_t> p = BuildPayload(slot, h);
    if (session.role() == coop::net::Role::Host) {
        for (int x = 1; x < coop::net::kMaxPeers; ++x) {
            if (x == slot) continue;                 // never echo the originator
            if (!session.IsSlotReady(x)) continue;
            session.SendReliableToSlot(x, coop::net::ReliableKind::HandItem,
                                       p.data(), static_cast<int>(p.size()));
        }
    } else {
        session.SendReliableToSlot(0, coop::net::ReliableKind::HandItem,
                                   p.data(), static_cast<int>(p.size()));
    }
}

// Host: the hand states applied for a slot's occupant (StatesApplied).
coop::roster_ledger::PerSlotState<uint32_t> g_statesApplied;

// Host: one client's hand state applied, field by field: `lastCls` moves only while the hand holds
// something, since the pry gate reads it once the hand is empty (LastHeldClassIs). Then re-sent to
// every other ready client.
void ApplyHandState(coop::net::Session& session, uint8_t slot, const SlotHand& in) {
    SlotHand& h = g_hands[slot];
    h.has = in.has; h.cls = in.cls; h.name = in.name; h.item = in.item;
    if (in.has) h.lastCls = in.cls;
    std::memcpy(h.relPos, in.relPos, sizeof(h.relPos));
    std::memcpy(h.relRot, in.relRot, sizeof(h.relRot));
    h.haveRel = in.has;
    SendState(session, slot, h);
    ++g_statesApplied[slot];
}

// The hand's budget: an application is a mirror destroyed and spawned on every
// peer and two sends. Twelve at once and four a second sit above the most an honest player sent in the archived logs
// (9 in a second, 32 in ten); past them the newest state lands within a quarter second.
constexpr coop::net::IntentBudget kHandBudget{12.0f, 4.0f};

// Host: per slot, the newest changed hand state and its pace (coop/net/latest_wins.h); one struct, so both clear with the
// occupant.
struct PendingHand { SlotHand value; coop::net::LatestWins pace; };
coop::roster_ledger::PerSlotState<PendingHand> g_pendingHand;

// Whether two states hold the same thing: emptiness, the class without case, the name and the item. The hold
// transform is not compared: the pose stream overwrites the applied one each sample.
bool SameHeld(const SlotHand& a, const SlotHand& b) {
    if (a.has != b.has) return false;
    return !a.has || (::_wcsicmp(a.cls.c_str(), b.cls.c_str()) == 0 && a.name == b.name && a.item == b.item);
}

void DestroyMirror(uint8_t slot, const char* why) {
    Mirror& m = g_mirrors[slot];
    if (m.actor && R::IsLiveByIndex(m.actor, m.idx)) {
        // Suppress the destroy observer's echo; the host would otherwise broadcast a destroy for a
        // display-only actor.
        coop::prop_echo_suppress::MarkIncomingDestroy(m.actor);
        E::DestroyActor(m.actor);
        UE_LOGI("hand_item: slot %u mirror destroyed (%s)", static_cast<unsigned>(slot), why);
    }
    m = Mirror{};
}

void NoteSpawnFailed(uint8_t slot, const SlotHand& want, void* puppetActor) {
    Mirror& m = g_mirrors[slot];
    m = Mirror{};
    m.cls = want.cls;
    m.name = want.name;
    m.puppetIdx = R::InternalIndexOf(puppetActor);
    m.puppetSerial = R::AllocateSlotSerial(m.puppetIdx);
    m.failed = true;
}

void SpawnMirror(uint8_t slot, void* puppetActor) {
    const SlotHand& want = g_hands[slot];
    // Two index lookups a tick, the class and the prop base: Blueprint classes load on demand, and one
    // this peer has not loaded, or is still loading, answers null and is asked for again next tick; the
    // classes a slot waits on are warned at the first and each power of two.
    void* cls = nullptr;
    const HandClass kind = ResolveHandClass(want.cls, &cls, nullptr);
    if (kind == HandClass::Unresolved) {
        if (::_wcsicmp(g_unresolvedWarned[slot].c_str(), want.cls.c_str()) != 0) {
            g_unresolvedWarned[slot] = want.cls;
            const uint32_t n = ++g_unresolvedBySlot[slot];
            if ((n & (n - 1)) == 0)
                UE_LOGW("hand_item: slot %u class '%ls' or the prop base not loaded yet -- will retry (%u class(es) "
                        "waited on for this slot)", static_cast<unsigned>(slot), want.cls.c_str(), n);
        }
        return;
    }
    // A mirror is built only from a hand item's class, whoever authored the state: the name stamp below
    // writes at the prop name field's offset, which only a prop has. This is also where a class judged
    // nowhere else is judged: one the host could not resolve when it relayed it, and, on a client, the
    // host's own hand, which no receipt checks. MTA's receiving client builds a vehicle from a model id off
    // the wire only once its own table accepts it
    // (reference/mtasa-blue/Client/mods/deathmatch/logic/CPacketHandler.cpp:3278-3282).
    if (kind == HandClass::NotHand) {
        UE_LOGW("hand_item: slot %u class '%ls' is not a hand item -- no mirror, not tried again until the "
                "item or the puppet changes", static_cast<unsigned>(slot), want.cls.c_str());
        NoteSpawnFailed(slot, want, puppetActor);
        return;
    }
    ue_wrap::FVector loc{};
    if (!E::TryGetActorLocation(puppetActor, loc)) return;   // retried next tick, as a missing class is
    const ue_wrap::FRotator rot{};
    // The begin dispatches through ProcessEvent, so the spawn observers (the ambient broadcaster's among
    // them, which a held pinecone, stick or crystal passes) run inside it, before the actor exists to be
    // marked; the mirror-spawn scope tells them it is a display's.
    void* actor = nullptr;
    {
        coop::prop_echo_suppress::ScopedMirrorSpawn mirrorScope;
        actor = E::BeginDeferredSpawn(cls, loc, rot);
    }
    if (!actor) {
        UE_LOGW("hand_item: slot %u BeginDeferredSpawn '%ls' failed -- not tried again until the item or the "
                "puppet changes", static_cast<unsigned>(slot), want.cls.c_str());
        NoteSpawnFailed(slot, want, puppetActor);
        return;
    }
    // The item name is stamped before Init runs: generic prop meshes are name-driven, the game's
    // own updateHold pattern.
    if (!want.name.empty()) {
        const int32_t off = PropNameOffset();
        if (off >= 0) {
            R::FName n = ue_wrap::fname_utils::StringToFName(want.name);
            std::memcpy(reinterpret_cast<uint8_t*>(actor) + off, &n, sizeof(n));
        }
    }
    // Suppress the Init spawn-catch echo: a display mirror must never be expressed as a world
    // prop.
    coop::prop_echo_suppress::MarkMirrorSpawn(actor);
    if (!E::FinishDeferredSpawn(actor, loc, rot)) {
        // The begun actor is not left in the world, half made: it is destroyed, silently to the peers.
        UE_LOGW("hand_item: slot %u FinishDeferredSpawn '%ls' failed -- the begun actor is destroyed, and the "
                "spawn is not tried again until the item or the puppet changes",
                static_cast<unsigned>(slot), want.cls.c_str());
        coop::prop_echo_suppress::MarkIncomingDestroy(actor);
        E::DestroyActor(actor);
        NoteSpawnFailed(slot, want, puppetActor);
        return;
    }
    E::SetActorSimulatePhysics(actor, false);
    E::SetActorEnableCollision(actor, false);
    Mirror& m = g_mirrors[slot];
    m = Mirror{};
    m.actor = actor;
    m.idx = R::InternalIndexOf(actor);
    m.cls = want.cls;
    m.name = want.name;
    m.puppetIdx = R::InternalIndexOf(puppetActor);
    m.puppetSerial = R::AllocateSlotSerial(m.puppetIdx);
    UE_LOGI("hand_item: slot %u mirror SPAWNED cls='%ls' name='%ls' (display-only, view-anchored)",
            static_cast<unsigned>(slot), want.cls.c_str(), want.name.c_str());
}

}  // namespace

void TickOwner(coop::net::Session& session, void* local, void* holdingProp) {
    g_session = &session;
    if (local && local != g_localPlayer) {
        g_localPlayer    = local;
        g_localPlayerIdx = R::InternalIndexOf(local);
    }
    if (!session.connected()) return;
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    if (self >= coop::players::kMaxPeers) return;

    if (holdingProp) {
        // The live view-relative hold transform is measured and latched every tick while holding;
        // the net thread streams it, so a melee swing reads as continuous motion on the mirror.
        // Before the identity-latch early return: motion must flow even with the held identity
        // unchanged.
        {
            float sp[3], sr[3];
            if (coop::hand_item_pose::MeasureLocalHoldRelative(local, holdingProp, sp, sr)) {
                coop::net::HandPoseSnapshot hp{};
                std::memcpy(hp.relPos, sp, sizeof(sp));
                std::memcpy(hp.relRot, sr, sizeof(sr));
                session.SetLocalHandPose(true, hp);
            }
        }
        // O(1) steady state: the same live actor as last tick skips the renders, except a re-check
        // every 30 ticks, since the game renames a held prop in place (arming a grenade), which a
        // pointer latch would never see.
        static uint32_t sSameStreak = 0;
        const int32_t idx = R::InternalIndexOf(holdingProp);
        if (holdingProp == g_ownHeldPtr && idx == g_ownHeldIdx &&
            (++sSameStreak % 30) != 0) {
            return;
        }
        if (holdingProp != g_ownHeldPtr && g_ownHeldPtr) {
            // The identity changed with a previous actor latched: if it survived, it was released
            // to the world (place, then the next slot).
            ExpressReleasedHandActor(session, g_ownHeldPtr, g_ownHeldIdx);
        }
        g_ownHeldPtr = holdingProp;
        g_ownHeldIdx = idx;
        const std::wstring cls = R::ClassNameOf(holdingProp);
        const std::wstring name = ReadItemName(holdingProp);
        const std::wstring item = ue_wrap::inventory::ReadHeldItemName();
        if (!g_ownHas || cls != g_ownCls || name != g_ownName || item != g_ownItem) {
            g_ownHas = true;
            g_ownCls = cls;
            g_ownName = name;
            g_ownItem = item;
            SlotHand& h = g_hands[self];
            h.has = true; h.cls = cls; h.name = name; h.item = item; h.lastCls = cls;
            // The first measure of the weld's view-relative transform, which may land a tick early;
            // the periodic re-check corrects it. Measured into scratch and reset on failure: on a
            // quick-slot switch a failed measure would otherwise ship the previous item's transform
            // under the new item's name, plausible values the receiver cannot reject.
            float p[3], rr[3];
            if (coop::hand_item_pose::MeasureLocalHoldRelative(local, holdingProp, p, rr)) {
                std::memcpy(h.relPos, p, sizeof(p));
                std::memcpy(h.relRot, rr, sizeof(rr));
                h.haveRel = true;
            } else {
                h.relPos[0] = kFallbackRelPos[0];
                h.relPos[1] = kFallbackRelPos[1];
                h.relPos[2] = kFallbackRelPos[2];
                h.relRot[0] = h.relRot[1] = h.relRot[2] = 0.f;
                h.haveRel = false;
            }
            SendState(session, self, h);
            UE_LOGI("hand_item: local hand -> cls='%ls' name='%ls' item='%ls' (announced, rel %s)",
                    cls.c_str(), name.c_str(), item.c_str(), h.haveRel ? "measured" : "fallback");
            // No reconcile request here: the pickup's ground-actor death is caught at the destroy
            // seam, and the stream carries the live transform.
        }
    } else if (g_ownHas) {
        session.SetLocalHandPose(false, {});  // stop the stream on the empty edge
        // The hand is empty this tick: an ex-hand actor that survived the edge was released into
        // the world, so express it now, then drop the identity latch so a re-appearance re-renders
        // and re-compares.
        ExpressReleasedHandActor(session, g_ownHeldPtr, g_ownHeldIdx);
        g_ownHeldPtr = nullptr;
        g_ownHeldIdx = -1;
        // The stow announce is edge-instant: a quick-slot switch is one synchronous updateHold call
        // (destroy, spawn, then the holding name), so a poll never observes a mid-switch null and
        // no debounce is needed.
        g_ownHas = false;
        g_ownCls.clear();
        g_ownName.clear();
        g_ownItem.clear();
        SlotHand& h = g_hands[self];
        const std::wstring last = h.lastCls;
        h = SlotHand{};
        h.lastCls = last;
        SendState(session, self, h);
        UE_LOGI("hand_item: local hand -> EMPTY (announced)");
        // No reconcile request: the released actor was expressed above, and inventory spawns ride
        // the finish-spawn seam.
    }
}

void* LocalHandActor() {
    if (!g_localPlayer || !R::IsLiveByIndex(g_localPlayer, g_localPlayerIdx))
        return nullptr;
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (!ue_wrap::engine::ReadMainPlayerGrabState(g_localPlayer, gs)) return nullptr;
    void* ha = gs.holdingActor;
    if (!ha || !R::IsLive(ha)) return nullptr;
    // The same routing predicate as local_streams: only a prop descendant in the holding actor is
    // the hotbar hand; the clump or pile carry is a world entity and must stay adoptable.
    if (!ue_wrap::prop::IsDescendantOfProp(ha)) return nullptr;
    return ha;
}

void* MirrorActorForSlot(uint8_t slot) {
    if (slot >= coop::players::kMaxPeers) return nullptr;
    const Mirror& m = g_mirrors[slot];
    return (m.actor && R::IsLiveByIndex(m.actor, m.idx)) ? m.actor : nullptr;
}

size_t CollectHandAxisActors(void* out[], size_t cap) {
    size_t n = 0;
    if (void* lh = LocalHandActor(); lh && n < cap) out[n++] = lh;
    for (uint8_t slot = 0; slot < coop::players::kMaxPeers && n < cap; ++slot) {
        const Mirror& m = g_mirrors[slot];
        // IsLiveByIndex, not a bare pointer match: a dead mirror's recycled address belongs to a
        // different actor that must stay adoptable.
        if (m.actor && R::IsLiveByIndex(m.actor, m.idx)) out[n++] = m.actor;
    }
    return n;
}

bool IsHandAxisActor(void* actor) {
    if (!actor) return false;
    void* axis[1 + coop::players::kMaxPeers];
    const size_t n = CollectHandAxisActors(axis, 1 + coop::players::kMaxPeers);
    for (size_t i = 0; i < n; ++i)
        if (axis[i] == actor) return true;
    return false;
}

void TickMirrors() {
    auto& reg = coop::players::Registry::Get();
    const uint8_t self = reg.LocalPeerId();
    for (uint8_t slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        if (slot == self) continue;  // never mirror our own hand
        const SlotHand& want = g_hands[slot];
        Mirror& m = g_mirrors[slot];
        const bool mirrorLive = m.actor && R::IsLiveByIndex(m.actor, m.idx);

        if (!want.has) {
            if (mirrorLive || m.actor) DestroyMirror(slot, "hand now empty");
            else if (m.failed) m = Mirror{};  // the failed item left the hand; the next one is tried
            continue;
        }
        coop::RemotePlayer* pup = reg.Puppet(slot);
        // valid() is IsLiveByIndex on the puppet's captured index, never bare IsLive on the cached
        // actor.
        void* puppetActor = (pup && pup->valid()) ? pup->GetActor() : nullptr;
        if (!puppetActor) {
            // The puppet not up yet (the join window) or torn down: keep the state, drop any
            // orphaned mirror, retry next tick.
            if (mirrorLive || m.actor) DestroyMirror(slot, "puppet gone");
            continue;
        }
        // The freshest streamed hold transform overrides the announce-time one; the announce only
        // covers the pre-stream frame.
        if (g_session) {
            coop::net::HandPoseSnapshot hp;
            bool isNew = false;
            if (g_session->TryGetRemoteHandPose(slot, hp, &isNew) && isNew) {
                SlotHand& wh = g_hands[slot];
                std::memcpy(wh.relPos, hp.relPos, sizeof(hp.relPos));
                std::memcpy(wh.relRot, hp.relRot, sizeof(hp.relRot));
                wh.haveRel = true;
            }
        }
        const int32_t pupIdx = R::InternalIndexOf(puppetActor);
        const bool sameWant = ::_wcsicmp(m.cls.c_str(), want.cls.c_str()) == 0 && m.name == want.name &&
                              m.puppetIdx == pupIdx &&
                              R::SlotSerial(pupIdx) == m.puppetSerial;
        if (mirrorLive && sameWant) {
            coop::hand_item_pose::DriveMirror(m.actor, pup, want.relPos, want.relRot);  // steady state: re-command the view hold
            continue;
        }
        if (m.failed && sameWant) continue;  // this item failed to spawn on this puppet (see Mirror)
        if (mirrorLive || m.actor) DestroyMirror(slot, "state/puppet changed");
        SpawnMirror(slot, puppetActor);
        Mirror& nm = g_mirrors[slot];
        if (nm.actor) coop::hand_item_pose::DriveMirror(nm.actor, pup, want.relPos, want.relRot);  // no first-frame pop at spawn pos
    }
}

bool HandleHandItem(coop::net::Session& session,
                    const coop::net::Session::ReliableMessage& msg) {
    UE_ASSERT_GAME_THREAD("HandleHandItem");
    if (msg.payloadLen < 4) {
        if (const uint32_t n = DropToLog(msg.senderPeerSlot))
            UE_LOGW("hand_item: payload %zu B too short -- dropping (%u from slot %d)",
                    static_cast<size_t>(msg.payloadLen), n, msg.senderPeerSlot);
        return true;
    }
    const uint8_t* p = msg.payload;
    const size_t len = static_cast<size_t>(msg.payloadLen);
    size_t off = 0;
    const uint8_t describedSlot = p[off++];
    const bool has = p[off++] != 0;
    // Each string: within the cap BuildPayload writes to, so what the host keeps is what it relays, and
    // with no control character -- a NUL would end the name the class index reads before the string
    // does, and a line break would split the log lines that quote it.
    auto readStr = [&](const char* field, std::wstring& out) -> bool {
        if (off + 1 > len) {
            if (const uint32_t c = DropToLog(msg.senderPeerSlot))
                UE_LOGW("hand_item: no %s field -- dropping (%u from slot %d)", field, c, msg.senderPeerSlot);
            return false;
        }
        const uint8_t n = p[off++];
        bool control = false;
        for (size_t i = off; i < off + n && i < len; ++i) control = control || p[i] < 0x20 || p[i] == 0x7F;
        const char* why = off + n > len ? "truncated"
                        : n > kMaxStr   ? "over the cap"
                        : control       ? "with a control character"
                                        : nullptr;
        if (why) {
            if (const uint32_t c = DropToLog(msg.senderPeerSlot))
                UE_LOGW("hand_item: %s field %s -- dropping (%u from slot %d)", field, why, c, msg.senderPeerSlot);
            return false;
        }
        for (uint8_t i = 0; i < n; ++i) out.push_back(static_cast<wchar_t>(p[off++]));
        return true;
    };
    std::wstring cls, name, item;
    if (!readStr("class", cls) || !readStr("name", name) || !readStr("item", item)) return true;

    // The measured view-relative hold transform, present whenever held. Insane values (non-finite,
    // out of arm's reach) keep the fallback placement.
    float relPos[3] = {kFallbackRelPos[0], kFallbackRelPos[1], kFallbackRelPos[2]};
    float relRot[3] = {0.f, 0.f, 0.f};
    if (has) {
        if (off + 24 > len) {
            if (const uint32_t n = DropToLog(msg.senderPeerSlot))
                UE_LOGW("hand_item: missing hold-transform field -- dropping (%u from slot %d)", n,
                        msg.senderPeerSlot);
            return true;
        }
        float rel[6];
        std::memcpy(rel, p + off, sizeof(rel));
        off += sizeof(rel);
        bool sane = true;
        for (int i = 0; i < 6; ++i) if (!std::isfinite(rel[i])) sane = false;
        for (int i = 0; i < 3; ++i) if (!(rel[i] > -300.f && rel[i] < 300.f)) sane = false;
        if (sane) {
            relPos[0] = rel[0]; relPos[1] = rel[1]; relPos[2] = rel[2];
            relRot[0] = rel[3]; relRot[1] = rel[4]; relRot[2] = rel[5];
        }
    }

    const char* malformed = describedSlot >= coop::net::kMaxPeers ? "a slot out of range"
                          : has && cls.empty()                   ? "an item with no class"
                                                                 : nullptr;
    if (malformed) {
        if (const uint32_t n = DropToLog(msg.senderPeerSlot))
            UE_LOGW("hand_item: %s -- dropping (%u from slot %d)", malformed, n, msg.senderPeerSlot);
        return true;
    }

    if (session.role() == coop::net::Role::Host) {
        // The forgery guard: a client may only describe its own hand.
        if (msg.senderPeerSlot != describedSlot || describedSlot == 0) {
            if (const uint32_t n = DropToLog(msg.senderPeerSlot))
                UE_LOGW("hand_item: slot=%u from senderSlot=%d -- forged, dropping (%u from that slot)",
                        static_cast<unsigned>(describedSlot), msg.senderPeerSlot, n);
            return true;
        }
        // A class that is not a hand item -- one of the two families by name, or a class this host resolves
        // to something else -- is not kept and not relayed, and the slot keeps the state it had; the link
        // stays. Any other class this host cannot resolve yet is kept and relayed, and
        // judged where it would be built (SpawnMirror): the owner announces only on a change, so an
        // honest state refused here would never come again. MTA's server drops a weapon id outside the
        // set it knows and keeps the link
        // (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CBulletsyncPacket.cpp:78-82). It
        // can also hold a weapon against its own record of the player; this host's only copy of a
        // client's hold slot is the profile that client streams itself, so it checks the lane's type. A
        // hand item is kept and relayed under its class's own spelling, and every peer compares the class
        // without case.
        SlotHand in;
        in.has = has; in.cls = std::move(cls); in.name = std::move(name); in.item = std::move(item);
        std::memcpy(in.relPos, relPos, sizeof(relPos));
        std::memcpy(in.relRot, relRot, sizeof(relRot));
        // The newest state kept, applied at the budget's pace (coop/net/latest_wins.h). An unchanged one costs the
        // parse and a compare, ahead of the class gate: the applied state passed it, and the compare reads the
        // class without case. The host's verbs and its mirror read the applied state, at most a refill behind.
        PendingHand& pend = g_pendingHand[describedSlot];
        if (SameHeld(in, g_hands[describedSlot])) {
            pend.pace.Withdraw();
            return true;
        }
        std::wstring own;
        const HandClass kind = in.has ? ResolveHandClass(in.cls, nullptr, &own) : HandClass::Unresolved;
        if (kind == HandClass::NotHand) {
            if (const uint32_t n = DropToLog(msg.senderPeerSlot))
                UE_LOGW("hand_item: slot %u names class '%ls', which is not a hand item -- not kept or relayed "
                        "(%u from this slot)", static_cast<unsigned>(describedSlot), in.cls.c_str(), n);
            return true;
        }
        if (kind == HandClass::Hand) in.cls = own;
        pend.value = in;
        if (pend.pace.Offer(kHandBudget, coop::net::NowMs())) ApplyHandState(session, describedSlot, in);
        return true;
    }

    // Client: only the host relays hand states.
    if (msg.senderPeerSlot != 0) {
        if (const uint32_t n = DropToLog(msg.senderPeerSlot))
            UE_LOGW("hand_item: HandItem from non-host senderPeerSlot=%d -- dropping (%u from that slot)",
                    msg.senderPeerSlot, n);
        return true;
    }
    if (describedSlot == coop::players::Registry::Get().LocalPeerId()) return true;  // own echo
    SlotHand& h = g_hands[describedSlot];
    h.has = has; h.cls = cls; h.name = name; h.item = item;
    if (has) h.lastCls = cls;
    std::memcpy(h.relPos, relPos, sizeof(relPos));
    std::memcpy(h.relRot, relRot, sizeof(relRot));
    h.haveRel = has;
    return true;
}

void ReplayPeerStatesToSlot(int slot) {
    coop::net::Session* session = g_session;
    if (!session || session->role() != coop::net::Role::Host) return;
    if (slot < 1 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    for (uint8_t s = 0; s < coop::players::kMaxPeers; ++s) {
        if (s == slot) continue;
        if (!g_hands[s].has) continue;  // empty is the joiner's default -- nothing to send
        const std::vector<uint8_t> p = BuildPayload(s, g_hands[s]);
        session->SendReliableToSlot(slot, coop::net::ReliableKind::HandItem,
                                    p.data(), static_cast<int>(p.size()));
        UE_LOGI("hand_item: connect-replay -- slot %u holds '%ls' -> joiner slot %d",
                static_cast<unsigned>(s), g_hands[s].cls.c_str(), slot);
    }
}

void Reset() {
    // Stop the hand-pose stream before the session cache drops, or a stale latch would stream the
    // dead session's transform into the next one.
    if (g_session) g_session->SetLocalHandPose(false, {});
    for (uint8_t s = 0; s < coop::players::kMaxPeers; ++s) {
        DestroyMirror(s, "session reset");
        g_hands[s] = SlotHand{};
        g_droppedBySender[s] = 0;
        g_unresolvedWarned[s].clear();
        g_unresolvedBySlot[s] = 0;
    }
    g_ownHas = false;
    g_ownCls.clear();
    g_ownName.clear();
    g_ownItem.clear();
    g_ownHeldPtr = nullptr;
    g_ownHeldIdx = -1;
    // Cached engine pointers die with the session too: a stale local player would satisfy the
    // index check across a world reload only by luck; TickOwner re-caches.
    g_localPlayer    = nullptr;
    g_localPlayerIdx = -1;
    g_session        = nullptr;
}

bool HeldClassIs(uint8_t slot, const wchar_t* cls) {
    if (slot >= coop::players::kMaxPeers || !cls) return false;
    const SlotHand& h = g_hands[slot];
    return h.has && ::_wcsicmp(h.cls.c_str(), cls) == 0;
}

bool LastHeldClassIs(uint8_t slot, const wchar_t* cls) {
    if (slot >= coop::players::kMaxPeers || !cls) return false;
    const SlotHand& h = g_hands[slot];
    return !h.lastCls.empty() && ::_wcsicmp(h.lastCls.c_str(), cls) == 0;
}

void PayOwed(coop::net::Session& session) {
    if (session.role() != coop::net::Role::Host) return;
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot) {
        PendingHand& pend = g_pendingHand[slot];
        if (pend.pace.Owed() && pend.pace.Due(kHandBudget, coop::net::NowMs()))
            ApplyHandState(session, static_cast<uint8_t>(slot), pend.value);
    }
}

bool HoldsItem(uint8_t slot) {
    return slot < coop::players::kMaxPeers && g_hands[slot].has;
}

uint32_t StatesApplied(int slot) {
    return (slot >= 0 && slot < static_cast<int>(coop::players::kMaxPeers)) ? g_statesApplied[slot] : 0;
}

std::wstring HeldItem(uint8_t slot) {
    if (slot >= coop::players::kMaxPeers) return {};
    const SlotHand& h = g_hands[slot];
    return h.has ? h.item : std::wstring();
}

void OnSlotDisconnected(uint8_t slot) {
    if (slot >= coop::players::kMaxPeers) return;
    DestroyMirror(slot, "peer disconnected");
    g_hands[slot] = SlotHand{};
    g_droppedBySender[slot] = 0;
    g_unresolvedWarned[slot].clear();
    g_unresolvedBySlot[slot] = 0;
}

}  // namespace coop::hand_item
