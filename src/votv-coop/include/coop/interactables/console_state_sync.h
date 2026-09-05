// coop/interactables/console_state_sync.h -- the screen-state mirror for the signal-catcher
// desk: four lanes on one module, since they share the spaceRenderer and desk substrate.
// Sky signals (SkySignalState): the coordinate-minigame targets were per-peer random (the
// spaceRenderer's spawnSignal re-arms itself and rolls everything locally), so the host is the
// only roller; a client clears its own roller timer (one reflected spawnSignal re-arms it on
// disconnect), keeps its widget lifetimes wire-driven, and reconciles its set to the host's
// snapshot. The catch consume replay lives in coop/interactables/signal_catch_sync; this
// module hands incoming snapshots through its recent-catch filter. Desk scalars (DeskState):
// adopt-only, the joiner seed from the host; live desk input rides DeskInput and the
// simulation outputs ride DeskSimPose. Desk log lines (DeskLogLine): a producer-side exact
// diff of the coord log text, complete lines only, the self-regenerating cooldown lines
// filtered; receivers append through writeToCoordLog_2 and advance their own baseline past
// the applied text. Dish aim (DishAimState): the committed coordinate locks and the direction
// toggle, sent by the desk-claim holder on change; receivers raw-write and repaint. Game
// thread throughout: Tick from the gameplay tick, the wire ingest from the event feed drain.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::console_state_sync {

// Store the session. Idempotent.
void Install(coop::net::Session* session);

// Per tick: the host polls the sky set at 1 Hz and broadcasts on change; the client enforces
// its roller suppression and sweeps rows the wire never expressed; every peer produces desk
// log lines at 1 Hz; the desk-claim holder sends the committed dish locks on change.
void Tick();

// Wire ingest. The catch message ingests in coop/interactables/signal_catch_sync.
void OnSkySignalState(const coop::net::SkySignalStatePayload& p, uint8_t senderSlot);
void OnDeskState(const coop::net::DeskStatePayload& p, uint8_t senderSlot);
void OnDishAim(const coop::net::DishAimStatePayload& p, uint8_t senderSlot);
void OnDeskLogLine(const coop::net::DeskLogLinePayload& p, uint8_t senderSlot);

// Host: send the current sky snapshot, the desk scalars as an adopt and the committed dish
// locks to a joining peer at world-ready.
void QueueConnectBroadcastForSlot(int peerSlot);

// Teardown: clear the mirrors; a client restores its native sky roller.
void OnDisconnect();

}  // namespace coop::console_state_sync
