// ue_wrap/devices/floppybox.h -- the disc crate (Aprop_floppyBox_C) wrapper.
//
// RE ground truth: a LIFO stack of up to 15 discs held as two parallel persisted arrays --
// floppyTypes (TArray<int32>) and floppyData (TArray<FString> of actorDataToString blobs).
// addFloppy appends and the caller destroys the held disc; getFloppy spawns the tail INTO THE
// HAND and removes it; gen() rebuilds the instanced-mesh visuals from the arrays.
// Aprop_floppyBox_C : Aprop_C, so it is a prop-universe row and eid-addressable.
//
// Same ClassOf-verdict-cache identity as portable_pc (no FindClass polling). No network
// logic, no coop state (principle 7). Game thread only.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::floppybox {

// The NATIVE stack depth: addFloppy appends onto a LIFO of at most this many discs. It lives
// here rather than at a use site because it is a property of the GAME's crate, not of any one
// sync lane -- a receive path that lets a wire op push past it has written a state the game
// itself cannot produce.
inline constexpr size_t kNativeCapacity = 15;

bool IsFloppyBoxClass(void* cls);

struct BoxArrays {
    std::vector<int32_t>      types;
    std::vector<std::wstring> data;
};
bool ReadArrays(void* actor, BoxArrays& out);

// Alloc-free content digest over the RAW field buffers (types ints and data FString wchar
// bytes through the TArray headers -- no wstring mints). It is the 1 Hz sweep's pre-filter, so
// a full ReadArrays runs only when the digest changes.
bool ReadDigest(void* actor, uint64_t& outDigest);

// Raw-write both arrays + reflected gen() (the native visual rebuild).
bool WriteArraysAndGen(void* actor, const BoxArrays& in);

void ResetCache();

}  // namespace ue_wrap::floppybox
