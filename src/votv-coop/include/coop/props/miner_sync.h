// coop/props/miner_sync.h -- a GPU mining rig (prop_miner_C) is the host's: what it holds and the points it mines.
//
// A rig holds slots, GPUs and coolers, counted in its own fields (upg_slots, upg_gpu, upg_fans): a held scrap metal, GPU
// or cooler used on it (playerUsedOn) runs its `upgrade`, which counts one more and repaints it through `upd`, and the
// item goes. Its tick mines `accumulatedPoints` while it is powered and cools `overheat`; its action 4 claims the points
// (lib_C::addPoints) and empties them. Each ran on the one machine, so a rig a player filled stood empty for the others,
// and each copy mined its own points. Here a client refuses the rig's tick and its fire (`fireDamage`, which spawns
// the rig's debris and loose GPUs), and its use of an item and its claim are
// refused and asked of the host (MinerIntent), which judges the sender's reach and pace and runs its own rig's `upgrade`
// or claim, the rig's hints going to the asker (coop/requester_feedback). A granted use spends the item in the asker's
// hand, whose going the prop lanes carry. The host sends each rig's counts, points and heat when they change, the
// points at most once a second, and to a joiner at its ready edge (MinerState); a client writes them and repaints
// through the rig's own `upd`. Game thread.
#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct MinerPayload;
}  // namespace coop::net

namespace coop::miner_sync {

// Keep the session and register the watches on the rig's tick, use and actions. Idempotent.
void Install(coop::net::Session* session);

// HOST: send each rig's change, judge the queued asks. CLIENT: apply the host's states. Settles the watches. Per frame.
void Tick();

// HOST, at a joiner's world-ready edge: every rig's state, to that slot.
void SendJoinSnapshotForSlot(int slot);

// HOST: a client's ask, its length checked by the dispatcher.
void OnIntent(int senderSlot, const coop::net::MinerPayload& p);

// CLIENT: the host's state or answer, its length checked by the dispatcher.
void OnState(const coop::net::MinerPayload& p);

// Session end: the records are cleared.
void OnDisconnect();

}  // namespace coop::miner_sync
