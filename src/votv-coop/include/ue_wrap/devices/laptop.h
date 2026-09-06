// ue_wrap/laptop.h -- the stationary base PC (Alaptop_C) engine wrapper.
//
// RE ground truth: research/findings/computers-devices/votv-laptop-pc-RE-2026-07-17.md
// (bytecode-cited offsets + chains). Offsets resolved live via reflection
// (version-portable); the Alpha 0.9.0-n values are logged fallbacks.
//
// Scope (v1, laptop_sync): the power/boot axis (isOpened / powered /
// actionOptionIndex b8) + the floppy slot axis (floppyType / zip /
// readWrites / nametype / objectData JSON / data[] scalar apply)
// + the disc prop content accessors (Aprop_floppyDisc_C .data/.readWrites).
// v121 (OPEN-10, laptop_buffer_sync): the file-buffer QUAD accessors
// (floppyData/floppyBuffer/floppyBufferUIDs/floppyReadwrites) + the widget
// rebuild (per-row teardown = native removeBuffer semantics; rebuild = native
// loadData recipe genFloppyBuffer + updFloppy) + the widget-side buffer
// mirror digest (selftest discrimination of a stale rebuild).
//
// No network logic, no coop state (principle 7). Game thread only.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::laptop {

// Resolve classes/offsets/functions (1 Hz retry backoff inside). True once ready.
bool EnsureResolved();

// The single placed stationary laptop (cached ptr + IsLiveByIndex re-check).
void* Instance();

// ---- power/boot axis ----
struct PowerState {
    bool powered  = false;  // wall power (mirrors gamemode.powerChanged)
    bool isOpened = false;  // PC booted/ON
    bool anim     = false;  // boot/shutdown latent in progress
};
bool ReadPower(PowerState& out);

// Reflected actionOptionIndex(player=null, hit={}, action=b8, lookAt=null) --
// the native power-button press (empty-frame proof: beginplayTurnOn@815).
bool CallPowerToggle();

// ---- floppy slot axis ----
struct SlotState {
    int32_t floppyType = -1;   // -1 = empty
    bool    zip        = false;
    int32_t readWrites = -1;
};
bool ReadSlot(SlotState& out);

struct SlotContent {
    std::wstring nametype;               // floppyNametype
    std::wstring objectData;             // floppyObjectData (JSON of the disc's struct_save)
    std::vector<std::wstring> data;      // floppyData
};
bool ReadSlotContent(SlotContent& out);

// Receiver-side insert/eject scalar apply: floppyType/zip/readWrites raw
// (native writes them raw too), nametype/objectData via engine-side FString
// mint, data[] via EngineAlloc'd TArray<FString> mint. Refreshes the widget
// (updFloppy) when reachable.
bool WriteSlot(const SlotState& st, const SlotContent& content);
bool WriteSlotScalars(const SlotState& st);  // scalars only; strings untouched
bool ClearSlot();  // floppyType=-1 + arrays/strings emptied + widget refresh

// ---- disc prop accessors (Aprop_floppyDisc_C) ----
bool  IsDiscClass(void* cls);      // any prop_floppyDisc variant
struct DiscContent {
    int32_t readWrites = -1;
    std::vector<std::wstring> data;  // .data @0x368-equivalent (resolved)
};
bool ReadDiscContent(void* discActor, DiscContent& out);
bool WriteDiscContent(void* discActor, const DiscContent& in);

// ---- the file-buffer quad (v121, OPEN-10) ----
struct BufferQuad {
    std::vector<std::wstring> data;      // floppyData (also slot-owned; quad reads it whole)
    std::vector<std::wstring> buffer;    // floppyBuffer
    std::vector<int32_t>      bufferUids; // floppyBufferUIDs (parallel to buffer)
    int32_t readWrites = -1;             // floppyReadwrites
};
bool ReadQuad(BufferQuad& out);

// The cheap int pre-filter (R3 proof: EVERY native buffer verb changes at
// least one of these; rw is monotone-decreasing between inserts): array nums
// + rw without any string copies.
bool ReadQuadInts(int32_t& fdNum, int32_t& fbNum, int32_t& uidNum, int32_t& rw);

// Receiver-side quad apply: raw-write the four fields, then the WIDGET REBUILD
// (measured invariant: updFloppy regenerates floppyBuffer FROM bufferSlots, so
// stale widgets stomp wire values): RemoveFromParent each bufferSlots row +
// bufferSlots.num=0 (native removeBuffer per-row semantics), genFloppyBuffer
// (native loadData recipe), updFloppy. False if the widget is unreachable
// (fields are still written).
bool WriteQuadAndRebuild(const BufferQuad& in);

// Widget-side buffer mirror (selftest digest): bufferSlots count + FNV-1a64
// over each row widget's 'data' string. False when the widget is unreachable.
bool ReadWidgetBufferMirror(int32_t& outCount, uint64_t& outFnv);

void ResetCache();  // level reload: drop the cached instance

}  // namespace ue_wrap::laptop
