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
// The PC buffer (floppyBuffer/UIDs) + the portable PC are OUT (TRACKER row).
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
bool  IsZipDiscClass(void* cls);   // the white (_Wh) zip disc
struct DiscContent {
    int32_t readWrites = -1;
    std::vector<std::wstring> data;  // .data @0x368-equivalent (resolved)
};
bool ReadDiscContent(void* discActor, DiscContent& out);
bool WriteDiscContent(void* discActor, const DiscContent& in);

void ResetCache();  // level reload: drop the cached instance

}  // namespace ue_wrap::laptop
