// coop/element/element.h -- the base of the runtime entity addressing layer, adapted from MTA's
// CClientEntity (MIT) down to the useful subset: id, type tag, name, class-name tag, engine
// actor pointer, being-deleted flag. Omitted from MTA's shape: the Lua data bag and event
// dispatch, dimensions and interiors, the attachment chain (the pose streams drive transforms
// directly), collision bookkeeping (the engine owns it), the class-kind enum (dynamic_cast at
// the handful of downcasts), the element groups and spatial database, and the parent tree.
// Subclasses own the lifecycle of their engine actor; Element never deletes the actor, which
// the engine owns. Instances are owned by their subsystem and register with the element
// registry on construction. Fields are not internally synchronised: the registry owns
// inter-thread visibility for alloc and free, and a subsystem writes an Element's fields once
// at the publishing moment (the spawn observer writing the actor right after the engine
// spawned it), which can run on a parallel-anim worker; a cross-feature reader treats them as
// best-effort.

#pragma once

#include <cstdint>
#include <string>

namespace coop::element {

// 32-bit at rest, 16-bit on the wire; 65536 is ample for a four-peer session with thousands of
// props.
using ElementId = uint32_t;

inline constexpr ElementId kInvalidId   = 0xFFFFFFFFu;
inline constexpr uint32_t  kMaxElements = 65536;

// The two-range partition mirrors MTA's server and client split: the host range for the
// authoritative side, the peer range for client-local elements, split evenly as MTA does.
inline constexpr uint32_t kHostRangeSize = kMaxElements / 2;  // 32768

enum class ElementType : uint8_t {
    Unknown = 0,
    Player  = 1,
    Prop    = 2,
    Npc     = 3,
    // A logical kerfur that flips between an AI NPC and a grabbable prop: a host-only authority
    // record reserving one stable host-range id spanning both forms; the rendered form is a
    // normal Npc or Prop mirror at its own eid (see coop/creatures/kerfur_entity.h).
    Kerfur  = 4,
    // A non-character event actor (a saucer, the mothership, an ariral ship, the sky UFO, the
    // jellyfish, the firetank): a plain actor the host streams a transform-only mirror of, a
    // sibling of Npc under Element (see coop/element/world_actor.h).
    WorldActor = 5,
};

class Element {
public:
    explicit Element(ElementType type);
    virtual ~Element();

    // Non-copyable and non-movable: an Element's identity is its id, registered on construction.
    Element(const Element&)            = delete;
    Element& operator=(const Element&) = delete;
    Element(Element&&)                 = delete;
    Element& operator=(Element&&)      = delete;

    // Identity.

    ElementId   GetId() const   { return m_id; }
    ElementType GetType() const { return m_type; }

    // The save-stable identity: for a prop the game's key string, which survives save
    // serialisation; empty for runtime-only elements, whose id is the only identity.
    const std::string& GetName() const { return m_name; }
    void SetName(std::string n)        { m_name = std::move(n); }

    // The class-name tag (a prop's, an NPC's or the player's class name), the source of truth for
    // the wire class name.
    const std::string& GetTypeName() const { return m_typeName; }
    void SetTypeName(std::string n)        { m_typeName = std::move(n); }

    // The engine binding.

    // The engine actor, or null while the element has no engine representation yet (a
    // host-allocated NPC whose spawn observer has not run) or at all. Element does not own the
    // actor's lifetime: the subsystem holds the pointer, the engine destroys actors, and the
    // subsystem then destroys the Element.
    void* GetActor() const  { return m_actor; }

    // `internalIdx` must be the actor's object-array index captured while the actor is known
    // live, at this publishing moment, so a consumer holding the pointer across ticks can
    // validate it with IsLiveByIndex without dereferencing possibly freed memory. Part of
    // SetActor rather than a separate setter, so it can never be silently omitted (a stale index
    // would drop the actor from snapshots); -1 only for an element with no actor. Non-inline:
    // besides the two fields it maintains the registry's actor-to-eid reverse, so EidForActor is
    // always consistent with the live binding, for every element type.
    void  SetActor(void* a, int32_t internalIdx);

    // The cached object-array index of the actor, or -1; feed it to IsLiveByIndex after a possible
    // GC.
    int32_t GetInternalIdx() const { return m_internalIdx; }

    // The actor pointer, slot-validated: the actor if IsLiveByIndex passes, else null. This, never
    // bare IsLive on the actor, is how a consumer probes an element's actor across ticks: bare
    // IsLive dereferences the possibly freed actor, and a co-resident crash reporter surfaces
    // that first-chance fault as a crash. Non-inline, since this header cannot include
    // reflection.
    void* LiveActor() const;

    // Lifecycle and sync state.

    // MTA's being-deleted flag: set by the owning subsystem when the element is about to be
    // destroyed, so concurrent observers skip a doomed instance without racing the destructor.
    // Checked, not enforced.
    bool IsBeingDeleted() const   { return m_beingDeleted; }
    void SetBeingDeleted(bool b)  { m_beingDeleted = b; }

    // True if this Element is a client-side mirror of a host-allocated id: the destructor
    // unregisters the mirror instead of freeing the id, since a mirror borrows its id from the
    // host's allocation space.
    bool IsMirror() const { return m_mirror; }

    // True iff this Element is a save-loaded native pile or kerfur the client bound as the
    // host-range mirror through the identity bind. A field on the Element, atomic with the
    // binding, rather than a satellite set that can desync; set by the bind, cleared when the
    // Element is retired.
    bool IsSaveNative() const   { return m_saveNative; }
    void SetSaveNative(bool b)  { m_saveNative = b; }

    // For a mirror, the originating peer's slot: the logical origin the host relay stamps into the
    // reliable header and preserves through the relay (a prop from one peer carries that peer's
    // slot on another, not the relaying host's 0), so a per-slot disconnect drains exactly the
    // departing peer's mirrors instead of leaking them until teardown; -1 means no owner (a
    // locally allocated element, or an untagged mirror). MTA removes a quitting player's owned
    // elements the same way. Set on the game thread at install and read there at the drain.
    int8_t GetOwnerSlot() const   { return m_ownerSlot; }
    void   SetOwnerSlot(int8_t s) { m_ownerSlot = s; }

private:
    // The id and the mirror flag are set by the registry at registration and are otherwise
    // immutable.
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
    bool        m_saveNative   = false;  // a bound save-loaded native; see IsSaveNative
    int8_t      m_ownerSlot    = -1;     // the originating peer slot for a mirror; -1 = none
};

}  // namespace coop::element
