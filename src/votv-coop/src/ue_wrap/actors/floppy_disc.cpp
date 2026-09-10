// ue_wrap/actors/floppy_disc.cpp -- see ue_wrap/actors/floppy_disc.h. Offsets resolved live via
// reflection; the Alpha 0.9.0-n fallbacks come from the CXX header dump.

#include "ue_wrap/actors/floppy_disc.h"

#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>

namespace ue_wrap::floppy_disc {
namespace {

namespace R = reflection;

using field_io::ReadFStringArrayField;
using field_io::WriteFStringArrayField;

void*   g_cls          = nullptr;  // prop_floppyDisc_C
int32_t g_offData      = -1;
int32_t g_offReadWrites = -1;
bool    g_resolved     = false;
uint64_t g_nextTryMs   = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved) return true;
    const uint64_t now = NowMs();
    if (now < g_nextTryMs) return false;
    g_nextTryMs = now + 1000;

    void* cls = R::FindClass(L"prop_floppyDisc_C");
    if (!cls) return false;  // loads on demand; the backoff above bounds the retry
    g_offData       = R::FindPropertyOffset(cls, L"Data");
    g_offReadWrites = R::FindPropertyOffset(cls, L"readWrites");
    if (g_offData < 0)       { UE_LOGW("floppy_disc: data offset -- fallback 0x368");       g_offData = 0x368; }
    if (g_offReadWrites < 0) { UE_LOGW("floppy_disc: readWrites offset -- fallback 0x37C"); g_offReadWrites = 0x37C; }
    g_cls = cls;
    g_resolved = true;
    UE_LOGI("floppy_disc: resolved (data=0x%X readWrites=0x%X)", g_offData, g_offReadWrites);
    return true;
}

bool IsDiscClass(void* cls) {
    if (!cls || !EnsureResolved()) return false;
    if (cls == g_cls) return true;
    void* base[1] = { g_cls };
    return R::IsDescendantOfAny(cls, base, 1);
}

bool ReadDiscContent(void* discActor, DiscContent& out) {
    if (!discActor || !EnsureResolved()) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(discActor);
    out.readWrites = *reinterpret_cast<const int32_t*>(p + g_offReadWrites);
    out.data       = ReadFStringArrayField(discActor, g_offData);
    return true;
}

bool WriteDiscContent(void* discActor, const DiscContent& in) {
    if (!discActor || !EnsureResolved()) return false;
    uint8_t* p = reinterpret_cast<uint8_t*>(discActor);
    *reinterpret_cast<int32_t*>(p + g_offReadWrites) = in.readWrites;
    return WriteFStringArrayField(discActor, g_offData, in.data);
}

void ResetCache() {
    g_cls = nullptr;
    g_offData = g_offReadWrites = -1;
    g_resolved = false;
    g_nextTryMs = 0;
}

}  // namespace ue_wrap::floppy_disc
