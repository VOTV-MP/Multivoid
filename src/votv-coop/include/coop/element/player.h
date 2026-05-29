// coop/element/player.h -- the Player Element subclass.
//
// Second subclass of `coop::element::Element` (after Npc). Each remote peer
// gets a Player Element whose ElementId is the unified runtime address used
// by future event_feed dispatch (`Registry::Get(id) -> Element*` resolves
// uniformly across Player/Npc/Prop without per-type switch cases).
//
// The Player Element does NOT own the engine actor or the `coop::RemotePlayer`
// puppet object -- those stay in `coop::players::Registry` and `harness.cpp`
// respectively. The Element carries:
//   - `m_peerSlot` (uint8_t): the legacy peer-slot id [0, kMaxPeers). Kept on
//     the Element for diagnostic logs and as the routing key into per-puppet
//     state maps (item_activate's pending-apply table, flashlight_click_sound's
//     last-state array, nameplate's nick slots). After A4 (v13 protocol bump
//     2026-05-29) wire packets carry `senderElementId` (uint32_t) instead of
//     `peerSessionId`; the receiver resolves senderElementId via
//     `coop::element::Registry::Get` -> `coop::element::Player*` and reads
//     `PeerSlot()` to feed those existing per-peerSlot maps. `m_id`
//     (Element::GetId()) is the wire-side unified ElementId.
//   - `m_puppet` (RemotePlayer*): the puppet for this peer, OR nullptr for
//     the local peer's own Player Element (the local doesn't have a puppet
//     -- it IS the local player).
//
// Lifecycle: owned by `coop::players::Registry` keyed by peerSlot via a
// fixed-size `std::unique_ptr<Player>[kMaxPeers]`. Constructed by
// `RegisterPuppet` (or by the local-init path for the local peer in
// `SetLocalPeerId`). Destroyed by `UnregisterPuppet` (or by replacement
// in `EnsurePlayerElement_` when the puppet pointer changes for the same
// slot). There is no global `Registry::Reset` -- per-subsystem drain only.
//
// Per the audit (`research/findings/votv-mta-cclientelement-audit-2026-05-28.md`
// section 4.3): MTA's `CClientPlayer` is just a refinement of `CClientPed`.
// Local and remote are the same class; the discriminator is `IsLocalPlayer()`.
// Our scope keeps the local-vs-puppet split inside `coop::players::Registry`
// (the local is tracked via `localCached_`; puppets via `puppetByPeer_`).
// `Player` here is the Element shadow of EITHER -- the m_puppet pointer
// distinguishes them.

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
    // A4 (2026-05-29): puppet pointer is mutable so the players::Registry
    // can bind a real puppet onto a MIRROR Player Element that was created
    // before RegisterPuppet ran. This avoids dropping the mirror (and its
    // wire-bound ElementId) when the AssignPeerSlot/Join handshake landed
    // before the puppet's first PoseSnapshot did. Only `Registry` is
    // expected to drive this transition.
    friend class coop::players::Registry;
    void SetPuppet_(coop::RemotePlayer* p) { m_puppet = p; }

    uint8_t              m_peerSlot;
    coop::RemotePlayer*  m_puppet;  // nullptr for the local peer
};

}  // namespace coop::element
