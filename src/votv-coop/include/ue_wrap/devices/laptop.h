// ue_wrap/devices/laptop.h -- the stationary base PC (Alaptop_C) engine wrapper. Offsets are
// resolved live via reflection (version-portable); the Alpha 0.9.0-n values are logged fallbacks.
//
// What the wrapper reaches:
//   - the power and boot axis: isOpened, powered, and the native power-button press
//   - the file-buffer quad (floppyData, floppyBuffer, floppyBufferUIDs, floppyReadwrites), and the
//     widget rebuild a write to it must be followed by
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

// The actor the laptop's interface is shown on: the laptop itself, or a portable PC a player works it through as
// its remote terminal (the interface widget's nearestActor, which each writes as a player enters it); null before
// any has. Game thread.
void* TerminalInUse();

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

// [dev] The laptop's own insert, as the game's overlap runs it with no player: processFloppy through its floppy
// hitbox with `disc` as the manual actor, which casts it to a disc and runs insertFloppy (the disc destroyed, the
// slot filled). And its ejectFloppy. False when unresolved or the call failed.
bool CallInsertDisc(void* disc);
bool CallEjectDisc();

// The slot timeline is still sliding a disc in or out (laptop_C.floppyProcess): both insertFloppy and ejectFloppy
// return at once while it is (`if (floppyProcess) return;`), so a verb dispatched now does nothing. False when
// unresolved. Game thread.
bool FloppyBusy();

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

}  // namespace ue_wrap::laptop
