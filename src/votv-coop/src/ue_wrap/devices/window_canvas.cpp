// ue_wrap/devices/window_canvas.cpp -- see ue_wrap/devices/window_canvas.h. Engine access for the
// base's bay window render target (Ad_window_C). Every offset is resolved by name from the live
// classes; the bytecode facts this relies on are in the header.

#include "ue_wrap/devices/window_canvas.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/gc_pin.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace ue_wrap::window_canvas {
namespace {

namespace R = reflection;

// ---- core: the window and the draw (resolved together; published through g_resolved) ----
std::atomic<bool> g_resolved{false};
void*   g_winCls      = nullptr;  // d_window_C
int32_t g_offA        = -1;       // FVector2D A  -- the last dab's top-left corner
int32_t g_offCanv     = -1;       // UCanvas* canv -- the world's shared render-target canvas
int32_t g_cvByte      = -1;       // bool cv      -- a stroke session is open
uint8_t g_cvMask      = 0;
void*   g_canvasEvent = nullptr;  // d_window_C::Canvas -- opens/joins the session, re-arms its close
void*   g_drawFn      = nullptr;  // UCanvas::K2_DrawMaterial

template <class T>
T ReadField(void* obj, int32_t off) {
    T v{};
    std::memcpy(&v, static_cast<const char*>(obj) + off, sizeof(T));
    return v;
}

// ---- lazily resolved: sponge brush, material parameters, the signal-playback gate ----
void*   g_spongeCls     = nullptr;  // prop_sponge_C (prop_mop_C derives from it)
int32_t g_offSize       = -1;       // float size
int32_t g_offStrength   = -1;       // float strength
int32_t g_offSoap       = -1;       // float soapAmount
int32_t g_offDynmat     = -1;       // MaterialInstanceDynamic* dynmat
int32_t g_offDrawmat    = -1;       // MaterialInterface* drawmat (the brush parent material)
void*   g_playerCls     = nullptr;
int32_t g_offHolding    = -1;       // mainPlayer_C::holding_actor -- the item in hand
void*   g_setScalarFn   = nullptr;  // MaterialInstanceDynamic::SetScalarParameterValue
void*   g_getScalarFn   = nullptr;  // MaterialInstanceDynamic::K2_GetScalarParameterValue
void*   g_matLibCdo     = nullptr;  // Default__KismetMaterialLibrary
void*   g_createMidFn   = nullptr;  // KismetMaterialLibrary::CreateDynamicMaterialInstance
int32_t g_offPanels     = -1;       // mainGamemode_C::analogPanels
void*   g_panelsCls     = nullptr;  // class the isPlayingSignal offset below was resolved on
int32_t g_playingByte   = -1;
uint8_t g_playingMask   = 0;
bool    g_drawSigChecked = false;
bool    g_drawSigOk      = false;

ue_wrap::GcPin g_brush;  // our brush MaterialInstanceDynamic (outer = transient package)

bool EnsureSponge() {
    if (g_spongeCls) return true;
    void* cls = R::FindClass(L"prop_sponge_C");
    if (!cls) return false;
    g_offSize     = R::FindPropertyOffset(cls, L"size");
    g_offStrength = R::FindPropertyOffset(cls, L"strength");
    g_offSoap     = R::FindPropertyOffset(cls, L"soapAmount");
    g_offDynmat   = R::FindPropertyOffset(cls, L"dynmat");
    g_offDrawmat  = R::FindPropertyOffset(cls, L"drawmat");
    g_spongeCls = cls;
    UE_LOGI("window_canvas: sponge resolved size@0x%X strength@0x%X soap@0x%X dynmat@0x%X drawmat@0x%X",
            g_offSize, g_offStrength, g_offSoap, g_offDynmat, g_offDrawmat);
    return true;
}

bool EnsureMaterialFns() {
    if (g_setScalarFn && g_getScalarFn) return true;
    void* mid = R::FindClass(L"MaterialInstanceDynamic");
    if (!mid) return false;
    g_setScalarFn = R::FindFunction(mid, L"SetScalarParameterValue");
    g_getScalarFn = R::FindFunction(mid, L"K2_GetScalarParameterValue");
    return g_setScalarFn && g_getScalarFn;
}

// The two parameter names, converted once. StringToFName DISPATCHES ProcessEvent, so converting a
// literal per call would put an engine dispatch in front of every scalar read and write -- four per
// dab replayed. The drone's dust parameter is cached the same way and for the same reason.
R::FName NameOnce(R::FName& slot, const wchar_t* name) {
    if (slot.ComparisonIndex == 0 && slot.Number == 0) slot = fname_utils::StringToFName(name);
    return slot;
}
R::FName g_nOpac{0, 0};
R::FName g_nCol{0, 0};

bool SetScalar(void* mid, R::FName name, float value) {
    ParamFrame f(g_setScalarFn);
    if (!f.valid()) return false;
    f.Set<R::FName>(L"ParameterName", name);
    f.Set<float>(L"Value", value);
    return Call(mid, f);
}

bool GetScalar(void* mid, R::FName name, float& out) {
    ParamFrame f(g_getScalarFn);
    if (!f.valid() || f.ParamOffset(L"ReturnValue") < 0) return false;
    f.Set<R::FName>(L"ParameterName", name);
    if (!Call(mid, f)) return false;
    out = f.Get<float>(L"ReturnValue");
    return true;
}

// The brush parent material: the sponge class default's drawmat, else the asset by name.
void* FindBrushParent() {
    if (EnsureSponge() && g_offDrawmat >= 0) {
        if (void* cdo = R::FindClassDefaultObject(L"prop_sponge_C")) {
            void* m = ReadField<void*>(cdo, g_offDrawmat);
            if (m && R::IsLive(m)) return m;
        }
    }
    return R::FindObject(L"mat_cleanBrush_Inst", L"MaterialInstanceConstant");
}

void* EnsureBrush(void* worldContext) {
    if (g_brush.Held() && R::IsLive(g_brush.Raw())) return g_brush.Raw();
    if (!g_matLibCdo) g_matLibCdo = R::FindClassDefaultObject(L"KismetMaterialLibrary");
    if (!g_createMidFn) {
        if (void* lib = R::FindClass(L"KismetMaterialLibrary"))
            g_createMidFn = R::FindFunction(lib, L"CreateDynamicMaterialInstance");
    }
    void* parent = FindBrushParent();
    if (!g_matLibCdo || !g_createMidFn || !parent) return nullptr;
    ParamFrame f(g_createMidFn);
    if (!f.valid() || f.ParamOffset(L"Parent") < 0 || f.ParamOffset(L"ReturnValue") < 0) return nullptr;
    if (f.ParamOffset(L"WorldContextObject") >= 0) f.Set<void*>(L"WorldContextObject", worldContext);
    f.Set<void*>(L"Parent", parent);
    if (!Call(g_matLibCdo, f)) return nullptr;
    void* mid = f.Get<void*>(L"ReturnValue");
    if (!mid || !g_brush.Pin(mid)) return nullptr;
    UE_LOGI("window_canvas: brush material instance %p created from %p", mid, parent);
    return mid;
}

// K2_DrawMaterial is set up by name; refuse (once, loudly) if the engine's signature differs.
bool DrawSignatureOk() {
    if (g_drawSigChecked) return g_drawSigOk;
    g_drawSigChecked = true;
    ParamFrame f(g_drawFn);
    const wchar_t* const names[] = {L"RenderMaterial", L"ScreenPosition", L"ScreenSize",
                                    L"CoordinatePosition", L"CoordinateSize", L"Rotation", L"PivotPoint"};
    g_drawSigOk = f.valid();
    for (const wchar_t* n : names) {
        if (f.ParamOffset(n) < 0) {
            UE_LOGE("window_canvas: UCanvas::K2_DrawMaterial has no parameter '%ls' -- refusing to draw", n);
            g_drawSigOk = false;
        }
    }
    return g_drawSigOk;
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;
    void* winCls = R::FindClass(L"d_window_C");
    if (!winCls) return false;
    const int32_t offA    = R::FindPropertyOffset(winCls, L"A");
    const int32_t offCanv = R::FindPropertyOffset(winCls, L"canv");
    int32_t cvByte = -1;
    uint8_t cvMask = 0;
    const bool cvOk = R::FindBoolProperty(winCls, L"cv", cvByte, cvMask);
    void* canvasEvent = R::FindFunction(winCls, L"Canvas");
    void* canvasCls = R::FindClass(L"Canvas");
    void* drawFn = canvasCls ? R::FindFunction(canvasCls, L"K2_DrawMaterial") : nullptr;
    if (offA < 0 || offCanv < 0 || !cvOk || !canvasEvent || !drawFn) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            UE_LOGW("window_canvas: d_window_C incomplete (A@%d canv@%d cv=%d Canvas=%p K2_DrawMaterial=%p)",
                    offA, offCanv, cvOk ? 1 : 0, canvasEvent, drawFn);
        }
        return false;
    }
    g_winCls = winCls;
    g_offA = offA;
    g_offCanv = offCanv;
    g_cvByte = cvByte;
    g_cvMask = cvMask;
    g_canvasEvent = canvasEvent;
    g_drawFn = drawFn;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("window_canvas: resolved d_window_C=%p A@0x%X canv@0x%X cv@0x%X/0x%02X Canvas=%p K2_DrawMaterial=%p",
            winCls, offA, offCanv, cvByte, cvMask, canvasEvent, drawFn);
    return true;
}

bool IsWindow(void* obj) {
    if (!obj || !g_resolved.load(std::memory_order_acquire)) return false;
    void* cls = R::ClassOf(obj);
    void* bases[1] = {g_winCls};
    return cls && R::IsDescendantOfAny(cls, bases, 1);
}

void* DrawMaterialFn() {
    return g_resolved.load(std::memory_order_acquire) ? g_drawFn : nullptr;
}

bool IsStrokeDraw(void* sourceObject) {
    if (!sourceObject || !g_resolved.load(std::memory_order_acquire)) return false;
    if (R::ClassOf(sourceObject) != g_winCls) return false;
    const uint8_t b = ReadField<uint8_t>(sourceObject, g_cvByte);
    return (b & g_cvMask) != 0;
}

bool ReadStrokeCorner(void* window, float& x, float& y) {
    if (!window || !g_resolved.load(std::memory_order_acquire)) return false;
    const FVector2D a = ReadField<FVector2D>(window, g_offA);
    x = a.X;
    y = a.Y;
    return true;
}

bool ReadHeldBrush(void* mainPlayer, float& size, float& opac, float& col) {
    if (!mainPlayer || !R::IsLive(mainPlayer) || !EnsureSponge()) return false;
    if (g_playerCls != R::ClassOf(mainPlayer)) {
        g_playerCls = R::ClassOf(mainPlayer);
        g_offHolding = R::FindPropertyOffset(g_playerCls, L"holding_actor");
    }
    if (g_offHolding < 0 || g_offSize < 0) return false;
    void* held = ReadField<void*>(mainPlayer, g_offHolding);
    if (!held || !R::IsLive(held)) return false;
    void* bases[1] = {g_spongeCls};
    if (!R::IsDescendantOfAny(R::ClassOf(held), bases, 1)) return false;

    size = ReadField<float>(held, g_offSize);
    // The sponge rewrote its material's opac right before the dab; read it back. If the material
    // cannot be read, fall back to the held-sponge formula: 0.4 x strength, doubled with soap.
    void* mid = g_offDynmat >= 0 ? ReadField<void*>(held, g_offDynmat) : nullptr;
    if (mid && R::IsLive(mid) && EnsureMaterialFns() &&
        GetScalar(mid, NameOnce(g_nOpac, L"opac"), opac) &&
        GetScalar(mid, NameOnce(g_nCol, L"col"), col)) {
        return true;
    }
    const float strength = g_offStrength >= 0 ? ReadField<float>(held, g_offStrength) : 1.5f;
    const float soap = g_offSoap >= 0 ? ReadField<float>(held, g_offSoap) : 0.f;
    opac = 0.4f * strength * (soap > 0.f ? 2.f : 1.f);
    col = 0.f;
    return true;
}

bool IsSignalPlaying() {
    void* gm = world_singleton::Gamemode();
    if (!gm) return false;
    if (g_offPanels < 0) g_offPanels = R::FindPropertyOffset(R::ClassOf(gm), L"analogPanels");
    if (g_offPanels < 0) return false;
    void* panels = ReadField<void*>(gm, g_offPanels);
    if (!panels || !R::IsLive(panels)) return false;
    if (g_panelsCls != R::ClassOf(panels)) {
        g_panelsCls = R::ClassOf(panels);
        if (!R::FindBoolProperty(g_panelsCls, L"isPlayingSignal", g_playingByte, g_playingMask))
            g_playingByte = -1;
    }
    if (g_playingByte < 0) return false;
    return (ReadField<uint8_t>(panels, g_playingByte) & g_playingMask) != 0;
}

bool DrawDab(void* window, const Dab& d) {
    if (!window || !R::IsLive(window) || !g_resolved.load(std::memory_order_acquire)) return false;
    if (!DrawSignatureOk() || !EnsureMaterialFns()) return false;
    void* brush = EnsureBrush(window);
    if (!brush) return false;

    ParamFrame open(g_canvasEvent);
    if (!open.valid() || !Call(window, open)) return false;
    void* canv = ReadField<void*>(window, g_offCanv);
    if (!canv || !R::IsLive(canv)) return false;

    SetScalar(brush, NameOnce(g_nOpac, L"opac"), d.opac);
    SetScalar(brush, NameOnce(g_nCol, L"col"), d.col);
    ParamFrame f(g_drawFn);
    f.Set<void*>(L"RenderMaterial", brush);
    f.Set<FVector2D>(L"ScreenPosition", FVector2D{d.x, d.y});
    f.Set<FVector2D>(L"ScreenSize", FVector2D{d.size, d.size});
    f.Set<FVector2D>(L"CoordinatePosition", FVector2D{0.f, 0.f});
    f.Set<FVector2D>(L"CoordinateSize", FVector2D{1.f, 1.f});
    f.Set<float>(L"Rotation", 0.f);
    f.Set<FVector2D>(L"PivotPoint", FVector2D{0.5f, 0.5f});
    return Call(canv, f);
}

void ReleaseBrush() {
    g_brush.Release();
}

}  // namespace ue_wrap::window_canvas
