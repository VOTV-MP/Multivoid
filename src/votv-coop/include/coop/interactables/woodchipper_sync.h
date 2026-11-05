// coop/interactables/woodchipper_sync.h -- a woodchipper's fuel and its on/off are the host's, and every peer has them.
//
// A woodchipper (woodchipper_C, a keyed prop_C) keeps `fuel` (0..100) and `active`. A gas can fills it through its
// gascanFuel, the button flips `active` through its actionOptionIndex, and its own tick burns fuel while it runs and
// switches it off at 0. All of it ran on the copy it happened on, so a client's pour or press never reached the host,
// whose copy is the one the save keeps. So:
//   - HOST: its copy is the chipper. A poll sends each chipper's fuel and on/off when either changes, every second while
//     it runs, and every chipper again to a joiner (WoodchipperState). A client's pour or press (WoodchipperIntent) is
//     checked -- the sender's world, the chipper, its reach -- and done on the host's copy: the fuel added, or the
//     button's own result written, then the chipper's checkActive, as the game's bodies end.
//   - CLIENT: its own pour and press run on its copy as before, so the can, the sound and the button answer at once, and
//     each is sent to the host: the press from the button's own body (the actionOptionIndex watch), the pour as a rise in
//     the copy's fuel that its own burning cannot make (the gas can's gascanFuel has no row to watch). The host's state
//     is then written on the copy, which keeps running its own tick between the host's sends.
// Game thread.

#pragma once

namespace coop::net {
class Session;
struct WoodchipperPayload;
}  // namespace coop::net

namespace coop::woodchipper_sync {

// Store the session, register the button watch and the scan-hub consumer. Idempotent; called every load tick.
void Install(coop::net::Session* session);

// The poll and the inboxes, on either role. Game thread.
void Tick();

// A client's receiver: the host's chipper.
void OnState(const coop::net::WoodchipperPayload& p);

// The host's receiver: a client's pour or press.
void OnIntent(int senderSlot, const coop::net::WoodchipperPayload& p);

// Every chipper again for a joiner.
void QueueConnectBroadcastForSlot(int slot);

// The session ended: everything is forgotten.
void OnDisconnect();

}  // namespace coop::woodchipper_sync
