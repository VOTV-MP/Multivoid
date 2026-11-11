// coop/props/container_record_edit.h -- the host's record editor for one shared inventory, a world container (named by
// its element id) or the delivery drone's cargo (coop/props/container_target::kDroneCargoEid): read its records, take one
// out, take part of a stack, merge one into another, put one in. Built on container_contents_io; holds no state, and every
// entry is the host's, on the game thread.
//
// The raw write runs none of the inventory's verbs, so the verb watch (container_writer_watch) does not see it, and
// MarkWritten (container_target) is what publishes the edit to the peers. Each write orphans the old array exactly as the
// contents lane's own apply does (container_contents_io).

#pragma once

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/inventory_item.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::props::container_record_edit {

enum class EditResult : uint8_t {
    Done,
    NotHost,       // this peer is not the host
    NoElement,     // the eid names no live container
    NoInventory,   // the container has no world inventory component
    Unreadable,    // the records do not read, or the container holds more than the lane's cap
    NotReady,      // the inventory's slice slot does not resolve yet
    Refused,       // not written: over the cap, a personal inventory, or an alloc failure
    OutOfRange,    // the index is past the end
    Mismatch,      // the record at the index is not the one the caller named
};

// Done when the container resolves and its slice slot is writable.
EditResult Probe(uint32_t eid);

EditResult ReadRecords(uint32_t eid, std::vector<ue_wrap::inventory_item::Item>& out);

// Remove the record at `index`, if its hash (container_slice_wire::RecordHash) is `expectedHash`; `outRecord` gets the
// record as stored. Nothing is written on any other result.
EditResult RemoveAt(uint32_t eid, uint16_t index, uint64_t expectedHash,
                    ue_wrap::inventory_item::Item& outRecord);

// Take `amount` units out of the stack at `index`, if its hash is `expectedHash`: the record keeps the rest, and
// `outRecord` gets a copy of it standing for the `amount` taken. Refused (nothing written) unless 0 < amount < the
// record's amount: a take of the whole stack is RemoveAt's.
EditResult TakeFromStackAt(uint32_t eid, uint16_t index, uint64_t expectedHash, uint8_t amount,
                           ue_wrap::inventory_item::Item& outRecord);

// Merge the record at `fromIndex` (hash `fromHash`) into the one at `toIndex` (hash `toHash`): the target's amount rises
// by the source's and the source leaves. Refused (nothing written) when they are one record, are not the same item
// (class and name), or the sum is past what an amount holds.
EditResult MergeAt(uint32_t eid, uint16_t toIndex, uint64_t toHash, uint16_t fromIndex, uint64_t fromHash);

// Insert at `index`, clamped to the end (an index past the last record appends).
EditResult InsertAt(uint32_t eid, uint16_t index, const ue_wrap::inventory_item::Item& record);

// What a put into the target is judged on. For a container this is the container's (it can be locked, or a mailbox); the
// drone's cargo has neither. False when the target does not resolve or a field does not.
bool PutTermsOf(uint32_t eid, ue_wrap::container_inventory::PutTerms& out);

const char* EditResultName(EditResult r);

namespace dev {   // called only from src/coop/dev/ (the container intent drill's sale arm)

// Writes an empty record list over the container and publishes it, as a writer the act does not see would (a sale
// that empties it). `cleared` gets the number of records it held; Done when it was written.
EditResult Clear(uint32_t eid, size_t& cleared);

}  // namespace dev

namespace internal {   // pure

size_t ClampIndex(uint16_t index, size_t length);   // min(index, length)

}  // namespace internal

}  // namespace coop::props::container_record_edit
