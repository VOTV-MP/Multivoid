// ue_wrap/store_catalog.h -- the laptop shop's own price list, read from the game's `list_store`
// UDataTable. Engine-wrapper layer (principle 7): NO network, NO coop state, NO policy. It answers
// exactly one question -- "what does the game say row <name> is?" -- and coop/items/order_sync
// decides what to do about it.
//
// WHY THIS EXISTS (security A34/A35). A client's laptop shop order is forwarded to the host and
// re-committed there, but the intent carried the client's own `price` and `object` and the host
// wrote them through verbatim; and `makeAnOrder` never charges (the charge lives in `ui_laptop`'s
// Button_order ubergraph, `[V]` bytecode). So every connected client shopped for free BY DEFAULT.
// The rule that fixes it is `docs/COOP_SYNCER_MODEL.md` 2b: **an intent may name WHAT, never WHAT IT
// COSTS** -- the arbiter prices the action from its own tables. This module IS the arbiter's table.
//
// IT IS ALSO THE ONLY ENCODING THAT CAN NAME A SHOP ITEM, which is the part that surprised the
// design. `[V]` The 473 rows map onto only **368 distinct object classes** -- `prop_C` is shared by
// 50 rows, `prop_seed_C` by 26, `prop_rug_C` by 13 -- so **112 of 473 rows are not uniquely
// identified by their class** and the old class-keyed wire was structurally incapable of naming
// them. The row NAME is the shop's real identity, and `[V]` `generateStore` stamps it into
// `Fstruct_store.name` at store-generation time, so every item in a forwarded order already carries
// it. (In the table itself that field is literally "None" on all 473 rows -- it is a runtime stamp.)
//
// HOW THE ROW IS READ, and why it is this way and not one of the two alternatives: measured, by a
// probe that ran all three candidates against a frozen offline truth (`coop/dev/store_table_probe`,
// commit `2a9a97d9`). `UDataTableFunctionLibrary::GetDataTableRowFromName` is a CustomThunk and is
// UNUSABLE from C++ -- it dispatched for all 473 rows, returned false every time, and never wrote
// the out slot. `GetDataTableColumnAsString` and a raw `RowMap` walk BOTH reproduced the truth
// exactly. The walk ships because it is the only one that yields **row BYTES**, which the commit
// needs: the host memcpys the live row wholesale into the native cart element rather than
// re-assembling nine fields, which is also what fixes the ~141 of 473 rows that mis-delivered when
// `asProp` / `parseRowNameToObject` were written as NAME_None (`prop_orderBox` reads them back).
//
// AND THE COLUMN READER BECOMES THE GATE. The walk carries the only layout assumptions in this
// module -- that `RowMap` sits immediately after the reflected `RowStruct`, and the TSetElement
// stride -- so every price it produces is checked against `GetDataTableColumnAsString`, which is
// fully reflected, needs no layout at all, and returns the whole column in ONE dispatch. A single
// disagreement invalidates the WHOLE catalog: `Ready()` goes false, the host refuses client orders
// with a loud log, and nothing is ever mischarged. (This retired an earlier plan to gate on
// `lib_C::sellObject`, which returns `FTrunc(price/2)` -- ambiguous by construction for the 113
// odd-priced rows, so it could never have verified the number actually debited.)
//
// THE FAIL-CLOSED PATH IS DRILLABLE, because a fail-closed branch that cannot fire in a healthy
// build is an instrument that always passes: `VOTVCOOP_STORE_CATALOG_BREAK=1` makes the walk read a
// neighbouring field instead of `price`, so the gate disagrees and the refusal path can be shown RED
// before it is trusted.
//
// LIFETIME. The `UDataTable` is held through a `CachedObjRef`. A cooked asset has a null world stamp
// and `cached_obj_ref.h:93-95` is explicit that this means "not world-scoped -- outliving a world is
// CORRECT for it", so the catalog survives a world load and is built ONCE PER PROCESS. `[V]` It also
// cannot go stale: no Blueprint in the cooked corpus can mutate a DataTable at all (`SetDataTableRow`
// / `AddDataTableRow` / `RemoveDataTableRow` / `EmptyDataTable` / `FillDataTableFromCSV` occur in
// ZERO assets, and the three `list_store` consumers call only getters).
//
// Every field offset here is resolved BY NAME off the table's own `RowStruct`, so this module adds
// no version-coupled literal (`docs/VERSION_MIGRATION.md`). Game thread only (UObject access + a
// ProcessEvent dispatch for the gate).

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::store_catalog {

// One shop row as the game defines it. `data` points at the LIVE row inside the DataTable's RowMap
// -- valid for as long as `Ready()` keeps returning true, which is the table's own lifetime. Do not
// free it, do not hold it across a `Ready()` that returned false.
struct Row {
    const uint8_t* data  = nullptr;  // the live Fstruct_store row
    int32_t        price = 0;        // its `price` member, gate-verified
};

// Build the catalog if it is not built, and report whether it is usable. False means the table did
// not resolve, or the gate found a disagreement -- callers MUST refuse rather than guess. Cheap
// after the first successful call. Game thread.
bool Ready();

// The row for a `list_store` key (as carried in Fstruct_store.name), or nullptr if the catalog is
// unusable or the key is unknown. Case-insensitive, because FName comparison is. Game thread.
const Row* Find(const std::wstring& rowName);

// Row count, 0 when unusable. For logs and for the caller's own sanity checks.
int32_t Count();

// Byte offset of the `subcategory` FText inside a row, resolved by name. The commit path overwrites
// exactly this field (see order_economy::CommitOrder for why) and needs nothing else. -1 if unusable.
int32_t SubcategoryOffset();

// Byte offset of the `name` FName inside a row, resolved by name. This module owns the row's SHAPE,
// so the one other place that has to read a field out of an Fstruct_store -- order_economy::ReadOrder
// on the CLIENT, pulling the row key out of a locally-placed order -- asks here rather than carrying
// its own literal. -1 if unusable. (In the TABLE this field is "None" on all 473 rows; `[V]`
// `generateStore` stamps the row key into it at store-generation time, which is why a live order
// item carries it and a table row does not.)
int32_t NameOffset();

}  // namespace ue_wrap::store_catalog
