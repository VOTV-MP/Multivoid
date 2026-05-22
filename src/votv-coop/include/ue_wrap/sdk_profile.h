// ue_wrap/sdk_profile.h -- THE VERSION SURFACE.
//
// Everything in this file is specific to one game build. When the mod is
// brought up against a new VOTV version (e.g. 1.0.0), THIS is the file you
// review and re-derive; the rest of the code is version-agnostic logic.
//
// Three kinds of knowledge, three failure modes:
//   * AOB signatures  -- break on ANY engine recompile (even a patch).
//   * struct offsets   -- stable within an engine version; shift across them.
//   * content names    -- change when the game's blueprints/levels change.
//
// Re-derivation workflow (see research/findings/ue-wrap-reflection-2026-05-22.md):
//   1. Launch once with UE4SS; its log prints GUObjectArray / FName::ToString
//      addresses (ground truth). Compute RVAs (addr - module base).
//   2. In IDA (the .exe is loaded), confirm each RVA, derive a unique AOB
//      (wildcard rip displacements), verify uniqueness with find_bytes.
//   3. For ProcessEvent, dump a UObject vtable at runtime (HealthCheck can),
//      find the un-overridden slot, decompile to confirm, derive its AOB.
//   4. Update the constants here; the boot HealthCheck reports what still fails.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ue_wrap::profile {

// ---- target identity -----------------------------------------------------
inline constexpr const char* kTargetGameVersion = "Alpha 0.9.0-n";
inline constexpr const char* kTargetEngineVersion = "UE4.27";  // exe FileVersion 4.27.2.0

// Exe fingerprint of the build these signatures were derived against. The boot
// HealthCheck logs the running exe's size + file version and WARNS on a
// mismatch (signatures/offsets are then suspect). Recorded from a passing
// health check on the 0.9.0-n shipping exe. 0 = not recorded.
inline constexpr unsigned long long kExpectedExeSize = 84751360;

// ---- AOB signatures (most volatile: any recompile breaks these) ----------
// FName::ToString -- unique function prologue; match == function address.
inline constexpr const char* kSigFNameToString =
    "48 89 5C 24 18 55 56 57 48 8B EC 48 83 EC 30 8B 01 48 8B F1 "
    "44 8B 49 04 8B F8 C1 EF 10 48 8B DA 0F B7 C8";

// GUObjectArray -- static init-guard that does `lea rcx,[rip+&GUObjectArray]`.
// Decode the disp32 at match+kGUObjArrayLeaDispOff; the lea ends (rip base) at
// match+kGUObjArrayLeaEndOff, so addr = match + EndOff + disp32.
inline constexpr const char* kSigGUObjectArray =
    "8B 05 ?? ?? ?? ?? 3B 05 ?? ?? ?? ?? 75 13 48 8D 15 ?? ?? ?? ?? "
    "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 45 33 C9 "
    "48 89 45 ?? 4C 8D 45";
inline constexpr size_t kGUObjArrayLeaDispOff = 24;
inline constexpr size_t kGUObjArrayLeaEndOff = 28;

// UObject::ProcessEvent -- unique prologue (vtable index 68). Match == address.
inline constexpr const char* kSigProcessEvent =
    "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC F0 00 00 00 48 8D 6C 24 30 "
    "48 89 9D 18 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C5 48 89 85 B0 00 00 00";

// ---- struct offsets (stable within UE4.27; re-check on an engine bump) ----
namespace off {
inline constexpr size_t UObject_ClassPrivate = 0x10;
inline constexpr size_t UObject_NamePrivate = 0x18;
inline constexpr size_t UObject_OuterPrivate = 0x20;

inline constexpr size_t FUObjectArray_ObjObjects = 0x10;  // FChunkedFixedUObjectArray
inline constexpr size_t Chunk_Objects = 0x00;             // FUObjectItem** chunk table
inline constexpr size_t Chunk_MaxElements = 0x10;
inline constexpr size_t Chunk_NumElements = 0x14;
inline constexpr size_t Chunk_NumChunks = 0x1C;
inline constexpr int32_t ElemsPerChunk = 64 * 1024;
inline constexpr size_t FUObjectItem_Stride = 0x18;       // {Object*, flags, cluster, serial}
}  // namespace off

// ---- content names (change with game content, not the engine) ------------
namespace name {
inline constexpr const wchar_t* MainPlayerClass = L"mainPlayer_C";
inline constexpr const wchar_t* GamemodeClass = L"mainGamemode_C";
inline constexpr const wchar_t* ActorClass = L"Actor";
inline constexpr const wchar_t* WorldClass = L"World";
inline constexpr const wchar_t* SetActorLocationFn = L"K2_SetActorLocation";
inline constexpr const wchar_t* GameplayLevel = L"untitled_1";
}  // namespace name

}  // namespace ue_wrap::profile
