// coop/props/miner_sync.h -- a GPU mining rig (prop_miner_C) is the host's: what it holds and the points it mines.
//
// A rig counts slots, GPUs and coolers in its own fields; a held scrap metal, GPU or cooler used on it (playerUsedOn)
// adds one and the item goes, its tick mines points while it is powered, and its action 4 claims them into the balance.
// Each ran on one machine only. Here a client refuses the rig's tick and fire, runs its use on its own copy (which spends
// the item from its hand) and asks the host (MinerIntent op 2), and asks for its claim without running it (op 3). The host
// judges each ask as it arrives, in the order of its answer's reasons (kMinerReason*): the sender's budget, hand (the
// hand lane's last announce and the one use a hold has), world, body and reach; it runs its own rig's `upgrade` or claim,
// answers (MinerState op 1; a budget drop is not answered), gives a paid item back beside the sender on a refusal (within the
// sender's refund budget), and sends a refused asker the host's state of the rig. The host says each rig's counts, powered flag, points and heat (op 0) when one changes, read at the
// end of the rig's own tick, and to a joiner at its ready edge; a client writes them and repaints through `upd` and
// `plugged`, or keeps the latest per rig until its copy is here. The store sells the rig, so the record lane carries its
// counts at a drop: a client's record lands on the host only when its own drop awaits it and every number is in bounds.
// Game thread.
#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct MinerPayload;
}  // namespace coop::net

namespace coop::miner_sync {

// Keep the session; register the watches on the rig's tick, use, actions and fire, and the record gate. Idempotent.
void Install(coop::net::Session* session);

// HOST: the states a refusal owes. Settles the watches and resolves the class name. Per frame.
void Tick();

// CLIENT (coop/props/remote_prop): a prop copy was bound to element id `eid`; the state the host sent for it before the copy
// was here is applied to it.
void OnBound(uint32_t eid, void* actor);

// HOST, at a joiner's world-ready edge: every rig's state, to that slot.
void SendJoinSnapshotForSlot(int slot);

// HOST: a client's ask (op 2 or 3), its fields checked by the dispatcher.
void OnIntent(int senderSlot, const coop::net::MinerPayload& p);

// CLIENT: the host's state (op 0) or answer (op 1), its fields checked by the dispatcher.
void OnState(const coop::net::MinerPayload& p);

// Session end: the tables, the waiting states and the tallies are cleared.
void OnDisconnect();

// ---- for the drills ----

// What the host last answered `slot`, or what this client last heard: `seq` is the number of that answer (0: none yet),
// `op` the ask's op (2 a use, 3 a claim), `item` its item and `reason` kMinerReason*.
struct Answer {
    uint32_t seq = 0;
    uint32_t eid = 0;
    uint8_t op = 0;
    uint8_t item = 0;
    uint8_t reason = 0;
};
Answer HostLastAnswer(uint8_t slot);
Answer ClientLastAnswer();

// HOST: the uses granted, the claims asked and the claims that ran and paid, the refunds spawned, and the uses of `slot`
// judged for any reason (a budget drop included, which is not answered), since the session began.
uint32_t HostUsesGranted();
uint32_t HostClaimAsks();
uint32_t HostClaimsRan();
uint32_t HostRefunds();
uint32_t HostUsesJudged(uint8_t slot);

// HOST: whether a refused ask of `slot` still owes it the state of a rig (sent at the next tick).
bool HostOwesState(uint8_t slot);

// CLIENT: the state last applied to the copy of rig `eid`, and the number of that apply (0: none yet; it rises with every
// apply of any rig). The line "miner_sync: CLIENT applied state eid <n>" says each apply that changed a count, the
// powered flag, or emptied the points.
struct Applied {
    uint32_t seq = 0;
    int32_t gpus = 0, slots = 0, fans = 0, points = 0;
    float overheat = 0.f;
    bool powered = false;
};
Applied LastApplied(uint32_t eid);

// HOST: the records of a client's rig refused by the gate and the records accepted whose counts the host's rig then showed
// (the lines "miner_sync: HOST record refused <key>" and "record applied <key>"); the rigs a join snapshot skipped for
// having no element id.
uint32_t RecordsRefused();
uint32_t RecordsApplied();
uint32_t SnapshotEidless();

}  // namespace coop::miner_sync
