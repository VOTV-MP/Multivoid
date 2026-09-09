// ue_wrap/desk/phys_mods.cpp -- see ue_wrap/desk/phys_mods.h.

#include "ue_wrap/desk/phys_mods.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cstring>

namespace ue_wrap::phys_mods {
namespace {

namespace R = ue_wrap::reflection;
using Clock = std::chrono::steady_clock;

// class-level (persist across level reloads)
void*   g_deskCls = nullptr;
int32_t g_offPhysMods = -1;      // TArray<byte>, resolved live by property name
void*   g_updPhysModsFn = nullptr;
void*   g_moduleBaseCls = nullptr;  // Aprop_physModule_C
void*   g_libCdo = nullptr;         // lib_C CDO (physModToActor is a static-lib call)
void*   g_physModToActorFn = nullptr;
bool    g_resolved = false;
Clock::time_point g_nextTry{};

// byte -> class probe cache (built lazily per byte; enum range is small).
// index = byte value; kProbeMax bounds the probe domain.
constexpr int kProbeMax = 64;
void* g_classForByte[kProbeMax] = {};
bool  g_probed[kProbeMax] = {};

// UE TArray layout: data ptr @0, Num @8, Max @12.
uint8_t* ArrayData(void* desk, int32_t& num) {
    auto* base = reinterpret_cast<uint8_t*>(desk) + g_offPhysMods;
    num = *reinterpret_cast<int32_t*>(base + 8);
    return *reinterpret_cast<uint8_t**>(base);
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved) return true;
    const auto now = Clock::now();
    if (now < g_nextTry) return false;
    g_nextTry = now + std::chrono::seconds(1);

    if (!ue_wrap::console_desk::EnsureResolved()) return false;
    if (!g_deskCls) g_deskCls = R::FindClass(L"analogDScreenTest_C");
    if (!g_deskCls) return false;
    if (g_offPhysMods < 0) g_offPhysMods = R::FindPropertyOffset(g_deskCls, L"physMods");
    if (!g_updPhysModsFn) g_updPhysModsFn = R::FindFunction(g_deskCls, L"updPhysMods");
    if (!g_moduleBaseCls) g_moduleBaseCls = R::FindClass(L"prop_physModule_C");
    if (!g_libCdo) g_libCdo = R::FindClassDefaultObject(L"lib_C");
    if (!g_physModToActorFn) {
        if (void* libCls = R::FindClass(L"lib_C"))
            g_physModToActorFn = R::FindFunction(libCls, L"physModToActor");
    }
    if (g_offPhysMods < 0 || !g_updPhysModsFn || !g_moduleBaseCls || !g_libCdo ||
        !g_physModToActorFn) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            UE_LOGW("phys_mods: resolve incomplete (off=%d upd=%d base=%d lib=%d fn=%d) -- "
                    "backoff retry (log-once)",
                    g_offPhysMods >= 0 ? 1 : 0, g_updPhysModsFn ? 1 : 0,
                    g_moduleBaseCls ? 1 : 0, g_libCdo ? 1 : 0, g_physModToActorFn ? 1 : 0);
        }
        return false;
    }
    g_resolved = true;
    UE_LOGI("phys_mods: resolved (physMods@0x%X + updPhysMods + prop_physModule_C + "
            "lib.physModToActor)", g_offPhysMods);
    return true;
}

bool ReadArray(uint8_t out[kSlots]) {
    void* d = ue_wrap::console_desk::Instance();
    if (!d || !g_resolved) return false;
    int32_t num = 0;
    uint8_t* data = ArrayData(d, num);
    if (!data || num < 0) return false;
    std::memset(out, 0, kSlots);
    const int n = num < kSlots ? num : kSlots;
    std::memcpy(out, data, static_cast<size_t>(n));
    return true;
}

bool WriteArray(const uint8_t in[kSlots]) {
    void* d = ue_wrap::console_desk::Instance();
    if (!d || !g_resolved) return false;
    int32_t num = 0;
    uint8_t* data = ArrayData(d, num);
    if (!data || num < kSlots) {
        // The CDO array is fixed-12; a smaller live Num means the desk isn't
        // the shape we measured -- refuse rather than realloc engine memory.
        UE_LOGW("phys_mods: WriteArray refused (Num=%d < %d)", num, kSlots);
        return false;
    }
    std::memcpy(data, in, kSlots);
    return true;
}

bool CallUpdPhysMods() {
    void* d = ue_wrap::console_desk::Instance();
    if (!d || !g_updPhysModsFn) return false;
    ue_wrap::ParamFrame f(g_updPhysModsFn);
    return f.valid() && ue_wrap::Call(d, f);
}

bool IsModuleClass(void* cls) {
    if (!cls || !g_moduleBaseCls) return false;
    if (cls == g_moduleBaseCls) return true;
    void* base[1] = { g_moduleBaseCls };
    return R::IsDescendantOfAny(cls, base, 1);
}

void* ClassForByte(uint8_t byte) {
    if (!g_resolved || byte >= kProbeMax) return nullptr;
    if (g_probed[byte]) return g_classForByte[byte];
    ue_wrap::ParamFrame f(g_physModToActorFn);
    if (!f.valid()) return nullptr;  // transient -- do NOT negative-cache (audit MINOR-2)
    f.Set<uint8_t>(L"physmod", byte);
    // __WorldContext: the lib CDO itself suffices for a pure class-map call
    // (the function only reads a static mapping -- bytecode-verified no world use
    // beyond the K2 boilerplate; passing the CDO matches the votv_lib pattern).
    f.Set<void*>(L"__WorldContext", g_libCdo);
    if (!ue_wrap::Call(g_libCdo, f)) return nullptr;  // transient call failure -- retry next event
    void* cls = f.Get<void*>(L"Actor");
    g_probed[byte] = true;  // latch only on a SUCCESSFUL call (a null result = a
    g_classForByte[byte] = cls;  // legit unmapped byte, cached as a negative)
    return cls;
}

uint8_t ByteForClass(void* cls) {
    if (!cls) return 0;
    for (int b = 1; b < kProbeMax; ++b) {
        if (ClassForByte(static_cast<uint8_t>(b)) == cls) return static_cast<uint8_t>(b);
    }
    return 0;
}

void ResetCache() {
    // Class-level resolves persist; nothing instance-cached here (the desk
    // instance rides console_desk's own cache).
}

}  // namespace ue_wrap::phys_mods
