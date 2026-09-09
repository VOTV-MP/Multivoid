// ue_wrap/world/store_catalog.h -- the laptop shop's price list, read from the game's `list_store`
// UDataTable. Engine-wrapper layer (principle 7): no network, no coop state, no policy. It answers
// "what does the game say row <name> is?" and coop/items/order_sync decides what to do about it --
// the arbiter's table behind the rule that an intent names WHAT, never WHAT IT COSTS (the lane is
// docs/devices.md, "Shop orders").
// It is also the only encoding that can NAME a shop item: 473 rows map onto 368 classes, so 112
// have no unique class. `generateStore` stamps the row key into `Fstruct_store.name` at runtime;
// in the table itself that field reads "None" on every row.
// A raw RowMap walk reads the rows, since it alone yields row BYTES, which the commit memcpys into
// the native cart element. The walk holds the only layout assumptions here, so every price is
// checked against the fully reflected `GetDataTableColumnAsString`; one disagreement invalidates
// the whole catalog and the host refuses client orders rather than charge a number it cannot vouch
// for. `VOTVCOOP_STORE_CATALOG_BREAK=1` makes the walk read the wrong field, so that refusal can
// be seen firing rather than assumed. Built once per process behind a CachedObjRef; offsets
// resolved by name off the RowStruct, so no version-coupled literal. Game thread only.

#pragma once

#include "ue_wrap/core/reflection.h"

#include <cstdint>
#include <string>

namespace ue_wrap::store_catalog {

// One shop row as the game defines it. `data` points at the LIVE row inside the DataTable's RowMap
// -- valid for as long as `Ready()` keeps returning true, which is the table's own lifetime. Do not
// free it, do not hold it across a `Ready()` that returned false.
struct Row {
    const uint8_t*   data  = nullptr;  // the live Fstruct_store row
    int32_t          price = 0;        // its `price` member, gate-verified
    reflection::FName key{};           // the RowMap key, kept as an FName rather than re-minted
                                       // from a string: the commit path has to stamp it into the
                                       // item (the table stores "None"), and round-tripping it
                                       // through `StringToFName` was both a ProcessEvent PER ITEM
                                       // and a SILENT failure -- that helper returns NAME_None when
                                       // Kismet is unresolved, which is exactly the state this
                                       // change exists to stop shipping. The walk already reads
                                       // this value; it used to throw it away.
};

// Build the catalog if it is not built, and report whether it is usable. False means the table did
// not resolve, or the gate found a disagreement -- callers MUST refuse rather than guess. Cheap
// after the first successful call. Game thread.
bool Ready();

// The row for a `list_store` key (as carried in Fstruct_store.name), or nullptr if the catalog is
// unusable or the key is unknown. Case-insensitive, because FName comparison is. Game thread.
const Row* Find(const std::wstring& rowName);

// Byte offset of the `subcategory` FText inside a row, resolved by name. The commit path overwrites
// exactly this field (order_economy::CommitOrder says why) and needs nothing else. -1 if unusable.
//
// BUILDS THE CATALOG IF IT IS NOT BUILT, like Find(). Returning a cached offset without building
// was a critical defect: on a real client nothing else on the forward path called Ready(), so the
// catalog never built, this returned -1 forever, and every client order failed to forward -- after
// the client had already debited itself and disarmed its own delivery.
int32_t SubcategoryOffset();

// Byte offset of the `name` FName inside a row, resolved by name. This module owns the row's SHAPE,
// so the one other place that reads a field out of an Fstruct_store -- order_economy::ReadOrder on
// the CLIENT, pulling the row key out of a locally-placed order -- asks here rather than carrying
// its own literal. -1 if unusable, and it BUILDS if needed, like SubcategoryOffset.
int32_t NameOffset();  // same: BUILDS if needed (see SubcategoryOffset)

}  // namespace ue_wrap::store_catalog
