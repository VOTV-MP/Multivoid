// coop/dev/atv_tire_probe.cpp -- see the header for why this exists and why the
// declared types below are quoted from the dump rather than inferred.

#include "coop/dev/atv_tire_probe.h"

#include <cstdio>

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

namespace R = ue_wrap::reflection;

namespace coop::atv_tire_probe {
namespace {

bool  g_resolved = false;
bool  g_resolveTried = false;
int32_t g_tiresOff      = -1;   // TArray<bool>  (ElementSize 1)
int32_t g_durabilityOff = -1;   // TArray<float> (ElementSize 4)
int32_t g_dirtOff       = -1;   // TArray<float> (ElementSize 4)
int32_t g_fixesOff      = -1;   // TArray<int32> (ElementSize 4)
int32_t g_typesOff      = -1;   // TArray<uint8> (ElementSize 1)

// Slots we report. The rig is four wheels; the game's arrays are sized by the BP
// and a spare may extend them, so Num is read and clamped rather than assumed.
constexpr int kSlots = 4;

// FScriptArray = { void* Data; int32 Num; int32 Max }. Returns Num, or -1 when the
// array head is unreadable. A NEGATIVE return must stay distinguishable from 0:
// "no element" and "could not read" are the two readings a census like this most
// easily confuses, and conflating them is how a blind field passes for agreement.
int32_t ArrayHead(void* obj, int32_t off, void** dataOut) {
    *dataOut = nullptr;
    if (!obj || off < 0) return -1;
    uint8_t* arr = reinterpret_cast<uint8_t*>(obj) + off;
    void* data = *reinterpret_cast<void**>(arr);
    const int32_t num = *reinterpret_cast<int32_t*>(arr + 8);
    if (!data || num < 0 || num > 64) return -1;   // torn/garbage, not a finding
    *dataOut = data;
    return num;
}

// TArray<bool> stores one BYTE per element (it is not TBitArray). Bit i = slot i.
int32_t ReadBoolMask(void* obj, int32_t off) {
    void* data = nullptr;
    const int32_t num = ArrayHead(obj, off, &data);
    if (num < 0) return -1;
    int32_t mask = 0;
    const int32_t n = num < kSlots ? num : kSlots;
    for (int32_t i = 0; i < n; ++i)
        if (reinterpret_cast<uint8_t*>(data)[i]) mask |= (1 << i);
    return mask;
}

// -1.f is the unreadable sentinel: durability and dirt are both non-negative in
// every BP write (damageWheel clamps with FMax(x, 0)), so a negative can only be ours.
void ReadFloats(void* obj, int32_t off, float out[kSlots]) {
    for (int i = 0; i < kSlots; ++i) out[i] = -1.f;
    void* data = nullptr;
    const int32_t num = ArrayHead(obj, off, &data);
    if (num < 0) return;
    const int32_t n = num < kSlots ? num : kSlots;
    for (int32_t i = 0; i < n; ++i) out[i] = reinterpret_cast<float*>(data)[i];
}

void ReadInts(void* obj, int32_t off, int32_t out[kSlots]) {
    for (int i = 0; i < kSlots; ++i) out[i] = -1;
    void* data = nullptr;
    const int32_t num = ArrayHead(obj, off, &data);
    if (num < 0) return;
    const int32_t n = num < kSlots ? num : kSlots;
    for (int32_t i = 0; i < n; ++i) out[i] = reinterpret_cast<int32_t*>(data)[i];
}

// ByteProperty: one uint8 per element. Packed into a mask-like hex so the line
// stays one field wide; 0xFF in a nibble position is not possible (the enum is small).
int32_t ReadBytesPacked(void* obj, int32_t off) {
    void* data = nullptr;
    const int32_t num = ArrayHead(obj, off, &data);
    if (num < 0) return -1;
    int32_t packed = 0;
    const int32_t n = num < kSlots ? num : kSlots;
    for (int32_t i = 0; i < n; ++i)
        packed |= (static_cast<int32_t>(reinterpret_cast<uint8_t*>(data)[i]) & 0xF) << (i * 4);
    return packed;
}

bool Resolve() {
    if (g_resolved) return true;
    if (g_resolveTried) return false;      // one attempt per process; the class is resident
    g_resolveTried = true;
    void* cls = R::FindClass(L"ATV_C");
    if (!cls) return false;
    g_tiresOff      = R::FindPropertyOffset(cls, L"tires");
    g_durabilityOff = R::FindPropertyOffset(cls, L"tiresDurability");
    g_dirtOff       = R::FindPropertyOffset(cls, L"tiresDirt");
    g_fixesOff      = R::FindPropertyOffset(cls, L"tiresFixes");
    g_typesOff      = R::FindPropertyOffset(cls, L"tiresTypes");
    // NON-FATAL per field. A field that will not resolve prints its sentinel for the
    // life of the process; suppressing the whole line instead would make a resolve
    // failure look exactly like "the ATV never moved", which is the reading this
    // instrument exists to rule out.
    g_resolved = true;
    UE_LOGI("[ATVT] offsets: tires=%d durability=%d dirt=%d fixes=%d types=%d",
            g_tiresOff, g_durabilityOff, g_dirtOff, g_fixesOff, g_typesOff);
    return true;
}

}  // namespace

void Sample(void* atv, std::size_t idx, const wchar_t* key, bool ownsTick, std::uint32_t n) {
    if (!atv) return;
    if (!Resolve()) return;

    const int32_t tires = ReadBoolMask(atv, g_tiresOff);
    const int32_t types = ReadBytesPacked(atv, g_typesOff);
    float dur[kSlots]; ReadFloats(atv, g_durabilityOff, dur);
    float drt[kSlots]; ReadFloats(atv, g_dirtOff, drt);
    int32_t fix[kSlots]; ReadInts(atv, g_fixesOff, fix);

    UE_LOGI("[ATVT] n=%u i=%zu key='%ls' owns=%d tires=0x%X types=0x%X "
            "dur=(%.2f,%.2f,%.2f,%.2f) dirt=(%.2f,%.2f,%.2f,%.2f) fixes=(%d,%d,%d,%d)",
            n, idx, key ? key : L"?", ownsTick ? 1 : 0, tires, types,
            dur[0], dur[1], dur[2], dur[3],
            drt[0], drt[1], drt[2], drt[3],
            fix[0], fix[1], fix[2], fix[3]);
}

}  // namespace coop::atv_tire_probe
