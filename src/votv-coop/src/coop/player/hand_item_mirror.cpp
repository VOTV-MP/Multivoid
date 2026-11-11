// coop/player/hand_item_mirror.cpp -- see coop/player/hand_item_mirror.h.

#include "coop/player/hand_item_mirror.h"

#include "coop/player/hand_item_pose.h"  // where the main hand's mirror sits, the per-tick drive
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/prop_lifecycle.h"  // the two prop families never on the wire (ResolveHandClass)
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"  // the prop base's class name (ResolveHandClass)
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"  // the off-hand mirror on the puppet's weapon_off

#include <cstring>
#include <cwchar>
#include <string>

namespace coop::hand_item {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

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
Mirror g_mirrors[coop::players::kMaxPeers];     // GT-only: the main hands'
Mirror g_offMirrors[coop::players::kMaxPeers];  // GT-only: the off hands'

Mirror& MirrorOf(Hand hand, uint8_t slot) { return hand == Hand::Main ? g_mirrors[slot] : g_offMirrors[slot]; }

// The class each slot's mirror last waited on unresolved, and how many such classes it has waited on,
// warned at the first and each power of two, so a slot whose class keeps changing cannot flood the log.
std::wstring g_unresolvedWarned[coop::players::kMaxPeers];
uint32_t     g_unresolvedBySlot[coop::players::kMaxPeers] = {};

void DestroyMirrorOf(Hand hand, uint8_t slot, const char* why) {
    Mirror& m = MirrorOf(hand, slot);
    if (m.actor && R::IsLiveByIndex(m.actor, m.idx)) {
        // Suppress the destroy observer's echo; the host would otherwise broadcast a destroy for a
        // display-only actor.
        coop::prop_echo_suppress::MarkIncomingDestroy(m.actor);
        E::DestroyActor(m.actor);
        UE_LOGI("hand_item: slot %u %s-hand mirror destroyed (%s)", static_cast<unsigned>(slot), HandName(hand), why);
    }
    m = Mirror{};
}

void NoteSpawnFailed(Mirror& m, const SlotHand& want, void* puppetActor) {
    m = Mirror{};
    m.cls = want.cls;
    m.name = want.name;
    m.puppetIdx = R::InternalIndexOf(puppetActor);
    m.puppetSerial = R::AllocateSlotSerial(m.puppetIdx);
    m.failed = true;
}

// The puppet's own off-hand component (mainPlayer_C.weapon_off), where better_holdItem_offhand attaches the item;
// null when it does not resolve.
void* OffHandComponent(void* puppetActor) {
    static R::InstanceOffset s_off(L"weapon_off");
    const int32_t off = s_off.Of(puppetActor);
    if (off < 0) return nullptr;
    void* comp = nullptr;
    std::memcpy(&comp, static_cast<const uint8_t*>(puppetActor) + off, sizeof(comp));
    return comp && R::IsLive(comp) ? comp : nullptr;
}

void SpawnMirror(Hand hand, uint8_t slot, void* puppetActor) {
    const SlotHand& want = HandOf(hand, slot);
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
        NoteSpawnFailed(MirrorOf(hand, slot), want, puppetActor);
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
        NoteSpawnFailed(MirrorOf(hand, slot), want, puppetActor);
        return;
    }
    // The item name is stamped before Init runs: generic prop meshes are name-driven, the game's
    // own better_holdItem_mainhand pattern.
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
        NoteSpawnFailed(MirrorOf(hand, slot), want, puppetActor);
        return;
    }
    E::SetActorSimulatePhysics(actor, false);
    E::SetActorEnableCollision(actor, false);
    // The off hand's item sits where the game puts it: snapped to the puppet's weapon_off. A puppet without one
    // leaves it at the puppet's location, still display-only.
    if (hand == Hand::Off) {
        void* const comp = OffHandComponent(puppetActor);
        if (!comp || !E::AttachActorToComponentSocket(actor, comp, nullptr))
            UE_LOGW("hand_item: slot %u off-hand mirror '%ls' not attached (weapon_off %s)",
                    static_cast<unsigned>(slot), want.cls.c_str(), comp ? "refused the attach" : "did not resolve");
    }
    Mirror& m = MirrorOf(hand, slot);
    m = Mirror{};
    m.actor = actor;
    m.idx = R::InternalIndexOf(actor);
    m.cls = want.cls;
    m.name = want.name;
    m.puppetIdx = R::InternalIndexOf(puppetActor);
    m.puppetSerial = R::AllocateSlotSerial(m.puppetIdx);
    UE_LOGI("hand_item: slot %u %s-hand mirror SPAWNED cls='%ls' name='%ls' (display-only, %s)",
            static_cast<unsigned>(slot), HandName(hand), want.cls.c_str(), want.name.c_str(),
            hand == Hand::Main ? "view-anchored" : "on weapon_off");
}

// One hand's mirror of `slot` kept to what the slot's state wants: destroyed when the hand is empty or the puppet is
// gone, spawned again when the item or the puppet changes, and the main hand's re-commanded to the view hold.
void TickOne(Hand hand, uint8_t slot, coop::RemotePlayer* pup, void* puppetActor) {
    const SlotHand& want = HandOf(hand, slot);
    Mirror& m = MirrorOf(hand, slot);
    const bool mirrorLive = m.actor && R::IsLiveByIndex(m.actor, m.idx);
    if (!want.has) {
        if (mirrorLive || m.actor) DestroyMirrorOf(hand, slot, "hand now empty");
        else if (m.failed) m = Mirror{};  // the failed item left the hand; the next one is tried
        return;
    }
    if (!puppetActor) {
        // The puppet not up yet (the join window) or torn down: keep the state, drop any
        // orphaned mirror, retry next tick.
        if (mirrorLive || m.actor) DestroyMirrorOf(hand, slot, "puppet gone");
        return;
    }
    const int32_t pupIdx = R::InternalIndexOf(puppetActor);
    const bool sameWant = ::_wcsicmp(m.cls.c_str(), want.cls.c_str()) == 0 && m.name == want.name &&
                          m.puppetIdx == pupIdx &&
                          R::SlotSerial(pupIdx) == m.puppetSerial;
    if (mirrorLive && sameWant) {
        // Steady state: the main hand's is re-commanded to the view hold; the off hand's rides weapon_off.
        if (hand == Hand::Main) coop::hand_item_pose::DriveMirror(m.actor, pup, want.relPos, want.relRot);
        return;
    }
    if (m.failed && sameWant) return;  // this item failed to spawn on this puppet (see Mirror)
    if (mirrorLive || m.actor) DestroyMirrorOf(hand, slot, "state/puppet changed");
    SpawnMirror(hand, slot, puppetActor);
    Mirror& nm = MirrorOf(hand, slot);
    if (nm.actor && hand == Hand::Main)
        coop::hand_item_pose::DriveMirror(nm.actor, pup, want.relPos, want.relRot);  // no first-frame pop at spawn pos
}

}  // namespace

const char* HandName(Hand hand) { return hand == Hand::Main ? "main" : "off"; }

// The prop name field's offset, resolved once; the field lives on the prop base class, and
// every hotbar item is a descendant.
int32_t PropNameOffset() {
    static int32_t s_off = -1;
    if (s_off >= 0) return s_off;
    if (void* propCls = R::FindClass(L"prop_C"))
        s_off = R::FindPropertyOffset(propCls, L"name");
    return s_off;
}

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

namespace mirror {

void Tick(Hand hand, uint8_t slot, coop::RemotePlayer* pup, void* puppetActor) { TickOne(hand, slot, pup, puppetActor); }

void Destroy(Hand hand, uint8_t slot, const char* why) { DestroyMirrorOf(hand, slot, why); }

void* Live(Hand hand, uint8_t slot) {
    const Mirror& m = MirrorOf(hand, slot);
    // IsLiveByIndex, not a bare pointer match: a dead mirror's recycled address belongs to a
    // different actor that must stay adoptable.
    return (m.actor && R::IsLiveByIndex(m.actor, m.idx)) ? m.actor : nullptr;
}

void ForgetSlot(uint8_t slot) {
    g_unresolvedWarned[slot].clear();
    g_unresolvedBySlot[slot] = 0;
}

}  // namespace mirror

}  // namespace coop::hand_item
