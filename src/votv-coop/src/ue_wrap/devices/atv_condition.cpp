// ue_wrap/devices/atv_condition.cpp -- see header. Layout facts and the write-in-place
// contract are documented there; this file is the mechanics.

#include "ue_wrap/devices/atv_condition.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <cstring>

namespace ue_wrap::atv_condition {

namespace R = ue_wrap::reflection;

namespace {

constexpr int kSlots = 4;

bool g_resolved = false;
bool g_tried    = false;

// Property offsets on ATV_C. -1 = unresolved.
int32_t g_offTires = -1, g_offDur = -1, g_offDirtArr = -1, g_offFixes = -1, g_offTypes = -1;
int32_t g_offBodyDirt = -1, g_offFuel = -1, g_offHealth = -1;
int32_t g_offHasSpare = -1, g_offSpareDur = -1, g_offSpareDirt = -1, g_offSpareFixes = -1;
int32_t g_offSkip = -1;

// The four reducer UFunctions, resolved once (R::FindFunction has NO result cache -- the
// browser perf lane measured a full GUObjectArray walk per call; never resolve in a hot path).
void* g_fnUpdTires = nullptr;
void* g_fnUpdDirt = nullptr;
void* g_fnUpdSpare = nullptr;
void* g_fnUpdHealth = nullptr;

// FScriptArray head = { void* Data; int32 Num; int32 Max }. Returns Num (clamped view), or -1
// when the head reads as garbage -- the caller's all-or-nothing contract needs "unreadable"
// kept distinct from "empty" (the tire probe's own lesson).
int32_t ArrayHead(void* obj, int32_t off, void** dataOut) {
    *dataOut = nullptr;
    if (!obj || off < 0) return -1;
    uint8_t* arr = reinterpret_cast<uint8_t*>(obj) + off;
    void* data = *reinterpret_cast<void**>(arr);
    const int32_t num = *reinterpret_cast<int32_t*>(arr + 8);
    if (!data || num < 0 || num > 64) return -1;
    *dataOut = data;
    return num;
}

template <class T>
int8_t ClampI8(T v) {
    if (v < static_cast<T>(-128)) return -128;
    if (v > static_cast<T>(127)) return 127;
    return static_cast<int8_t>(v);
}

}  // namespace

bool Resolve() {
    if (g_resolved) return true;
    if (g_tried) return false;
    g_tried = true;
    void* cls = R::FindClass(L"ATV_C");
    if (!cls) { UE_LOGW("atv_condition: ATV_C not resident at resolve"); return false; }
    g_offTires      = R::FindPropertyOffset(cls, L"tires");
    g_offDur        = R::FindPropertyOffset(cls, L"tiresDurability");
    g_offDirtArr    = R::FindPropertyOffset(cls, L"tiresDirt");
    g_offFixes      = R::FindPropertyOffset(cls, L"tiresFixes");
    g_offTypes      = R::FindPropertyOffset(cls, L"tiresTypes");
    g_offBodyDirt   = R::FindPropertyOffset(cls, L"dirt");
    g_offFuel       = R::FindPropertyOffset(cls, L"fuel");
    g_offHealth     = R::FindPropertyOffset(cls, L"health");
    g_offHasSpare   = R::FindPropertyOffset(cls, L"hasSpareTire");
    g_offSpareDur   = R::FindPropertyOffset(cls, L"spareTire_durability");
    g_offSpareDirt  = R::FindPropertyOffset(cls, L"spareTire_dirt");
    g_offSpareFixes = R::FindPropertyOffset(cls, L"spareTire_fixes");
    g_offSkip       = R::FindPropertyOffset(cls, L"skipTireUpdate");
    g_fnUpdTires  = R::FindFunction(cls, L"updTires");
    g_fnUpdDirt   = R::FindFunction(cls, L"updDirt");
    g_fnUpdSpare  = R::FindFunction(cls, L"updSpareTire");
    g_fnUpdHealth = R::FindFunction(cls, L"updHealth");
    const bool offsOk = g_offTires >= 0 && g_offDur >= 0 && g_offDirtArr >= 0 &&
                        g_offFixes >= 0 && g_offTypes >= 0 && g_offBodyDirt >= 0 &&
                        g_offFuel >= 0 && g_offHealth >= 0 && g_offHasSpare >= 0 &&
                        g_offSpareDur >= 0 && g_offSpareDirt >= 0 && g_offSpareFixes >= 0 &&
                        g_offSkip >= 0;
    const bool fnsOk = g_fnUpdTires && g_fnUpdDirt && g_fnUpdSpare && g_fnUpdHealth;
    g_resolved = offsOk && fnsOk;
    if (!g_resolved) {
        UE_LOGE("atv_condition: resolve INCOMPLETE (offsets ok=%d fns ok=%d) -- the condition "
                "lane stays inert this session; producers ship tiresValid=0",
                offsOk ? 1 : 0, fnsOk ? 1 : 0);
    }
    return g_resolved;
}

bool Resolved() { return g_resolved; }

bool Read(void* atv, Snapshot& out) {
    out = Snapshot{};
    if (!g_resolved || !atv) return false;
    void* d = nullptr;
    // All-or-nothing: any unreadable array head fails the WHOLE read (tiresValid contract).
    int32_t n = ArrayHead(atv, g_offTires, &d);
    if (n < 0) return false;
    const int32_t nt = n < kSlots ? n : kSlots;
    for (int32_t i = 0; i < nt; ++i)
        if (reinterpret_cast<uint8_t*>(d)[i]) out.mask |= static_cast<uint8_t>(1u << i);
    n = ArrayHead(atv, g_offDur, &d);
    if (n < 0) return false;
    for (int32_t i = 0, e = n < kSlots ? n : kSlots; i < e; ++i)
        out.dur[i] = reinterpret_cast<float*>(d)[i];
    n = ArrayHead(atv, g_offDirtArr, &d);
    if (n < 0) return false;
    for (int32_t i = 0, e = n < kSlots ? n : kSlots; i < e; ++i)
        out.dirt[i] = reinterpret_cast<float*>(d)[i];
    n = ArrayHead(atv, g_offFixes, &d);
    if (n < 0) return false;
    for (int32_t i = 0, e = n < kSlots ? n : kSlots; i < e; ++i)
        out.fixes[i] = ClampI8(reinterpret_cast<int32_t*>(d)[i]);
    n = ArrayHead(atv, g_offTypes, &d);
    if (n < 0) return false;
    for (int32_t i = 0, e = n < kSlots ? n : kSlots; i < e; ++i)
        out.types[i] = reinterpret_cast<uint8_t*>(d)[i];
    uint8_t* base = reinterpret_cast<uint8_t*>(atv);
    out.bodyDirt   = *reinterpret_cast<float*>(base + g_offBodyDirt);
    out.fuel       = *reinterpret_cast<float*>(base + g_offFuel);
    out.health     = *reinterpret_cast<float*>(base + g_offHealth);
    out.spareDur   = *reinterpret_cast<float*>(base + g_offSpareDur);
    out.spareDirt  = *reinterpret_cast<float*>(base + g_offSpareDirt);
    out.spareFixes = ClampI8(*reinterpret_cast<int32_t*>(base + g_offSpareFixes));
    out.hasSpare   = *(base + g_offHasSpare) != 0;
    return true;
}

bool WriteAccumulators(void* atv, const Snapshot& s) {
    if (!g_resolved || !atv) return false;
    void* d = nullptr;
    int32_t n = ArrayHead(atv, g_offDur, &d);
    if (n >= 0)
        for (int32_t i = 0, e = n < kSlots ? n : kSlots; i < e; ++i)
            reinterpret_cast<float*>(d)[i] = s.dur[i];
    n = ArrayHead(atv, g_offDirtArr, &d);
    if (n >= 0)
        for (int32_t i = 0, e = n < kSlots ? n : kSlots; i < e; ++i)
            reinterpret_cast<float*>(d)[i] = s.dirt[i];
    n = ArrayHead(atv, g_offFixes, &d);
    if (n >= 0)
        for (int32_t i = 0, e = n < kSlots ? n : kSlots; i < e; ++i)
            reinterpret_cast<int32_t*>(d)[i] = s.fixes[i];  // sign-extends int8 -> int32
    n = ArrayHead(atv, g_offTypes, &d);
    if (n >= 0)
        for (int32_t i = 0, e = n < kSlots ? n : kSlots; i < e; ++i)
            reinterpret_cast<uint8_t*>(d)[i] = s.types[i];
    uint8_t* base = reinterpret_cast<uint8_t*>(atv);
    *reinterpret_cast<float*>(base + g_offBodyDirt)  = s.bodyDirt;
    *reinterpret_cast<float*>(base + g_offFuel)      = s.fuel;
    *reinterpret_cast<float*>(base + g_offHealth)    = s.health;
    *reinterpret_cast<float*>(base + g_offSpareDur)  = s.spareDur;
    *reinterpret_cast<float*>(base + g_offSpareDirt) = s.spareDirt;
    *reinterpret_cast<int32_t*>(base + g_offSpareFixes) = s.spareFixes;
    return true;
}

bool WritePresence(void* atv, uint8_t mask, bool hasSpare) {
    if (!g_resolved || !atv) return false;
    void* d = nullptr;
    const int32_t n = ArrayHead(atv, g_offTires, &d);
    if (n >= 0)
        for (int32_t i = 0, e = n < kSlots ? n : kSlots; i < e; ++i)
            reinterpret_cast<uint8_t*>(d)[i] = (mask >> i) & 1u;
    *(reinterpret_cast<uint8_t*>(atv) + g_offHasSpare) = hasSpare ? 1 : 0;
    return true;
}

bool ReadSkipTireUpdate(void* atv, bool& flagOut) {
    if (!g_resolved || !atv) return false;
    flagOut = *(reinterpret_cast<uint8_t*>(atv) + g_offSkip) != 0;
    return true;
}

bool CallVerb(void* atv, Verb v) {
    if (!g_resolved || !atv) return false;
    void* fn = nullptr;
    switch (v) {
        case Verb::UpdTires:     fn = g_fnUpdTires;  break;
        case Verb::UpdDirt:      fn = g_fnUpdDirt;   break;
        case Verb::UpdSpareTire: fn = g_fnUpdSpare;  break;
        case Verb::UpdHealth:    fn = g_fnUpdHealth; break;
    }
    if (!fn) return false;
    ue_wrap::ParamFrame frame(fn);   // all four are no-param self-calls
    if (!frame.valid()) return false;
    return ue_wrap::Call(atv, frame);
}

}  // namespace ue_wrap::atv_condition
