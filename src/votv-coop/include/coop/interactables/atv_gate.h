// coop/interactables/atv_gate.h -- an ATV's acts at their own functions, observed at the script gate. A player's item on
// an ATV runs in the ATV's own Blueprint bodies, and what they change of the ATV is the host's (atv_host_sync.h):
//   - fuelUp (the gas can's pour, from gascanFuel), toolboxFix, playerUsedOn (a module fitted) and upgradeTake (a module
//     taken off) are observed before and after, refused only as below: on a CLIENT the verb runs on its copy, where the item is
//     spent, and what it did to the ATV is asked of the host (op refuel with the gain, repair once per hold, fit and
//     take-off with the module's value); on the HOST the same POSTs call the publish point, which sends nothing when the
//     call changed nothing, and a verb nested in another (playerUsedOn calls fuelUp and toolboxFix) publishes once, at the
//     outermost one's POST;
//   - the exceptions: fuelUp from anything but gascanFuel (the fuelbox's overlap with a can lying in the world) is refused on a
//     client, the host's copy being that can's author; and a client at its cap of open requests (4 on one ATV, 8 over all)
//     has the verb cancelled at its PRE, since nothing irreversible may happen on its copy unanswered;
//   - the honk key's press and release on the copy a player sits in set the horn's level and latch the syncer's next block
//     carries (atv_sync.h NoteHorn); our own replay of a horn is no press (atv_host_sync::Applying).
// A take-off the host refuses leaves the client with a module prop its own upgradeTake made: it is destroyed at the answer.
// Game thread.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::atv_gate {

// The ATV functions this module watches, one row each (the verbs' own bodies and the honk key's two events).
enum Row : int { kFuelUp, kToolbox, kUsedOn, kTake, kHonkPress, kHonkRelease, kRowCount };

// [dev] How often `row`'s watch has run this process, one relaxed bump a hit (the honk rows at their POST, the others at
// their PRE): the ATV drill's way to learn that the game answered an act. Any thread.
uint32_t Seen(Row row);

// [dev] A request the drill makes in a player's place, a forged one: registered in the open-request gate and sent as the
// real path does (op = atv_host_op). The id, or 0 when nothing was sent. Game thread.
uint32_t SendForDrill(const std::wstring& key, uint8_t op, float amount, uint8_t module);

// Register the watches, once for the process, from the load-window installs (subsystems::InstallLoadWatchers). A watch
// that did not register is said in one ERROR line. Idempotent. Any thread.
void Install(coop::net::Session* session);

// Once the registered watches have settled, a row that settled dead is said in one WARN. Game thread, from atv_host_sync's
// tick.
void SettleWatches();

// CLIENT: the host's answer to request `reqId` (atv_host_reason in `reason`): a refused take-off destroys the module prop
// its own call handed this player. Game thread.
void OnAnswer(uint32_t reqId, uint8_t reason);

// Session end: the take-offs waiting for an answer are forgotten.
void OnDisconnect();

}  // namespace coop::atv_gate
