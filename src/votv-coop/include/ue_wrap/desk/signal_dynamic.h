// ue_wrap/signal_dynamic.h -- the Fstruct_signalDataDynamic (0x70) bridge.
//
// This ONE struct is both the desk refiner's loaded signal (comp_data_0) and
// the element of gamemode.savedSignals_0 (the desk signal library) -- the
// 2026-06-12 savedSignals RE corrected the prior doc: rows are pure
// POD+strings (NO texture/sound pointers; those live in the per-TYPE template
// Fstruct_signal_data, rebuilt identically on every peer from the
// list_signals DataTable and re-derived natively at play time via
// lib_C.dynamicToSignal). So the row is fully serializable; the only heavy
// member is `image` (a PNG byte TArray used solely by the laptop photo view),
// which the live mirror SKIPS (deferred bulk-lane increment; join converges
// via the v56 save transfer).
//
// Member offsets are dump-authoritative (struct_signalDataDynamic.hpp; the
// GUID-mangled cooked names make FindPropertyOffset unreliable here -- the
// door_box precedent).
//
// Principle-7 engine-wrapper layer; no network logic.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::signal_dynamic {

// Field offsets within Fstruct_signalDataDynamic (stride 0x70).
inline constexpr int32_t kOff_name     = 0x00;  // FString
inline constexpr int32_t kOff_level    = 0x10;  // int32
inline constexpr int32_t kOff_id       = 0x18;  // FString (base64url, ~22 chars)
inline constexpr int32_t kOff_size     = 0x28;  // float
inline constexpr int32_t kOff_decoded  = 0x2C;  // float
inline constexpr int32_t kOff_date     = 0x30;  // FDateTime (int64 ticks)
inline constexpr int32_t kOff_isCopy   = 0x38;  // bool
inline constexpr int32_t kOff_polarity = 0x3C;  // int32
inline constexpr int32_t kOff_loc      = 0x40;  // FVector2D
inline constexpr int32_t kOff_object   = 0x48;  // FName
inline constexpr int32_t kOff_signal   = 0x50;  // FName (the template key)
inline constexpr int32_t kOff_image    = 0x58;  // Fstruct_byteImage = TArray<uint8>
inline constexpr int32_t kOff_freq     = 0x68;  // enum byte
inline constexpr int32_t kOff_qual     = 0x69;  // enum byte
inline constexpr int32_t kOff_objType  = 0x6A;  // enum byte
inline constexpr int32_t kOff_daq      = 0x6C;  // float downloadedAtQuality
inline constexpr int32_t kStride       = 0x70;

// Wire-facing view of one row (image deliberately absent).
struct Row {
    std::wstring name;            // display name ("<type> [n]", user-renamable)
    std::wstring id;              // non-unique across copies; nothing looks rows up by it
    std::wstring object;          // FName leaf as string
    std::wstring signal;          // FName leaf as string -- the template key
    int32_t level = 0;
    int32_t polarity = 0;
    float   size = 0;             // saveSignal gate: must be > 0
    float   decoded = 0;          // saveSignal gate: must be >= size
    float   downloadedAtQuality = 0;
    float   locX = 0, locY = 0;
    int64_t date = 0;             // FDateTime ticks (ship non-epoch or the receiver stamps Now)
    bool    isCopy = false;
    uint8_t frequency = 0, quality = 0, objectType = 0;
    bool    hasData = false;      // comp empty-state marker (size>0 on real rows)
};

// Raw-read the struct at `base` into `out` (FStrings read directly; FNames
// via the engine name table). Zero reflected calls.
bool ReadStruct(const void* base, Row& out);

// Write `in` into the LIVE struct at `base`: PODs raw, FStrings ENGINE-MINTED
// (fstring_utils -- the game later reassigns/destroys these fields with the
// engine allocator), FNames interned via fname_utils. The previous string
// buffers deliberately leak (pin doctrine; button-rate writes). The image
// array is emptied (Num=0, allocation untouched) so a stale photo never
// attaches to a different signal. Game thread.
bool WriteStructLive(void* base, const Row& in);

// Build the 0x70 param-frame bytes for a struct PARAMETER (gamemode.saveSignal's
// `signal`): PODs raw, FStrings pointing AT `in`'s buffers (the callee
// deep-copies during the call -- `in` must outlive it), FNames interned,
// image empty. Game thread.
bool BuildParamBytes(const Row& in, uint8_t out[kStride]);

}  // namespace ue_wrap::signal_dynamic
