// ue_wrap/space_renderer.cpp -- see ue_wrap/space_renderer.h.
//
// Bytecode ground truth (research/bp_reflection/_spacerenderer_uber_full.txt):
//   @3819 BIND spawnSignal -> K2_SetTimerDelegate(delegate, rand(20,60), looping=false)
//     -- the self-re-arming roller. spawnSignal's body (@4010-4250) rolls a
//     random point in the coords-screen area and calls addSignal(MakeVector(
//     rx, ry, 0)) -- so addSignal's InVec IS the screen-space position. A
//     mirror passing the WIRE coords therefore places the row + widget at the
//     native position; only addSignal's internal rolls (type/strength/
//     frequency/spreads/polarity/objectName + the widget's lifetime pair)
//     need overwriting afterwards.
//   K2_ClearTimer(self, "spawnSignal") is the exact inverse of the
//     K2_SetTimerDelegate arm (UE4 ClearTimer builds the delegate from
//     object+fname); spawnSignal is the ONLY re-armer, so one kill silences
//     the roller for the instance's lifetime. Reflect-calling spawnSignal()
//     once rolls one signal AND re-arms the native loop (the restore path).

#include "ue_wrap/desk/space_renderer.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cstring>

namespace ue_wrap::space_renderer {
namespace {

namespace R = ue_wrap::reflection;

// Fstruct_signal_spawn member offsets (struct_signal_spawn.hpp; the member
// names are GUID-mangled, so the dump offsets are authoritative -- the
// door_box timeline-field precedent. Size 0x2C.)
constexpr int32_t kRow_coordinates    = 0x00;  // FVector
constexpr int32_t kRow_type           = 0x0C;  // int32
constexpr int32_t kRow_strength       = 0x10;  // float
constexpr int32_t kRow_frequency      = 0x14;  // float
constexpr int32_t kRow_freqSpread     = 0x18;  // float
constexpr int32_t kRow_polarity       = 0x1C;  // float
constexpr int32_t kRow_polSpread      = 0x20;  // float
constexpr int32_t kRow_objectName     = 0x24;  // FName (8)
constexpr int32_t kRowStride          = 0x2C;

struct TArrayView { uint8_t* data; int32_t num; int32_t max; };

void* g_cls = nullptr;
void* g_instance = nullptr;
int32_t g_instanceIdx = -1;

int32_t g_offSignals = -1;    // TArray<Fstruct_signal_spawn>
int32_t g_offSignalsA = -1;   // TArray<ui_signal_C*>
int32_t g_offMovement = -1;   // FVector2D movement -- the cursor glide integrator state
// (No coords/coords_rot/move_* offsets: dead bytecode -- the dish aim lives
// on the ui_coordinates widget, handled by ue_wrap::console_desk.)

void* g_uiSignalCls = nullptr;
int32_t g_offWidgetLifeTime = -1;     // ui_signal_C::LifeTime (the countdown divisor)
int32_t g_offWidgetMaxLifetime = -1;  // ui_signal_C::MaxLifetime
int32_t g_offWidgetAlpha = -1;        // ui_signal_C::Alpha (THE 1->0 expiry countdown)
int32_t g_offWidgetDirection = -1;    // ui_signal_C::Direction (catch-gate parity)
int32_t g_offWidgetDynmat = -1;       // ui_signal_C::dynmat (the dot's material instance)
void* g_dynmatSetScalarFn = nullptr;       // UMaterialInstanceDynamic::SetScalarParameterValue
void* g_widgetSetRenderScaleFn = nullptr;  // UWidget::SetRenderScale

void* g_addSignalFn = nullptr;
void* g_deleteSignalFn = nullptr;
void* g_spawnSignalFn = nullptr;
void* g_kismetSysCdo = nullptr;
void* g_clearTimerFn = nullptr;

std::chrono::steady_clock::time_point g_nextResolve{};
bool g_coreResolved = false;

void ResolvePass() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + std::chrono::seconds(2);

    if (!g_cls) g_cls = R::FindClass(L"spaceRenderer_C");
    if (!g_cls) return;
    if (g_offSignals < 0)   g_offSignals   = R::FindPropertyOffset(g_cls, L"signals");
    if (g_offSignalsA < 0)  g_offSignalsA  = R::FindPropertyOffset(g_cls, L"signals_a");
    if (g_offMovement < 0)  g_offMovement  = R::FindPropertyOffset(g_cls, L"movement");
    if (!g_addSignalFn)    g_addSignalFn    = R::FindFunction(g_cls, L"addSignal");
    if (!g_deleteSignalFn) g_deleteSignalFn = R::FindFunction(g_cls, L"deleteSignal");
    if (!g_spawnSignalFn)  g_spawnSignalFn  = R::FindFunction(g_cls, L"spawnSignal");

    if (!g_uiSignalCls) g_uiSignalCls = R::FindClass(L"ui_signal_C");
    if (g_uiSignalCls) {
        if (g_offWidgetLifeTime < 0)
            g_offWidgetLifeTime = R::FindPropertyOffset(g_uiSignalCls, L"LifeTime");
        if (g_offWidgetMaxLifetime < 0)
            g_offWidgetMaxLifetime = R::FindPropertyOffset(g_uiSignalCls, L"MaxLifetime");
        if (g_offWidgetAlpha < 0)
            g_offWidgetAlpha = R::FindPropertyOffset(g_uiSignalCls, L"Alpha");
        if (g_offWidgetDirection < 0)
            g_offWidgetDirection = R::FindPropertyOffset(g_uiSignalCls, L"Direction");
        if (g_offWidgetDynmat < 0)
            g_offWidgetDynmat = R::FindPropertyOffset(g_uiSignalCls, L"dynmat");
    }
    if (!g_dynmatSetScalarFn) {
        if (void* mc = R::FindClass(L"MaterialInstanceDynamic"))
            g_dynmatSetScalarFn = R::FindFunction(mc, L"SetScalarParameterValue");
    }
    if (!g_widgetSetRenderScaleFn) {
        if (void* wc = R::FindClass(L"Widget"))
            g_widgetSetRenderScaleFn = R::FindFunction(wc, L"SetRenderScale");
    }
    if (!g_kismetSysCdo) g_kismetSysCdo = R::FindClassDefaultObject(L"KismetSystemLibrary");
    if (g_kismetSysCdo && !g_clearTimerFn) {
        if (void* kc = R::FindClass(L"KismetSystemLibrary"))
            g_clearTimerFn = R::FindFunction(kc, L"K2_ClearTimer");
    }

    const bool core = g_offSignals >= 0 && g_offSignalsA >= 0 &&
                      g_addSignalFn && g_deleteSignalFn && g_spawnSignalFn &&
                      g_offWidgetLifeTime >= 0 && g_offWidgetMaxLifetime >= 0 &&
                      g_offWidgetAlpha >= 0 && g_offWidgetDirection >= 0;
    if (core && !g_coreResolved) {
        g_coreResolved = true;
        UE_LOGI("space_renderer: resolved (signals=0x%X signals_a=0x%X "
                "widget A/L/M/D=0x%X/0x%X/0x%X/0x%X clearTimer=%s)",
                g_offSignals, g_offSignalsA,
                g_offWidgetAlpha, g_offWidgetLifeTime, g_offWidgetMaxLifetime,
                g_offWidgetDirection,
                g_clearTimerFn ? "yes" : "NO (sweep-only suppression)");
    }
}

TArrayView* Rows(void* inst) {
    return reinterpret_cast<TArrayView*>(reinterpret_cast<uint8_t*>(inst) + g_offSignals);
}
TArrayView* Widgets(void* inst) {
    return reinterpret_cast<TArrayView*>(reinterpret_cast<uint8_t*>(inst) + g_offSignalsA);
}
uint8_t* RowAt(TArrayView* a, int32_t i) { return a->data + static_cast<size_t>(i) * kRowStride; }
void* WidgetAt(TArrayView* a, int32_t i) {
    return reinterpret_cast<void**>(a->data)[i];
}

template <class T>
T ReadAt(uint8_t* base, int32_t off) { T v; std::memcpy(&v, base + off, sizeof(T)); return v; }
template <class T>
void WriteAt(uint8_t* base, int32_t off, const T& v) { std::memcpy(base + off, &v, sizeof(T)); }

void ReadRow(uint8_t* rp, void* widget, SignalRow& out) {
    out.x = ReadAt<float>(rp, kRow_coordinates + 0);
    out.y = ReadAt<float>(rp, kRow_coordinates + 4);
    out.z = ReadAt<float>(rp, kRow_coordinates + 8);
    out.type = ReadAt<int32_t>(rp, kRow_type);
    out.strength = ReadAt<float>(rp, kRow_strength);
    out.frequency = ReadAt<float>(rp, kRow_frequency);
    out.frequencySpread = ReadAt<float>(rp, kRow_freqSpread);
    out.polarity = ReadAt<float>(rp, kRow_polarity);
    out.polaritySpread = ReadAt<float>(rp, kRow_polSpread);
    out.objectName = R::ToString(ReadAt<R::FName>(rp, kRow_objectName));
    if (widget && R::IsLive(widget)) {
        auto* w = reinterpret_cast<uint8_t*>(widget);
        out.alpha = ReadAt<float>(w, g_offWidgetAlpha);
        out.lifeTime = ReadAt<float>(w, g_offWidgetLifeTime);
        out.maxLifetime = ReadAt<float>(w, g_offWidgetMaxLifetime);
        out.direction = ReadAt<bool>(w, g_offWidgetDirection);
    }
}

// Overwrite a row's rolled fields with the wire values + push the wire
// lifetimes onto the paired widget. The POSITION is already native-correct
// (addSignal received the wire coords as InVec).
void OverwriteRow(uint8_t* rp, void* widget, const SignalRow& want) {
    WriteAt<float>(rp, kRow_coordinates + 0, want.x);
    WriteAt<float>(rp, kRow_coordinates + 4, want.y);
    WriteAt<float>(rp, kRow_coordinates + 8, want.z);
    WriteAt<int32_t>(rp, kRow_type, want.type);
    WriteAt<float>(rp, kRow_strength, want.strength);
    WriteAt<float>(rp, kRow_frequency, want.frequency);
    WriteAt<float>(rp, kRow_freqSpread, want.frequencySpread);
    WriteAt<float>(rp, kRow_polarity, want.polarity);
    WriteAt<float>(rp, kRow_polSpread, want.polaritySpread);
    WriteAt<R::FName>(rp, kRow_objectName,
                      ue_wrap::fname_utils::StringToFName(want.objectName));
    if (widget && R::IsLive(widget)) {
        auto* w = reinterpret_cast<uint8_t*>(widget);
        WriteAt<float>(w, g_offWidgetAlpha, want.alpha);
        WriteAt<float>(w, g_offWidgetLifeTime, want.lifeTime);
        WriteAt<float>(w, g_offWidgetMaxLifetime, want.maxLifetime);
        WriteAt<bool>(w, g_offWidgetDirection, want.direction);
    }
}

bool IdentityEq(uint8_t* rp, float x, float y, float z, float frequency) {
    return ReadAt<float>(rp, kRow_coordinates + 0) == x &&
           ReadAt<float>(rp, kRow_coordinates + 4) == y &&
           ReadAt<float>(rp, kRow_coordinates + 8) == z &&
           ReadAt<float>(rp, kRow_frequency) == frequency;
}

// After an ADD, correct the widget visuals Construct derived from addSignal's
// ROLLED values (phase-2 impl RE SS1.4: no spaceRenderer verb re-derives
// widget visuals from rows; the non-self-correcting pieces are the dynmat
// 'dir'/'pingSpeed' scalars and the RenderScale-from-strength). Kept rows
// never need this -- their visuals were built from their own wire add, and
// the host never mutates a rolled row's strength/direction.
void PushAddedWidgetVisuals(void* widget, const SignalRow& w) {
    if (!widget || !R::IsLive(widget)) return;
    if (g_offWidgetDynmat >= 0 && g_dynmatSetScalarFn) {
        void* dynmat = ReadAt<void*>(reinterpret_cast<uint8_t*>(widget), g_offWidgetDynmat);
        if (dynmat && R::IsLive(dynmat)) {
            auto setScalar = [&](const wchar_t* name, float v) {
                ue_wrap::ParamFrame f(g_dynmatSetScalarFn);
                if (!f.valid()) return;
                f.Set<R::FName>(L"ParameterName", ue_wrap::fname_utils::StringToFName(name));
                f.Set<float>(L"Value", v);
                ue_wrap::Call(dynmat, f);
            };
            setScalar(L"dir", w.direction ? 1.f : 0.f);
            setScalar(L"pingSpeed", w.lifeTime / 180.f);
        }
    }
    if (g_widgetSetRenderScaleFn) {
        // SS1.4 step 10: SetRenderScale(MakeVector2D(strength, strength)).
        ue_wrap::ParamFrame f(g_widgetSetRenderScaleFn);
        if (f.valid()) {
            struct { float X, Y; } sc{ w.strength, w.strength };
            if (f.SetRaw(L"Scale", &sc, sizeof(sc)))
                ue_wrap::Call(widget, f);
        }
    }
}

bool DeleteByWidget(void* inst, void* widget) {
    if (!g_deleteSignalFn || !widget) return false;
    ue_wrap::ParamFrame f(g_deleteSignalFn);
    if (!f.valid()) return false;
    f.Set<void*>(L"ItemToFind", widget);
    return ue_wrap::Call(inst, f);
}

}  // namespace

bool EnsureResolved() {
    ResolvePass();
    return g_coreResolved;
}

void* Instance() {
    if (g_instance && R::IsLiveByIndex(g_instance, g_instanceIdx)) return g_instance;
    g_instance = nullptr;
    if (!g_cls) return nullptr;
    for (void* obj : R::FindObjectsByClass(L"spaceRenderer_C")) {
        if (obj && R::IsLive(obj)) {
            g_instance = obj;
            g_instanceIdx = R::InternalIndexOf(obj);
            break;
        }
    }
    return g_instance;
}

bool ReadSignals(std::vector<SignalRow>& out) {
    out.clear();
    void* inst = Instance();
    if (!inst || !g_coreResolved) return false;
    TArrayView* rows = Rows(inst);
    TArrayView* wids = Widgets(inst);
    if (rows->num < 0 || rows->num > 256) return false;  // sanity
    int32_t n = rows->num;
    if (wids->num != rows->num) {
        static bool sWarned = false;
        if (!sWarned) {
            sWarned = true;
            UE_LOGW("space_renderer: signals(%d) / signals_a(%d) length mismatch -- "
                    "pairing by min", rows->num, wids->num);
        }
        n = rows->num < wids->num ? rows->num : wids->num;
    }
    out.reserve(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        SignalRow r;
        ReadRow(RowAt(rows, i), WidgetAt(wids, i), r);
        out.push_back(std::move(r));
    }
    return true;
}

bool ApplySignalSet(const std::vector<SignalRow>& want, ApplyStats& stats) {
    void* inst = Instance();
    if (!inst || !g_coreResolved) return false;

    // Pass 1: delete local rows not in `want` (collect widgets first --
    // deleteSignal compacts both arrays, invalidating indices).
    std::vector<void*> victims;
    {
        TArrayView* rows = Rows(inst);
        TArrayView* wids = Widgets(inst);
        const int32_t n = rows->num < wids->num ? rows->num : wids->num;
        for (int32_t i = 0; i < n; ++i) {
            uint8_t* rp = RowAt(rows, i);
            bool keep = false;
            for (const auto& w : want)
                if (IdentityEq(rp, w.x, w.y, w.z, w.frequency)) { keep = true; break; }
            if (!keep) victims.push_back(WidgetAt(wids, i));
        }
    }
    for (void* v : victims)
        if (DeleteByWidget(inst, v)) ++stats.removed;

    // Pass 2: for each wanted row -- overwrite the existing local twin (and
    // refresh its widget lifetimes), or addSignal + overwrite if missing.
    for (const auto& w : want) {
        TArrayView* rows = Rows(inst);
        TArrayView* wids = Widgets(inst);
        int32_t n = rows->num < wids->num ? rows->num : wids->num;
        int32_t found = -1;
        for (int32_t i = 0; i < n; ++i)
            if (IdentityEq(RowAt(rows, i), w.x, w.y, w.z, w.frequency)) { found = i; break; }
        if (found >= 0) {
            OverwriteRow(RowAt(rows, found), WidgetAt(wids, found), w);
            ++stats.kept;
            continue;
        }
        // addSignal(InVec = the wire coords) -- the native builder places the
        // row + widget at the right position; we then overwrite its rolls.
        ue_wrap::ParamFrame f(g_addSignalFn);
        if (!f.valid()) return false;
        struct { float X, Y, Z; } vec{ w.x, w.y, w.z };
        if (!f.SetRaw(L"InVec", &vec, sizeof(vec))) return false;
        const int32_t nBefore = n;
        if (!ue_wrap::Call(inst, f)) continue;
        rows = Rows(inst);
        wids = Widgets(inst);
        n = rows->num < wids->num ? rows->num : wids->num;
        // Audit C-1: the dispatch succeeding does NOT prove addSignal appended
        // (a BP-internal cap / allocation no-op leaves the count unchanged);
        // overwriting n-1 then would corrupt the LAST EXISTING row. Require
        // exactly +1 growth before touching the tail.
        if (n != nBefore + 1) {
            UE_LOGW("space_renderer: addSignal did not append (before=%d after=%d) -- "
                    "skipping overwrite for this row", nBefore, n);
            continue;
        }
        // The new row is the appended tail.
        OverwriteRow(RowAt(rows, n - 1), WidgetAt(wids, n - 1), w);
        PushAddedWidgetVisuals(WidgetAt(wids, n - 1), w);
        ++stats.added;
    }
    return true;
}

bool RemoveSignalByIdentity(float x, float y, float z, float frequency) {
    void* inst = Instance();
    if (!inst || !g_coreResolved) return false;
    TArrayView* rows = Rows(inst);
    TArrayView* wids = Widgets(inst);
    const int32_t n = rows->num < wids->num ? rows->num : wids->num;
    for (int32_t i = 0; i < n; ++i) {
        if (IdentityEq(RowAt(rows, i), x, y, z, frequency))
            return DeleteByWidget(inst, WidgetAt(wids, i));
    }
    return false;
}

bool KillClientSpawnTimer() {
    void* inst = Instance();
    if (!inst) return false;
    if (!g_clearTimerFn || !g_kismetSysCdo) {
        UE_LOGW("space_renderer: K2_ClearTimer unresolved -- roller suppression falls "
                "back to the per-poll sweep");
        return false;
    }
    // K2_ClearTimer(Object, FunctionName): builds the delegate from
    // object+fname internally -- the exact inverse of the BP's
    // K2_SetTimerDelegate({self, spawnSignal}) arm (uber @3880).
    ue_wrap::ParamFrame f(g_clearTimerFn);
    if (!f.valid()) return false;
    f.Set<void*>(L"Object", inst);
    const wchar_t* fn = L"spawnSignal";
    struct { const wchar_t* data; int32_t num; int32_t max; } fs{ fn, 12, 12 };
    if (!f.SetRaw(L"FunctionName", &fs, sizeof(fs))) return false;
    return ue_wrap::Call(g_kismetSysCdo, f);
}

bool ZeroMovement() {
    // v115 cursor mirror: kill a residual local glide when a REMOTE stream
    // takes cursor authority (qf R5 Q4 -- the integrator adds `movement` to
    // the cursor every tick with NO focus gate, so a stale glide co-writes
    // against the wire stream). `movement` is a raw EX_Let-written Vector2D
    // (NOT setter-managed) -- the same write class as WriteCursorOnly.
    void* inst = Instance();
    if (!inst || g_offMovement < 0) return false;
    float* v = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(inst) + g_offMovement);
    v[0] = 0.0f;
    v[1] = 0.0f;
    return true;
}

bool RestoreRoller() {
    void* inst = Instance();
    if (!inst || !g_spawnSignalFn) return false;
    // One native roll + the BP's own 20-60 s re-arm (uber @3819).
    ue_wrap::ParamFrame f(g_spawnSignalFn);
    if (!f.valid()) return false;
    return ue_wrap::Call(inst, f);
}

}  // namespace ue_wrap::space_renderer
