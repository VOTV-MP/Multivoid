// coop/props/container_record_edit.h -- the host's record editor for one world container: read its records,
// take one out, put one in. Built on container_contents_io; holds no state, and every entry is the host's, on the
// game thread.
//
// The raw write fires no addObject or takeObj, so the verb watch (container_writer_watch) does not see it, and
// MarkHostWrite is what publishes the edit to the peers. The read here is NOT neutered, so a nested container keeps
// its link to its own slot on the host; a record that came off the wire is neutered before it is written
// (Source::Wire), since its index names a slot in the sender's array. Each write orphans the old array exactly as
// the contents lane's own apply does (container_contents_io).

#pragma once

#include "ue_wrap/actors/save_record.h"

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

enum class Source : uint8_t {
    Host,   // a record read from this host's own container
    Wire,   // a record from a client: its nested index is neutered before the write
};

// Done when the container resolves and its slice slot is writable.
EditResult Probe(uint32_t eid);

EditResult ReadRecords(uint32_t eid, std::vector<ue_wrap::save_record::SaveRecord>& out, bool neuterNested);

// Remove the record at `index`, if its hash (container_slice_wire::RecordHash of the record in wire form) is
// `expectedHash`; `outRecord` gets the record as stored, not neutered. Nothing is written on any other result.
EditResult RemoveAt(uint32_t eid, uint16_t index, uint64_t expectedHash,
                    ue_wrap::save_record::SaveRecord& outRecord);

// Insert at `index`, clamped to the end (an index past the last record appends).
EditResult InsertAt(uint32_t eid, uint16_t index, const ue_wrap::save_record::SaveRecord& record, Source src);

const char* EditResultName(EditResult r);

namespace internal {   // pure

size_t ClampIndex(uint16_t index, size_t length);   // min(index, length)

// BOUNDARY 2: a container record's own slot index is cleared to -1; any other record is unchanged.
void NeuterIfNested(ue_wrap::save_record::SaveRecord& r);

}  // namespace internal

}  // namespace coop::props::container_record_edit
