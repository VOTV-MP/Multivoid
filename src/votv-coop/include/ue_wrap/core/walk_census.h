// ue_wrap/core/walk_census.h -- how often the reflection finders walk the whole object array, and from
// where. Every finder that scans GUObjectArray end to end (FindObject, FindClass on a miss,
// FindObjectByClass, FindObjectsByClass, ChildObjectsOf, CountObjectsByClass) notes its caller's return
// address here; the perf probe prints the rate and, every ten seconds, the six call sites most of the
// window's walks came from, as module+RVA the payload's linker map resolves. A finder a finder calls
// (FindClassDefaultObject calls FindObject) names that finder. FindFunction reads a class's own list.
// Lock-free, any thread. Engine-wrapper layer (principle 7).

#pragma once

namespace ue_wrap::walk_census {

// The call-site table's slots: the six finders have about 640 call sites in the source, and the
// first table, of 64, was full of boot-time sites within seconds, so every later site went uncounted.
inline constexpr int kSiteSlots = 4096;

// One whole-array walk, made on behalf of the code at `callSite` (the finder's _ReturnAddress()).
void NoteArrayWalk(void* callSite);

// Every walk noted since boot, read as a rate.
unsigned long long ArrayWalkCountTotal();

// The walks whose site found the table full: in the total, and attributed to no site.
unsigned long long ArrayWalkUnattributedTotal();

// Slot i of the call-site table: its site (null while unused) and its cumulative walks; false past the
// table.
bool ArrayWalkSiteAt(int i, void** outSite, unsigned long long* outCount);

}  // namespace ue_wrap::walk_census
