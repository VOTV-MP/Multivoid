// coop/props/bucket_internal.h -- what the bucket lane's files share: bucket_sync (the rows), bucket_pin (the client copies'
// tick) and bucket_record_gate (a client's drop record). Nothing outside coop/props includes it.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <string>

namespace coop::bucket_sync::detail {

// The role of the session the lane runs in: a running host, a running client.
bool IsHost();
bool IsClient();

// The element id of a world copy, 0 for an actor with none (a held item, a copy not bound yet).
uint32_t EidOf(void* actor);

// Whether this is the first row refused for the record of `key` this session: a row the transport took no part of is said once per
// key, whichever path refused it.
bool FirstRowRefusal(const std::wstring& key);

// HOST: the host's row of `actor` is owed to slot `slot`: its copy has no element id yet, so the row goes at the mint of one.
void OweRow(int slot, void* actor);

// CLIENT: one pin body ran for `ticks` QPC ticks (counted only while the perf probe is armed; said at the session end).
void NotePinCost(unsigned long long ticks);

// HOST: a bucket's own tick ran on `actor`: its row is marked when a field moved past a row's worth since the last one sent.
void NoteHostTick(void* actor);

// CLIENT: the last row applied to the copy of element `eid`, or null before the first.
const coop::net::BucketRow* HostRow(uint32_t eid);

// bucket_pin.cpp: registers the watches on the water and concrete buckets' ticks and deleteAlc, and settles them. Per tick.
void SettlePinWatches();

// bucket_record_gate.cpp: registers the class gate on a client's drop record of a bucket, a bag or a trowel. Once.
void RegisterRecordGate();

}  // namespace coop::bucket_sync::detail
