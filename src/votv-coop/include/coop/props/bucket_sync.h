// coop/props/bucket_sync.h -- a world bucket's water, a concrete bucket's concrete and a cement bag's units are the host's, on
// every peer.
//
// A water bucket (prop_bucket_C, the base's mop bucket under it) holds `height` of water with `soap` and a brewing `alcoholP`,
// changed by its own tick and by verbs. A cement bag on one with water makes it a prop_concreteBucket_C (lib.replaceProp) whose
// `units` a trowel takes one at a time and whose `dryTimer` runs to `dry`. On BucketState:
//   HOST   -- sends a row for an element when a field of it changed past a row's worth, read after its own tick (a water or
//             concrete bucket) or its transfer (a bag), and when a verb a client asked of it was applied (MarkDirty); one row per
//             element per 100 ms at most, carrying its latest values and a per-element seq. A joiner is sent every element at
//             its world-ready, each row numbered with the element's next seq like any other. A refused ask or a failed apply sends the asker alone the host's row (SendRowTo).
//   CLIENT -- puts each row on its copy; a row for a copy not bound yet waits and applies at the bind (OnBound), unless a newer
//             one was applied, and ends with the element. A water bucket copy's own tick runs (its visuals) with the host's last
//             row written into it at the tick's entry (coop/props/bucket_pin); a concrete bucket copy's tick does not run. A
//             copy's verbs are asked of the host as prop_act families 6..11 (coop/props/prop_act_bucket*); a held item and a
//             dropped one reach the host in the drop's record, which a gate judges (coop/props/bucket_record_gate).

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

// Both roles: an element's state ends at the engine's announcement of its end (OnElementEnded, the death seam), never on a
// clock. MTA divergences: its client eases a vehicle to the server's state (CClientVehicle.cpp:3716) where a bucket copy's water
// is written whole at each tick, since the game's own `interpFlow` draws the easing; its element data goes out once per change
// (CGame.cpp:2715-2798) and so do these rows, with no resync. Game thread.
namespace coop::bucket_sync {

// Cache the session and register the class gate on a client's drop record. Idempotent.
void Install(coop::net::Session* session);

// Per gameplay tick: the watches' names resolve until they are live and the death seam is subscribed; the host sends the rows
// marked changed.
void Tick();

// HOST: slot `slot`'s world is up (the connect replay): every element the host holds, to it alone.
void OnPeerWorldReady(int slot);

// CLIENT (coop/props/remote_prop): a prop copy was bound to element id `eid`: the row that waited for it is applied.
void OnBound(uint32_t eid, void* actor);

// Both roles (coop/props/remote_prop_destroy, the death seam): element `eid` ended; everything the lane keeps of it goes, a
// row still waiting for its bind included.
void OnElementEnded(uint32_t eid);

// CLIENT: a BucketState message off the wire. It is checked here (the sender, the length, the row count, the padding, every
// type and float) and each row is applied or kept for its bind; a refusal is counted and said once per sender and cause.
void OnStateWire(const uint8_t* bytes, size_t len, int senderSlot);

// Session end.
void OnDisconnect();

// HOST: a verb changed `actor` (a water bucket, a concrete bucket or a bag): its row goes to every peer at the next flush,
// whatever the compare against the last row sent would say. Returns the seq the row will carry, or 0 when the actor has no
// element id yet (its own tick tells it) or is not the host's to send.
uint32_t MarkDirty(void* actor);

// HOST: `actor`'s row now, to slot `slot` alone, so a copy a refused verb or record left ahead of the host is put back. False
// when the actor has no element id or the send was refused. A row refused for want of an id is owed to the slot by the record gate,
// and goes at the mint of the id; an element no row was numbered for takes its first number here.
bool SendRowTo(int slot, void* actor);

// What the lane counts this session; the drills read them, nothing branches on them.
enum class Count : uint8_t {
    RowsSent, JoinRows, RowsApplied, RowsRefused, PinWrites, PlainTicksRefused, ConcreteTicksRefused, ConcreteTicksRan,
    DeleteAlcRefused, RecordsNotAwaited, RecordsOutOfRange, FamilyApplies, FamilyApplyFailures, WireRefused, kCount
};
void Note(Count c);
uint64_t CountOf(Count c);

namespace dev {   // called only from src/coop/dev/ and the red sites; compiled always, inert unless called
// The red arms of the bucket drill (config row bucket_red), each withholding one effect, never a refusal. Read once.
//   Apply     a family's apply on the host writes nothing (prop_act_bucket*)
//   Pin       a client copy's water is not put back at its tick (bucket_pin)
//   Tick      a client's plain bucket copy has its own tick refused (bucket_pin)
//   Ctick     a client's concrete bucket copy runs its own tick (bucket_pin)
//   Gate      a client's drop record for a bucket is taken without the gate (bucket_record_gate)
//   Snapshot  the host sends a joiner no bucket row, though its rows take their seq (bucket_sync)
//   Exact     the host sends rows for the plain bucket class alone (bucket_sync)
//   Alc       a client copy's deleteAlc is not refused (bucket_pin)
//   Judge     a family's own check on the host is skipped (prop_act_bucket*)
enum class Red : uint8_t { Off, Apply, Pin, Tick, Ctick, Gate, Snapshot, Exact, Alc, Judge };
Red RedOf();

// The seq the host's last MarkDirty of the element's row will carry (0 for none), the seq of the last row applied to the client's
// copy of the element (0 for none), the seq of the first row applied to it, and the seq the host's last join send numbered the
// element's row with (0 for none; taken under the red Snapshot too, though no row went).
uint32_t CommitSeq(uint32_t eid);
uint32_t AppliedSeq(uint32_t eid);
uint32_t FirstAppliedSeq(uint32_t eid);
uint32_t JoinSeq(uint32_t eid);

// Logs the pin cost (the client's plain-bucket pin body: its calls and milliseconds, counted under the perf probe) when the probe is
// armed. The bucket drills call it at their verdict, since a run the rig ends never reaches the session's teardown.
void PinCostLine();
}  // namespace dev

}  // namespace coop::bucket_sync
