// coop/element/player.h -- the Player Element subclass.
//
// Each remote peer gets a Player Element whose ElementId is the unified runtime address event_feed
// dispatch resolves against: Registry::Get(id) -> Element* answers uniformly across Player, Npc and
// Prop with no per-type switch. Local and remote are the same class here, discriminated by
// m_puppet, which is what IsLocal() reads. The Element owns neither the engine actor nor the
// coop::RemotePlayer puppet -- those live in coop::players::Registry and in puppet_drive's
// g_puppets array. It carries m_peerSlot, the slot id in [0, kMaxPeers), because wire packets carry
// senderElementId and the receiver resolves it to a Player and reads PeerSlot() to key the per-slot
// maps: item_activate's pending applies, flashlight_click_sound's last-state array, the nameplate's
// nick slots. players::Registry owns them as a fixed unique_ptr<Player>[kMaxPeers], built by
// RegisterPuppet, by EstablishMirrorForSlot when the wire names a slot before its puppet exists, or
// by SetLocalPeerId's local-init path, and destroyed by UnregisterPuppet or by replacement when a
// slot's puppet pointer changes. No global reset exists.

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
    bool                 IsLocal() const  { return m_puppet == nullptr; }

private:
    // The puppet pointer is mutable so players::Registry can bind a real puppet onto a MIRROR
    // Player Element created before RegisterPuppet ran. Without it the mirror, and the wire-bound
    // ElementId it holds, would be dropped whenever the AssignPeerSlot/Join handshake landed ahead
    // of the puppet's first PoseSnapshot. Only Registry is expected to drive that transition.
    friend class coop::players::Registry;
    void SetPuppet_(coop::RemotePlayer* p) { m_puppet = p; }

    uint8_t              m_peerSlot;
    coop::RemotePlayer*  m_puppet;  // nullptr for the local peer
};

}  // namespace coop::element
