// ue_wrap/devices/laptop.h -- the stationary base PC (Alaptop_C) engine wrapper. Offsets are
// resolved live via reflection (version-portable); the Alpha 0.9.0-n values are logged fallbacks.
//
// What the wrapper reaches:
//   - the power and boot axis: isOpened, powered, and the native power-button press
//   - the file-buffer quad (floppyData, floppyBuffer, floppyBufferUIDs, floppyReadwrites), the
//     widget rebuild a write to it must be followed by, and a digest of the widget's own mirror
//     of that buffer
//
// The floppy SLOT is not here: every device that has one holds the same fields, so they live in
// ue_wrap/devices/floppy_slot. floppyData is the one field the two share -- the game keeps the
// disc's rows and the laptop's file buffer in it -- so a quad write and a slot write both reach
// it, and the quad is the one that rebuilds the widget's own copy.
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

// Reflected actionOptionIndex(player=null, hit={}, action=b8, lookAt=null) -- the native
// power-button press. The hit frame is never read on this path: the game's own self-call
// passes a default-initialised FHitResult with the same action byte, and the ubergraph
// stores that parameter without ever reading it.
bool CallPowerToggle();

// ---- the file-buffer quad ----
struct BufferQuad {
    std::vector<std::wstring> data;      // floppyData (also slot-owned; quad reads it whole)
    std::vector<std::wstring> buffer;    // floppyBuffer
    std::vector<int32_t>      bufferUids; // floppyBufferUIDs (parallel to buffer)
    int32_t readWrites = -1;             // floppyReadwrites
};
bool ReadQuad(BufferQuad& out);

// The cheap int pre-filter: the three array counts and rw, without any string copies. Measured:
// every native buffer verb changes at least one of them, and rw only decreases between inserts.
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
