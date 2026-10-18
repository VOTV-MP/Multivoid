// coop/element/player.h -- the Player Element subclass.
//
// Each remote peer gets a Player Element whose ElementId is the unified runtime address event_feed
// dispatch resolves against: Registry::Get(id) -> Element* answers uniformly across Player, Npc and
// Prop with no per-type switch. The local peer's Element is the one in the slot
// players::Registry::LocalPeerId() names; m_puppet is the puppet bound now, or null. The Element
// owns neither the engine actor nor the RemotePlayer puppet (players::Registry and puppet_drive's
// g_puppets hold those). It carries m_peerSlot in [0, kMaxPeers): wire packets carry
// senderElementId, the receiver resolves it to a Player and reads PeerSlot() to key the per-slot
// maps. players::Registry owns the Elements as unique_ptr<Player>[kMaxPeers], built by
// RegisterPuppet, by EstablishMirrorForSlot or by SetLocalPeerId. A puppet is bound onto an
// existing Element in place (a mirror, or one ClearPuppet emptied); ClearPuppet nulls it without
// dropping the Element. UnregisterPuppet, a changed local id, one real puppet replacing another,
// or EstablishMirrorForSlot replacing it with the wire-bound mirror destroys it. No global reset
// exists.

#pragma once

#include "coop/element/element.h"

#include <cstdint>

namespace coop { class RemotePlayer; }
namespace coop::players { class Registry; }

namespace coop::element {

class Player : public Element {
public:
    Player(uint8_t peerSlot, coop::RemotePlayer* puppet)
        : Element(ElementType::Player),
          m_peerSlot(peerSlot),
          m_puppet(puppet) {}

    uint8_t              PeerSlot() const { return m_peerSlot; }
    coop::RemotePlayer*  Puppet() const   { return m_puppet; }
    // True when no puppet is bound now. That holds for the local Element but also for a mirror
    // before its puppet and for a remote Element after ClearPuppet, so it does not say whose Element
    // this is: the local peer is the slot Registry::LocalPeerId() names.
    bool                 IsLocal() const  { return m_puppet == nullptr; }

private:
    // The puppet pointer is mutable so players::Registry can bind a real puppet onto a MIRROR
    // Player Element created before RegisterPuppet ran, or onto an Element ClearPuppet emptied.
    // Without it the mirror, and the wire-bound ElementId it holds, would be dropped whenever the
    // AssignPeerSlot/Join handshake landed ahead of the puppet's first PoseSnapshot. Only Registry
    // calls it: the mirror bind and the cleared-Element rebind in EnsurePlayerElement_, and
    // ClearPuppet's clear.
    friend class coop::players::Registry;
    void SetPuppet_(coop::RemotePlayer* p) { m_puppet = p; }

    uint8_t              m_peerSlot;
    coop::RemotePlayer*  m_puppet;  // null: no puppet bound now
};

}  // namespace coop::element
