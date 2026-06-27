// coop/element/element.h -- the unified runtime entity addressing layer base.
//
// Adapted from `reference/mtasa-blue/Client/mods/deathmatch/logic/CClientEntity.h`
// (MIT). The "minimum useful" subset: id + type tag + name + class-name tag +
// engine actor pointer + being-deleted flag + sync-time-context generation byte.
//
// EXPLICITLY OMITTED from MTA's CClientEntity (RULE 1 / RULE 3, per
// `research/findings/votv-mta-cclientelement-audit-2026-05-28.md` section 6):
//   - CCustomData (Lua key/value bag) -- no Lua at runtime.
//   - CMapEventManager Lua dispatch -- no Lua.
//   - Dimension / Interior layering -- no parallel-arena gameplay in VOTV.
//   - Attachment chain (m_AttachedEntities) -- our PropPose drives transforms
//     directly; a general attachment abstraction is over-scope until 2+ uses.
//   - OriginSourceUsers / Collision bookkeeping -- UE owns these.
//   - Class-kind enum + DECLARE_BASE_CLASS macro -- we use dynamic_cast on the
//     ~handful of downcast sites; not worth the per-subclass registration.
//   - Element-Group, spatial database -- no resource scripts; UE owns spatial.
//   - Parent/children tree -- deferred to v2 (NPCs are root elements; players
//     and props don't have intrinsic hierarchy in our scope yet).
//
// Subclasses (`Player`, `Prop`, `Npc`) own the lifecycle of their engine actor;
// Element itself does NOT delete `m_actor` in its destructor (engine owns it,
// per CLAUDE.md Principle 3 -- parallel class hierarchy).
//
// Lifetime: Element instances are owned by their respective subsystem
// (e.g. `npc_sync.cpp` owns the `Npc` it creates), and register themselves with
// `coop::element::Registry` on construction / unregister on destruction.
//
// Thread safety: Element fields are NOT internally synchronized. The Registry
// owns the inter-thread visibility (alloc / free are mutex-guarded). Per-
// Element field accesses (SetActor / SetName / SetTypeName / GetActor / etc.)
// happen during construction and from ProcessEvent observer callbacks, which
// can dispatch from parallel-anim task-graph workers per
// `ue_wrap/game_thread.h:118-120`. The current PoC convention is:
//   - Subsystems that own an Element write its fields once at the publishing
//     moment (e.g. NpcSpawn_POST writes m_actor right after the engine
//     spawned the actor) and never thereafter, so the race window is the
//     publish itself.
//   - Subsystems that READ another subsystem's Element fields (cross-feature)
//     should treat them as best-effort and acquire the relevant feature's
//     mutex if synchronization is required.
// If a Player or Prop subclass later needs full mutable concurrent access,
// promote the relevant field to atomic or add a per-Element mutex at that
// point. The PoC's NPC use sites are correct under the publish-once pattern.

#pragma once

#include <cstdint>
#include <string>

namespace coop::element {

// 32-bit at rest, 16-bit on wire (kMaxElements = 65536). Per the audit
// section 2.1: MTA uses 17-bit (131072) but our scope is smaller; 65536
// is overkill for a 4-peer coop session with ~2000 props + ~100 NPCs.
using ElementId = uint32_t;

inline constexpr ElementId kInvalidId   = 0xFFFFFFFFu;
inline constexpr uint32_t  kMaxElements = 65536;

// Two-range partition mirrors MTA's server/client split:
//   host range: [0, kHostRangeSize) -- authoritative side (host in our model).
//   peer range: [kHostRangeSize, kMaxElements) -- client-local elements.
// 50/50 split matches MTA's MAX_SERVER_ELEMENTS == MAX_CLIENT_ELEMENTS.
inline constexpr uint32_t kHostRangeSize = kMaxElements / 2;  // 32768

enum class ElementType : uint8_t {
    Unknown = 0,
    Player  = 1,
    Prop    = 2,
    Npc     = 3,
    // v78 (kerfur redesign): a logical kerfur that flips between an AI NPC
    // (kerfurOmega_C) and a grabbable prop (prop_kerfurOmega_C). A KerfurEntity
    // is a HOST-ONLY authority record reserving one stable host-range KerfurId
    // spanning BOTH forms; the kerfur's RENDERED form is a normal Npc/Prop mirror
    // at its own per-form eid (see coop/kerfur_entity.h + the redesign doc).
    Kerfur  = 4,
    // v80 (B3b): a NON-Character event WorldActor (gray saucer, Rozital mothership, ariral
    // ship, sky UFO, jellyfish, firetank, ...) -- a plain AActor/APawn the host streams a
    // transform-only mirror of. Peer of Npc under Element (the intended sibling design, MTA
    // CClientStreamElement shape); see coop/element/world_actor.h + coop/world_actor_sync.
    WorldActor = 5,
    // Door / Light / Vehicle / etc. get IDs as those features land.
};

class Element {
public:
    explicit Element(ElementType type);
    virtual ~Element();

    // Non-copyable, non-movable -- Element identity is the ElementId, which
    // is registered with Registry on construction.
    Element(const Element&)            = delete;
    Element& operator=(const Element&) = delete;
    Element(Element&&)                 = delete;
    Element& operator=(Element&&)      = delete;

    // ---- Identity -------------------------------------------------------

    ElementId   GetId() const   { return m_id; }
    ElementType GetType() const { return m_type; }

    // Save-stable identity. For props this is `Aprop_C.Key` (the cooked-asset
    // BP GUID-like string that survives game-save serialization). For runtime-
    // only elements (Player, Npc) this stays empty -- the ElementId itself
    // is the only identity.
    const std::string& GetName() const { return m_name; }
    void SetName(std::string n)        { m_name = std::move(n); }

    // Class-name tag. For Aprop_C derivatives this is "Aprop_chipPile_C",
    // "Aprop_food_C", etc. For NPCs: "npc_zombie_C" etc. For Player: the
    // mainPlayer_C class name. Replaces the per-payload WireClassName field
    // when the Element is the source of truth.
    const std::string& GetTypeName() const { return m_typeName; }
    void SetTypeName(std::string n)        { m_typeName = std::move(n); }

    // ---- Engine binding -------------------------------------------------

    // The UE-side AActor* (or nullptr if the element does not (yet) have an
    // engine representation -- e.g. a host-allocated NPC element whose actor
    // pointer has not been captured yet because the spawn POST observer hasn't
    // run, or a client-local UI marker with no engine actor at all).
    //
    // Element does NOT own the actor's lifetime. The owning subsystem holds
    // the actor pointer; the engine destroys actors via K2_DestroyActor; the
    // subsystem then destroys the Element. (Principle 3 -- parallel class
    // hierarchy: our state lives alongside the engine actor, not inside it.)
    void* GetActor() const  { return m_actor; }

    // `internalIdx` MUST be the actor's GUObjectArray InternalIndex captured
    // (via ue_wrap::reflection::InternalIndexOf) WHILE the actor is known live
    // -- i.e. at this publishing moment, right after spawn / when IsLive just
    // passed. It is cached so a consumer that holds the actor pointer across
    // ticks (e.g. the connect-edge prop snapshot) can later validate it with
    // reflection::IsLiveByIndex WITHOUT dereferencing the (possibly GC-purged)
    // actor memory. The index is part of SetActor (not a separate setter) so it
    // can never be silently omitted -- a stale/-1 index would make the actor
    // read as not-live and drop it from snapshots. Pass -1 only for an element
    // with no engine actor.
    // Non-inline (defined in element.cpp): besides writing m_actor/m_internalIdx
    // it maintains the Registry's unified actor->eid reverse index so
    // Registry::EidForActor(a) is always consistent with the live binding
    // (sync-refactor 2026-06-27: subsumes prop_element_tracker's
    // g_actorToPropElementId locals-only reverse AND g_boundMirrorNatives, which
    // existed only because mirrors weren't in that reverse). Covers EVERY element
    // type (Prop/Npc/WorldActor) uniformly -- one reverse for the whole registry,
    // the MTA CClientEntity shape (game-ptr owned by the entity; reverse implicit).
    void  SetActor(void* a, int32_t internalIdx);

    // Cached GUObjectArray InternalIndex of m_actor (see SetActor); -1 if none.
    // Feed to reflection::IsLiveByIndex to validate m_actor after a possible GC.
    int32_t GetInternalIdx() const { return m_internalIdx; }

    // ---- Lifecycle / sync state -----------------------------------------

    // Mirrors MTA's m_bBeingDeleted. Set by the owning subsystem when the
    // element is about to be destroyed, so concurrent observers (Init POST,
    // K2_DestroyActor PRE, etc.) can skip a doomed instance without racing
    // the destructor. Checked but not enforced -- callers must respect it.
    bool IsBeingDeleted() const   { return m_beingDeleted; }
    void SetBeingDeleted(bool b)  { m_beingDeleted = b; }

    // (v14 m_syncContext / GetSyncContext / BumpSyncContext / SetSyncContext
    // were the 8-bit per-element generation byte adopted from MTA's
    // m_ucSyncTimeContext. v16 PR-FOUNDATION-1b retired them: per-peer
    // stale-generation defense now lives in the packet header's
    // senderEpoch (32-bit, minted via std::random_device at Session::
    // Start), latched per peerSlot at the receiver in Session::
    // HandleMessage. Element no longer carries any wire-context state.)

    // True if this Element was created as a CLIENT-SIDE MIRROR of a host-
    // allocated ElementId (registered via Registry::RegisterMirror). The
    // dtor calls UnregisterMirror instead of FreeId so the host range id is
    // NOT pushed onto the client's free stack (it was never allocated from
    // it -- mirrors borrow ids from the host's allocation space).
    bool IsMirror() const { return m_mirror; }

    // True iff this Element is a SAVE-LOADED NATIVE pile/kerfur the client bound
    // as the host-range mirror via save_identity_bind::BindLocalNativeToHostEid.
    // (sync-refactor 2026-06-27, the D1 enabler): this REPLACES
    // prop_element_tracker's g_boundMirrorNatives actor-keyed SET, which could
    // read stale relative to the binding (15:01:49 morphBoundNative=false root).
    // As an Element field it is atomic with the binding -- the answer to "is this
    // a bound save-native" lives with the identity, not in a satellite set that
    // can desync. Set by the bind, cleared when the Element is retired.
    bool IsSaveNative() const   { return m_saveNative; }
    void SetSaveNative(bool b)  { m_saveNative = b; }

    // For a wire MIRROR (IsMirror()==true): the peer slot of the ORIGINATING
    // peer -- the logical origin the host relay stamps into the reliable
    // header's senderPeerSlot (session.cpp:512; preserved through relay so a
    // prop from peer A carries slot A on peer B, not the relaying host's 0).
    // Lets a per-slot disconnect drain exactly the departing peer's mirrors
    // (D1-7) instead of leaking them until full teardown. -1 = no owner:
    // AllocAndInstall'd locals (m_mirror=false) and any mirror left untagged.
    // (MTA precedent: CClientGame removes a quitting player's owned elements on
    // Event_OnPlayerQuit; our per-type MirrorManager::DrainMirrorsForSlot is the
    // same per-player eviction, generalized.) Set on the game thread at Install
    // and read on the game thread at the disconnect drain -- same publish-once /
    // GT-serialized discipline as m_mirror.
    int8_t GetOwnerSlot() const   { return m_ownerSlot; }
    void   SetOwnerSlot(int8_t s) { m_ownerSlot = s; }

private:
    // m_id + m_mirror are set by Registry when the element is registered.
    // They are otherwise immutable. Registry uses a friend helper.
    friend class Registry;
    void SetId_(ElementId id)   { m_id = id; }
    void SetMirror_(bool m)     { m_mirror = m; }

    ElementId   m_id           = kInvalidId;
    ElementType m_type         = ElementType::Unknown;
    std::string m_name;
    std::string m_typeName;
    void*       m_actor        = nullptr;
    int32_t     m_internalIdx  = -1;     // cached GUObjectArray slot of m_actor
    bool        m_beingDeleted = false;
    bool        m_mirror       = false;
    bool        m_saveNative   = false;  // bound save-loaded native (D1 enabler); see IsSaveNative
    int8_t      m_ownerSlot    = -1;     // originating peer slot for mirrors (D1-7); -1 = none
};

}  // namespace coop::element
