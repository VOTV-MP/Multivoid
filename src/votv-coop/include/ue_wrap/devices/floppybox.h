// ue_wrap/floppybox.h -- the disc crate (Aprop_floppyBox_C) wrapper.
//
// RE ground truth: the OPEN-10 pass (votv-laptop-v2-OPEN10-impl-DESIGN-
// 2026-07-18.md SS1 fact 6 + SS4): a LIFO stack of up to 15 discs held as two
// parallel persisted arrays -- floppyTypes (TArray<int32> @0x3A8) + floppyData
// (TArray<FString> @0x3B8, actorDataToString blobs). addFloppy appends + the
// caller destroys the held disc; getFloppy spawns the tail INTO THE HAND +
// removes. gen() rebuilds the instanced-mesh visuals from the arrays.
// Aprop_floppyBox_C : Aprop_C (prop universe row -> eid-addressable).
//
// Same ClassOf-verdict-cache identity as portable_pc (no FindClass polling).
// No network logic, no coop state (principle 7). Game thread only.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::floppybox {

// The NATIVE stack depth (RE fact, see the header comment above): addFloppy appends
// onto a LIFO of at most this many discs. It lives here rather than at a use site
// because it is a property of the GAME's crate, not of any one sync lane -- a receive
// path that lets a wire op push past it has written a state the game itself cannot
// produce (security W8).
inline constexpr size_t kNativeCapacity = 15;

bool IsFloppyBoxClass(void* cls);

struct BoxArrays {
    std::vector<int32_t>      types;
    std::vector<std::wstring> data;
};
bool ReadArrays(void* actor, BoxArrays& out);

// Alloc-free content digest over the RAW field buffers (types ints + data
// FString wchar bytes via the TArray headers -- no wstring mints). The 1 Hz
// sweep pre-filter (perf audit F1): full ReadArrays only on digest change.
bool ReadDigest(void* actor, uint64_t& outDigest);

// Raw-write both arrays + reflected gen() (the native visual rebuild).
bool WriteArraysAndGen(void* actor, const BoxArrays& in);

void ResetCache();

}  // namespace ue_wrap::floppybox
