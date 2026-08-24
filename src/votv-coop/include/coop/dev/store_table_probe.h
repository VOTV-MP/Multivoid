// coop/dev/store_table_probe.h -- DEV-ONLY, READ-ONLY. Decide HOW the mod reads a row out of the
// `list_store` UDataTable, by measuring three candidate mechanisms against a frozen offline truth.
//
// WHY THIS EXISTS. Security A34/A35: a client's laptop shop order is committed by the host with the
// client's own `price` and `object` written through verbatim, and `makeAnOrder` never charges, so
// every connected client shops for free by default. The fix is that the arbiter prices the order
// from ITS OWN store table -- which first requires the mod to be able to read a `list_store` row at
// all. Three mechanisms are plausible and each has exactly one unmeasured leg, so this probe runs
// all three in one pass and prints a comparable verdict per candidate. The winner is the ONLY one
// written into production code (RULE 2 -- no fallback chain), and its identity also decides whether
// a runtime layout gate ships at all: a reader that resolves `price` by PROPERTY NAME has no
// assumption to guard, and a gate over a non-assumption is an instrument that always passes
// ([[lesson-an-instrument-blind-to-the-phenomenon-always-passes]]).
//
// THE FROZEN TRUTH (offline, kismet-analyzer over the extracted cooked DataTable export, and it is
// a real parse of the row data, not a guess):
//     rows       473
//     price sum  73271
//     digest     7917FC66914020E1   FNV-1a64 over sorted lowercased "name=price\n"
//     name-only  3D110846BD629428   FNV-1a64 over sorted lowercased "name\n"
// The name-only digest is carried DELIBERATELY: a mechanism can get the row SET right and the
// PRICES wrong, and one digest cannot tell those apart. Two digests name which half failed.
//
// THE THREE CANDIDATES
//   (a) UDataTableFunctionLibrary::GetDataTableRowFromName -- a CustomThunk. Its exec compares the
//       DECLARED out-param struct (FTableRowBase) against the table's RowStruct and, on the reading
//       of the engine source this probe exists to TEST, bails when they differ; the BP compiler
//       retypes that property at compile time, which a C++ ProcessEvent call cannot do. If that
//       reading is wrong the thunk memcpys a 0x4D row into the out slot, so this candidate is called
//       with a deliberately OVERSIZED params blob and a canary after the out slot: whether anything
//       was written is then a measurement rather than an argument, and a wrong model corrupts
//       nothing. This is the leg conceded in a /qf round as recalled-source inference.
//   (b) UDataTableFunctionLibrary::GetDataTableColumnAsString(table, propertyName) zipped with
//       GetDataTableRowNames(table). No struct layout at all -- the cleanest winner if it works.
//       Two unmeasured legs: whether the property is matched by its BP-MANGLED name
//       (`price_11_BE3A...`) or its friendly name (`price`), and whether the two calls iterate the
//       same RowMap in the same order. Both are settled by the digests: a name/price mismatch or a
//       zip misalignment cannot produce 7917FC66914020E1.
//   (c) A raw walk of `UDataTable::RowMap`. `RowStruct` IS a reflected UPROPERTY, so the walk needs
//       exactly one derived constant -- RowMap sits immediately after it -- plus the TSetElement
//       stride. Every one of those assumptions is caught by the digests at once.
//
// STRICTLY READ-ONLY. It resolves objects, calls two BlueprintCallable engine getters, reads bytes,
// and logs. It never writes engine state, never touches the wire, never persists. `sellObject` is
// NOT called here: it is a candidate runtime gate, not a reader, and its 44-statement body was
// already censused (exactly one mutation-ish call, a PrintString, unreachable with a null actor).
//
// ONE-SHOT: it latches after the first pass that resolves the table, so a running game logs one
// block, not a per-tick wall. ini-gated OFF (`[dev] store_table_probe=1`); never ships enabled.
// (RULE 2 exempts probes/diagnostics/tools -- [[feedback-rule2-exempts-probes-diagnostics-tools]].)
//
// ---- STATUS: RUN 2026-08-24, autonomous LAN smoke, host log. RESULT BELOW. ----------------------
//     table class=DataTable  RowStruct@40 -> UserDefinedStruct  price@0
//     GetDataTableRowFromName frame=32 : table@0 RowName@8 OutRow@16 ReturnValue@24(1)
//     (a) dispatched for all 473 rows, returned false every time, never wrote the out slot -> UNUSABLE
//     (b) ColumnAsString, BOTH the mangled and the friendly property name: 473 / 73271 / both digests MATCH
//     (c) RowMap walk:                                                    473 / 73271 / both digests MATCH
//
// THE DECISION: **(c) is written into production; (b) becomes the runtime GATE.** Both readers are
// correct, so the digests alone could not choose -- the acceptance criterion was missing the term
// that actually decides it: the consumer memcpys the LIVE ROW WHOLESALE into the native cart item,
// and (b) returns `TArray<FString>` and can never hand back row bytes. (b) is not discarded, it is
// promoted: it is an EXACT, fully reflected, layout-free price for every row, so the catalog builds
// through (c) and verifies every price against (b). That retires the previously-planned
// `lib_C::sellObject` gate outright -- sellObject returns `FTrunc(price/2)`, ambiguous by
// construction for the 113 odd-priced rows, and it now has no job.
//
// ALSO MEASURED, and it is what licenses comparing a RUNTIME table against a COOKED export:
// `SetDataTableRow` / `AddDataTableRow` / `RemoveDataTableRow` / `EmptyDataTable` /
// `FillDataTableFromCSV` occur in ZERO assets of the cooked corpus, and the three `list_store`
// consumers call only getters (28x GetDataTableRowFromName, 7x GetDataTableRowNames). No Blueprint
// can mutate a row, so a correct reader cannot be failed by a legitimately-changed table.
//
// KEEP THIS PROBE. It is the version-migration instrument for this table: on a new game cook, run it
// FIRST -- a MISMATCH names which half moved (row set vs prices) before anything is re-derived.

#pragma once

namespace coop::dev::store_table_probe {

// Run the one-shot three-way census if enabled and `list_store` resolves. Game thread.
void Tick();

}  // namespace coop::dev::store_table_probe
