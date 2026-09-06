// coop/dev/store_table_probe.h -- DEV-ONLY, READ-ONLY. Decides HOW the mod reads a row out of
// the `list_store` UDataTable, measured against a frozen offline truth.
//
// The host prices a client's shop order from its OWN store table, which first requires being able
// to read a `list_store` row at all. Candidate mechanisms were measured in one pass against the
// table's known contents: 473 rows, price sum 73271, FNV-1a64 digests 7917FC66914020E1 over
// sorted lowercased `name=price` lines and 3D110846BD629428 over sorted lowercased `name` lines.
// Two digests, because a mechanism can get the row SET right and the PRICES wrong, and one
// digest cannot tell those apart.
//
// What production stands on: a raw walk of `UDataTable::RowMap` is the READER, because the consumer
// memcpys the live row wholesale into the native cart item, and GetDataTableColumnAsString is the
// layout-free GATE that verifies every price the walk produces. GetDataTableRowFromName cannot be
// used from C++ at all -- its CustomThunk compares the declared out-param struct against the
// table's RowStruct and bails, which only the BP compiler's compile-time retyping avoids.

#pragma once

namespace coop::dev::store_table_probe {

// Run the one-shot store-table census if enabled and `list_store` resolves. Game thread.
//
// STRICTLY READ-ONLY: it resolves objects, calls two BlueprintCallable getters, reads bytes and
// logs. It never writes engine state, never touches the wire, never persists. Nothing in the
// cooked corpus can invalidate it either -- the mutating DataTable functions appear in zero
// assets and the `list_store` consumers call only getters, which is what licenses comparing the
// runtime table against a cooked export.
//
// One-shot: it latches after the first pass that resolves the table. ini-gated OFF
// (`[dev] store_table_probe=1`), and it never ships enabled. KEEP IT -- it is the
// version-migration instrument for this table: on a new game cook run it FIRST, and a mismatch
// names which half moved, the row set or the prices, before anything is re-derived.
void Tick();

}  // namespace coop::dev::store_table_probe
