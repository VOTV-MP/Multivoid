// coop/dev/floppy_selftest_world.h -- what the disc driver's world holds, and how it is named.
//
// Co-located private header (src tree, not include/): nothing outside the driver may reach this.
// The driver is two concepts. This one READS and PREPARES: which boxes the episodes will use,
// which discs they will name, what each of them holds right now, and the census lines that put all
// of that in the log. The other half decides what to do and what it means, and lives beside it.
//
// Both peers must name the same targets by the same rule, or an episode quietly acts on a
// different disc on each machine, so every pick is logged with what it picked.
//
// Game thread throughout.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::dev::floppy_selftest::world {

// Boxes the episodes use. Discs outnumber them because the cross-peer eject is REPEATED: it is a
// race, and one pass of a race is an anecdote, so each repeat takes a disc of its own and the run
// reads as a ratio. A repeat that re-used the previous disc would measure nothing once the first
// one was lost. The last three go through the laptop instead of a box.
constexpr int kTargets = 3;
constexpr int kDiscs   = 11;

// Discs 8 and 9 carry this many rows, which puts the laptop's slot content well past the 4 KB
// laptop_sync cuts a slot blob at: the rows ride twice, in the save JSON and in floppyData. A live
// run lost a disc's content through exactly that cut (bug 19); every other disc carries one row.
constexpr int kBigFirst = 8;
constexpr int kBigLast  = 9;
constexpr int kBigRows  = 64;
inline int ExpectedRows(int disc) { return (disc >= kBigFirst && disc <= kBigLast) ? kBigRows : 1; }

// The marker a seeded disc carries in its own data array, so a disc this instrument prepared can
// be told from one it did not. The read-writes value is this plus the disc index; the class
// default is a round 32.
constexpr const wchar_t* kMarker = L"MULTIVOID-FLOPPY-SELFTEST";
constexpr int32_t kMarkerReadWrites = 900;

// One slot reading for a log line: the scalars plus the two sizes that say whether the box took
// the disc's content.
struct BoxSlot {
    int32_t floppyType    = -1;
    int32_t readWrites    = -1;
    int32_t dataNum       = 0;
    int32_t objectDataLen = 0;
};
bool ReadBoxSlot(void* box, BoxSlot& out);

// The base laptop, or null before it resolves, and its slot in the same shape.
void* Laptop();
bool ReadLaptopSlot(BoxSlot& out);

// The first boxes in the gamemode's own order whose slot is empty, on a retry backoff until the
// world is up. False until every target is in hand; latches off with a line saying so rather than
// walking the box list for the rest of the session.
bool ResolveBoxes();

// The target box, live-checked, or null. Its label, for the log.
void* Box(int index);
const std::wstring& BoxName(int index);

// The disc an episode names, by index. Empty until the pick has run.
const std::wstring& DiscKey(int index);

// HOST only, once: make sure the world holds enough discs and that each carries a payload a reader
// can attribute, and publish each stamp as the disc's save record, so the client's copy carries it
// too and an episode whose insert is the client's has something to lose.
void SeedAndStamp(coop::net::Session* session);

// Both peers name their discs by the same rule -- the lowest keys in the world -- and print them,
// so a set that is not shared shows up as two different lists rather than as an episode that
// quietly used another disc.
void PickDiscs(bool isHost);

// Every live disc and every target box, in one line. `sinceMs` is the age of the run, printed so
// two logs can be read side by side.
void Census(const char* tag, bool isHost, uint64_t sinceMs);

// Session teardown: the targets, the names, the picks and the per-class verdict cache.
void Reset();

}  // namespace coop::dev::floppy_selftest::world
