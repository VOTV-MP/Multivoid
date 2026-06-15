// ue_wrap/console_desk.cpp -- see ue_wrap/console_desk.h.

#include "ue_wrap/console_desk.h"

#include "ue_wrap/call.h"
#include "ue_wrap/fname_utils.h"
#include "ue_wrap/ftext_utils.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/signal_dynamic.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace ue_wrap::console_desk {
namespace {

namespace R = ue_wrap::reflection;

void* g_cls = nullptr;
void* g_instance = nullptr;
int32_t g_instanceIdx = -1;

// Field offsets, FindPropertyOffset-resolved (recook-robust). Order matches
// the resolve table below.
struct FieldSlot { const wchar_t* name; int32_t off; };
FieldSlot g_fields[] = {
    { L"DL_poFilterOffset",  -1 },  // [0] float
    { L"DL_FrFilterOffset",  -1 },  // [1] float
    { L"DL_poFilterSpeed",   -1 },  // [2] float
    { L"DL_FrFilterSpeed",   -1 },  // [3] float
    { L"DL_downloading",     -1 },  // [4] float
    { L"DL_resDetecPercent", -1 },  // [5] float
    { L"comp_progress",      -1 },  // [6] float
    { L"coord_cooldown",     -1 },  // [7] float
    { L"play_volume",        -1 },  // [8] int32
    { L"DL_PolarityDir",     -1 },  // [9] int32
    { L"comp_maxLevel",      -1 },  // [10] int32
    { L"play_selectIndex",   -1 },  // [11] int32
    { L"DL_activeFrFilter",  -1 },  // [12] bool
    { L"DL_activePoFilter",  -1 },  // [13] bool
    { L"active_play",        -1 },  // [14] bool
    { L"active_download",    -1 },  // [15] bool
    { L"active_coords",      -1 },  // [16] bool
    { L"active_comp",        -1 },  // [17] bool
    { L"coord_isPing",       -1 },  // [18] bool
    // (canDL left the table in v70 -- DERIVED state, see the Scalars header note.)
    { L"coord_coordLog2Text", -1 }, // [19] FString -- the LIVE log (coord_coordLogText is dead)
    { L"comp_data_0",        -1 },  // [20] Fstruct_signalDataDynamic (0x70) -- the loaded signal
    { L"comp_downloading",   -1 },  // [21] float -- per-tick decode increment (B\s readout)
    { L"comp_isDecodeActive", -1 }, // [22] bool -- the decode latch (mirrors never write true)
};
// Scalars marshal indices: [6] comp_progress and [10] comp_maxLevel stay in
// the table; comp_progress moved OFF the DeskState marshal in v65 (it rides
// CompState from the simulating peer -- see the comp section below).

// The parameterless screen-refresh verbs WriteScalars runs after the raw
// writes -- the same upd* family setData's own apply chain uses, so a mirror
// repaint goes through the BP's own painters (LEDs, toggles, text panes).
// All 15 upd*/refresh verbs were audited side-effect CLEAN (phase-2 impl RE);
// updComp takes a bool Condition and is dispatched separately below.
struct RefreshSlot { const wchar_t* name; void* fn; };
RefreshSlot g_refresh[] = {
    { L"updText",            nullptr },
    { L"updToggles",         nullptr },
    { L"updPolarity",        nullptr },
    { L"updVolume",          nullptr },
    { L"updCoordLights",     nullptr },
    { L"updPlaybackLights",  nullptr },
    { L"updPolarityLights",  nullptr },
    { L"updMaxLevelLights",  nullptr },
    { L"updateCoordCoords",  nullptr },
};
void* g_updCompFn = nullptr;        // updComp(bool Condition)
void* g_writeToCoordLogFn = nullptr;  // writeToCoordLog_2 (the LIVE log writer)

// ---- v70 signal-catch consume surface ----
// Fstruct_signal_spawn member offsets (struct_signal_spawn.hpp; GUID-mangled
// member names, dump offsets authoritative -- the space_renderer.cpp kRow_*
// twin; coord_signalData is the same struct on the desk).
constexpr int32_t kSig_coordinates = 0x00;  // FVector
constexpr int32_t kSig_type        = 0x0C;  // int32
constexpr int32_t kSig_strength    = 0x10;  // float
constexpr int32_t kSig_frequency   = 0x14;  // float
constexpr int32_t kSig_freqSpread  = 0x18;  // float
constexpr int32_t kSig_polarity    = 0x1C;  // float
constexpr int32_t kSig_polSpread   = 0x20;  // float
constexpr int32_t kSig_objectName  = 0x24;  // FName (8)

int32_t g_offCoordSignalData = -1;   // desk.coord_signalData (@0x0A38, Fstruct_signal_spawn)
int32_t g_offDLRow = -1;             // desk.DL_signalDownloadData (struct_signal_data row)
int32_t g_offDLData = -1;            // desk.DL_SignalDownloadDLData (Fstruct_signalDataDynamic)
int32_t g_offDLFrData = -1;          // desk.DL_frData (needle)
int32_t g_offDLPoData = -1;          // desk.DL_poData (needle)
// struct_signal_data MEMBER offsets (GUID-mangled -> prefix-resolved on the
// live UserDefinedStruct; see FindPropertyOffsetByPrefix).
int32_t g_offRowSignalName = -1;     // signalName_50_... (FName)
int32_t g_offRowMesh = -1;           // mesh_9_... (UStaticMesh*)
void* g_formDownloadFn = nullptr;        // formDownload(decoded, polarity)
void* g_initDownloadSignalFn = nullptr;  // initDownloadSignal(signalLocation, decoded, polarity)
void* g_playPingSoundFn = nullptr;       // playPingSound(NewSound)
void* g_sndPingSuccess = nullptr;        // newdesk_panelCoord_pingSuccess cue/wave

// ---- comp pane substrate (v65) ----
int32_t g_offWidget = -1;            // desk.Widget (Uui_consolesAtlas_C*)
void* g_atlasCls = nullptr;
int32_t g_offTextCompProgress = -1;  // atlas.text_comp_progress (UTextBlock*)
int32_t g_offTextCompProcess = -1;   // atlas.text_comp_process
int32_t g_offAtlasUiCoords = -1;     // atlas.ui_coordinates (Uui_coordinates_C* @0x0588)
int32_t g_offCueWorking = -1;        // desk.computerWorking_Cue (UAudioComponent*)
int32_t g_offCueProg = -1;           // desk.prog
int32_t g_offCueDone = -1;           // desk.Done
void* g_setTextFn = nullptr;         // UTextBlock::SetText(FText)
const wchar_t* g_setTextParam = nullptr;
void* g_setSoundFn = nullptr;        // UAudioComponent::SetSound(USoundBase*)
void* g_activateFn = nullptr;        // UActorComponent::Activate(bool bReset)
void* g_sndWorking = nullptr;        // SoundCue 'computerWorking_Cue' (the loop)
void* g_sndWorkingEnd = nullptr;     // SoundCue 'computerWorking_end' (the wind-down)

// ui_coordinates (the coords-panel widget; SINGLETON child of the atlas) --
// the REAL dish-aim state + its repaint verb.
void* g_uiCoordsCls = nullptr;
void* g_uiCoordsInst = nullptr;
int32_t g_uiCoordsIdx = -1;
int32_t g_offViewCoordinate = -1;  // FVector2D @0x03B8
int32_t g_offCoordinate0 = -1;     // FVector2D @0x03C0
int32_t g_offCoordinate1 = -1;     // FVector2D @0x03C8
int32_t g_offCoordinate2 = -1;     // FVector2D @0x03D0
int32_t g_offSelected = -1;        // @0x03D8
int32_t g_offDirection = -1;       // bool @0x0441 -- the catch-gate toggle (v70)
void* g_updCursorLocationsFn = nullptr;

std::chrono::steady_clock::time_point g_nextResolve{};
bool g_coreResolved = false;

void ResolvePass() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + std::chrono::seconds(2);

    if (!g_cls) g_cls = R::FindClass(L"analogDScreenTest_C");
    if (!g_cls) return;
    bool all = true;
    for (auto& f : g_fields) {
        if (f.off < 0) f.off = R::FindPropertyOffset(g_cls, f.name);
        if (f.off < 0) all = false;
    }
    for (auto& r : g_refresh) {
        if (!r.fn) r.fn = R::FindFunction(g_cls, r.name);
    }
    if (!g_updCompFn) g_updCompFn = R::FindFunction(g_cls, L"updComp");
    if (!g_writeToCoordLogFn) g_writeToCoordLogFn = R::FindFunction(g_cls, L"writeToCoordLog_2");

    if (g_offCoordSignalData < 0)
        g_offCoordSignalData = R::FindPropertyOffset(g_cls, L"coord_signalData");
    if (g_offDLRow < 0) g_offDLRow = R::FindPropertyOffset(g_cls, L"DL_signalDownloadData");
    if (g_offDLData < 0) g_offDLData = R::FindPropertyOffset(g_cls, L"DL_SignalDownloadDLData");
    if (g_offDLFrData < 0) g_offDLFrData = R::FindPropertyOffset(g_cls, L"DL_frData");
    if (g_offDLPoData < 0) g_offDLPoData = R::FindPropertyOffset(g_cls, L"DL_poData");
    if (!g_formDownloadFn) g_formDownloadFn = R::FindFunction(g_cls, L"formDownload");
    if (!g_initDownloadSignalFn)
        g_initDownloadSignalFn = R::FindFunction(g_cls, L"initDownloadSignal");
    if (!g_playPingSoundFn) g_playPingSoundFn = R::FindFunction(g_cls, L"playPingSound");
    if (!g_sndPingSuccess) {
        g_sndPingSuccess = R::FindObject(L"newdesk_panelCoord_pingSuccess", L"SoundCue");
        if (!g_sndPingSuccess)
            g_sndPingSuccess = R::FindObject(L"newdesk_panelCoord_pingSuccess", L"SoundWave");
    }
    if (g_offRowSignalName < 0 || g_offRowMesh < 0) {
        // The list_objects DataTable row type (struct_signal_data), reached
        // through the property's OWN FStructProperty::Struct pointer -- a
        // global FindObject by name+class returned null on the live build
        // (2026-06-12 smoke: rowName/mesh=-1); the property chain is
        // deterministic. Members are GUID-mangled -> prefix resolve.
        if (void* rowStruct = R::PropertyInnerStruct(g_cls, L"DL_signalDownloadData")) {
            if (g_offRowSignalName < 0)
                g_offRowSignalName = R::FindPropertyOffsetByPrefix(rowStruct, L"signalName_");
            if (g_offRowMesh < 0)
                g_offRowMesh = R::FindPropertyOffsetByPrefix(rowStruct, L"mesh_");
        }
    }

    if (g_offWidget < 0) g_offWidget = R::FindPropertyOffset(g_cls, L"Widget");
    if (g_offCueWorking < 0) g_offCueWorking = R::FindPropertyOffset(g_cls, L"computerWorking_Cue");
    if (g_offCueProg < 0) g_offCueProg = R::FindPropertyOffset(g_cls, L"prog");
    if (g_offCueDone < 0) g_offCueDone = R::FindPropertyOffset(g_cls, L"Done");
    if (!g_atlasCls) g_atlasCls = R::FindClass(L"ui_consolesAtlas_C");
    if (g_atlasCls) {
        if (g_offTextCompProgress < 0)
            g_offTextCompProgress = R::FindPropertyOffset(g_atlasCls, L"text_comp_progress");
        if (g_offTextCompProcess < 0)
            g_offTextCompProcess = R::FindPropertyOffset(g_atlasCls, L"text_comp_process");
        if (g_offAtlasUiCoords < 0)
            g_offAtlasUiCoords = R::FindPropertyOffset(g_atlasCls, L"ui_coordinates");
    }
    // The cue ASSETS share the component property's leaf name -- class-filter
    // the lookup so we never grab the component instance by mistake.
    if (!g_sndWorking) {
        g_sndWorking = R::FindObject(L"computerWorking_Cue", L"SoundCue");
        if (!g_sndWorking) g_sndWorking = R::FindObject(L"computerWorking_Cue", L"SoundWave");
    }
    if (!g_sndWorkingEnd) {
        g_sndWorkingEnd = R::FindObject(L"computerWorking_end", L"SoundCue");
        if (!g_sndWorkingEnd) g_sndWorkingEnd = R::FindObject(L"computerWorking_end", L"SoundWave");
    }

    if (!g_uiCoordsCls) g_uiCoordsCls = R::FindClass(L"ui_coordinates_C");
    if (g_uiCoordsCls) {
        if (g_offViewCoordinate < 0)
            g_offViewCoordinate = R::FindPropertyOffset(g_uiCoordsCls, L"viewCoordinate");
        if (g_offCoordinate0 < 0)
            g_offCoordinate0 = R::FindPropertyOffset(g_uiCoordsCls, L"Coordinate_0");
        if (g_offCoordinate1 < 0)
            g_offCoordinate1 = R::FindPropertyOffset(g_uiCoordsCls, L"Coordinate_1");
        if (g_offCoordinate2 < 0)
            g_offCoordinate2 = R::FindPropertyOffset(g_uiCoordsCls, L"Coordinate_2");
        if (g_offSelected < 0)
            g_offSelected = R::FindPropertyOffset(g_uiCoordsCls, L"selected");
        if (g_offDirection < 0)
            g_offDirection = R::FindPropertyOffset(g_uiCoordsCls, L"Direction");
        if (!g_updCursorLocationsFn)
            g_updCursorLocationsFn = R::FindFunction(g_uiCoordsCls, L"updCursorLocations");
    }

    if (all && !g_coreResolved) {
        g_coreResolved = true;
        int fns = 0;
        for (auto& r : g_refresh) if (r.fn) ++fns;
        UE_LOGI("console_desk: resolved -- 23/23 fields, %d/9 refresh verbs (+updComp=%s), "
                "writeToCoordLog_2=%s, uiCoords view/c0/sel/dir=0x%X/0x%X/0x%X/0x%X updCursor=%s, "
                "comp widget/texts/cues=%s/%s/%s sounds=%s/%s, "
                "catch sig/row/dld=0x%X/0x%X/0x%X rowName/mesh=0x%X/0x%X "
                "form/init/ping=%s/%s/%s pingSnd=%s",
                fns, g_updCompFn ? "yes" : "NO",
                g_writeToCoordLogFn ? "yes" : "NO",
                g_offViewCoordinate, g_offCoordinate0, g_offSelected, g_offDirection,
                g_updCursorLocationsFn ? "yes" : "NO",
                g_offWidget >= 0 ? "yes" : "NO",
                (g_offTextCompProgress >= 0 && g_offTextCompProcess >= 0) ? "yes" : "NO",
                (g_offCueWorking >= 0 && g_offCueProg >= 0 && g_offCueDone >= 0) ? "yes" : "NO",
                g_sndWorking ? "yes" : "NO", g_sndWorkingEnd ? "yes" : "NO",
                g_offCoordSignalData, g_offDLRow, g_offDLData,
                g_offRowSignalName, g_offRowMesh,
                g_formDownloadFn ? "yes" : "NO", g_initDownloadSignalFn ? "yes" : "NO",
                g_playPingSoundFn ? "yes" : "NO", g_sndPingSuccess ? "yes" : "NO");
    }
}

void* UiCoordsInstance() {
    if (g_uiCoordsInst && R::IsLiveByIndex(g_uiCoordsInst, g_uiCoordsIdx)) return g_uiCoordsInst;
    g_uiCoordsInst = nullptr;
    if (!g_uiCoordsCls || g_offWidget < 0 || g_offAtlasUiCoords < 0) return nullptr;
    // Resolve via the desk's OWN field chain (desk.Widget -> atlas.ui_coordinates)
    // -- the game's access path (getCoordWidget does exactly this). The previous
    // FindObjectsByClass(ui_coordinates_C) first-hit grabbed the cooked
    // widget-tree TEMPLATE (loads with the class, IsLive, NOT Default__-named,
    // permanently resident -> lowest index wins) and cached it forever: the
    // host wrote dot state into the template (invisible) and the client read
    // the template's frozen fields (nothing to stream) = the dead star-map
    // mirror (STOLAS RE 2026-06-12 root cause 1).
    void* desk = Instance();
    if (!desk) return nullptr;
    void* atlas = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(desk) + g_offWidget);
    if (!atlas || !R::IsLive(atlas)) return nullptr;
    void* coords = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(atlas) + g_offAtlasUiCoords);
    if (!coords || !R::IsLive(coords)) return nullptr;
    void* cls = R::ClassOf(coords);
    if (!cls || !R::IsDescendantOfAny(cls, &g_uiCoordsCls, 1)) {
        UE_LOGW("console_desk: desk.Widget.ui_coordinates is not a ui_coordinates_C (cls=%p) -- not caching",
                cls);
        return nullptr;
    }
    g_uiCoordsInst = coords;
    g_uiCoordsIdx = R::InternalIndexOf(coords);
    UE_LOGI("console_desk: ui_coordinates LIVE instance resolved via desk.Widget chain (%p)", coords);
    return g_uiCoordsInst;
}

template <class T>
T* FieldPtr(void* obj, int idx) {
    if (g_fields[idx].off < 0) return nullptr;
    return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(obj) + g_fields[idx].off);
}

// Engine FString view: {wchar_t* data; int32 num (incl. NUL); int32 max}.
struct FStringView { wchar_t* data; int32_t num; int32_t max; };

}  // namespace

bool EnsureResolved() {
    ResolvePass();
    return g_coreResolved;
}

void* Instance() {
    if (g_instance && R::IsLiveByIndex(g_instance, g_instanceIdx)) return g_instance;
    g_instance = nullptr;
    if (!g_cls) return nullptr;
    // Singleton placed actor (gamemode.analogPanels resolves it the same way:
    // GetActorOfClass). One-shot walk per loss; cached + liveness-revalidated.
    for (void* obj : R::FindObjectsByClass(L"analogDScreenTest_C")) {
        if (obj && R::IsLive(obj)) {
            g_instance = obj;
            g_instanceIdx = R::InternalIndexOf(obj);
            break;
        }
    }
    return g_instance;
}

bool ReadScalars(Scalars& out) {
    void* d = Instance();
    if (!d || !g_coreResolved) return false;
    out.dlPoFilterOffset  = *FieldPtr<float>(d, 0);
    out.dlFrFilterOffset  = *FieldPtr<float>(d, 1);
    out.dlPoFilterSpeed   = *FieldPtr<float>(d, 2);
    out.dlFrFilterSpeed   = *FieldPtr<float>(d, 3);
    out.dlDownloading     = *FieldPtr<float>(d, 4);
    out.dlResDetecPercent = *FieldPtr<float>(d, 5);
    out.coordCooldown     = *FieldPtr<float>(d, 7);
    out.playVolume        = *FieldPtr<int32_t>(d, 8);
    out.dlPolarityDir     = *FieldPtr<int32_t>(d, 9);
    out.compMaxLevel      = *FieldPtr<int32_t>(d, 10);
    out.playSelectIndex   = *FieldPtr<int32_t>(d, 11);
    out.dlActiveFrFilter  = *FieldPtr<bool>(d, 12);
    out.dlActivePoFilter  = *FieldPtr<bool>(d, 13);
    out.activePlay        = *FieldPtr<bool>(d, 14);
    out.activeDownload    = *FieldPtr<bool>(d, 15);
    out.activeCoords      = *FieldPtr<bool>(d, 16);
    out.activeComp        = *FieldPtr<bool>(d, 17);
    out.coordIsPing       = *FieldPtr<bool>(d, 18);
    return true;
}

bool WriteScalars(const Scalars& in) {
    void* d = Instance();
    if (!d || !g_coreResolved) return false;
    *FieldPtr<float>(d, 0)  = in.dlPoFilterOffset;
    *FieldPtr<float>(d, 1)  = in.dlFrFilterOffset;
    *FieldPtr<float>(d, 2)  = in.dlPoFilterSpeed;
    *FieldPtr<float>(d, 3)  = in.dlFrFilterSpeed;
    *FieldPtr<float>(d, 4)  = in.dlDownloading;
    *FieldPtr<float>(d, 5)  = in.dlResDetecPercent;
    *FieldPtr<float>(d, 7)  = in.coordCooldown;
    *FieldPtr<int32_t>(d, 8)  = in.playVolume;
    *FieldPtr<int32_t>(d, 9)  = in.dlPolarityDir;
    *FieldPtr<int32_t>(d, 10) = in.compMaxLevel;
    *FieldPtr<int32_t>(d, 11) = in.playSelectIndex;
    *FieldPtr<bool>(d, 12) = in.dlActiveFrFilter;
    *FieldPtr<bool>(d, 13) = in.dlActivePoFilter;
    *FieldPtr<bool>(d, 14) = in.activePlay;
    *FieldPtr<bool>(d, 15) = in.activeDownload;
    *FieldPtr<bool>(d, 16) = in.activeCoords;
    *FieldPtr<bool>(d, 17) = in.activeComp;
    *FieldPtr<bool>(d, 18) = in.coordIsPing;
    // Repaint through the BP's own painters. Each is a cheap widget/material
    // refresh, audited side-effect clean (human-rate call site: wire applies).
    // (The comp-pane repaint -- updComp(hasData) -- moved to the v65 comp
    // section below; v64 passed activeComp here, the wrong condition.)
    for (auto& r : g_refresh) {
        if (!r.fn) continue;
        ue_wrap::ParamFrame f(r.fn);
        if (f.valid()) ue_wrap::Call(d, f);
    }
    return true;
}

std::wstring ReadCoordLogTail(size_t maxChars) {
    void* d = Instance();
    if (!d || g_fields[19].off < 0) return {};
    auto* s = reinterpret_cast<FStringView*>(
        reinterpret_cast<uint8_t*>(d) + g_fields[19].off);
    if (!s->data || s->num <= 1) return {};
    const size_t len = static_cast<size_t>(s->num - 1);  // num includes the NUL
    const size_t take = len > maxChars ? maxChars : len;
    return std::wstring(s->data + (len - take), take);
}

bool CoordLogTailEquals(const std::wstring& expected, size_t maxChars) {
    void* d = Instance();
    if (!d || g_fields[19].off < 0) return expected.empty();
    auto* s = reinterpret_cast<FStringView*>(
        reinterpret_cast<uint8_t*>(d) + g_fields[19].off);
    if (!s->data || s->num <= 1) return expected.empty();
    const size_t len = static_cast<size_t>(s->num - 1);
    const size_t take = len > maxChars ? maxChars : len;
    if (take != expected.size()) return false;
    return std::wmemcmp(s->data + (len - take), expected.data(), take) == 0;
}

bool AppendCoordLog(const std::wstring& suffix) {
    void* d = Instance();
    if (!d || !g_writeToCoordLogFn || suffix.empty()) return false;
    // writeToCoordLog_2(FString B): the param-frame FString may point at OUR
    // buffer for the call's duration -- the BP assignment copies the data
    // into the engine-side field with the engine allocator (the economy
    // order-struct precedent). The BP self-caps the log at 1000 chars.
    ue_wrap::ParamFrame f(g_writeToCoordLogFn);
    if (!f.valid()) return false;
    FStringView v{ const_cast<wchar_t*>(suffix.c_str()),
                   static_cast<int32_t>(suffix.size() + 1),
                   static_cast<int32_t>(suffix.size() + 1) };
    if (!f.SetRaw(L"B", &v, sizeof(v))) return false;
    return ue_wrap::Call(d, f);
}

// ---- The refiner (comp) pane (v65) ----

namespace {

// Atlas widget instance (desk.Widget).
void* AtlasWidget() {
    void* d = Instance();
    if (!d || g_offWidget < 0) return nullptr;
    void* w = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(d) + g_offWidget);
    return (w && R::IsLive(w)) ? w : nullptr;
}

void* AtlasTextBlock(int32_t off) {
    void* atlas = AtlasWidget();
    if (!atlas || off < 0) return nullptr;
    void* tb = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(atlas) + off);
    return (tb && R::IsLive(tb)) ? tb : nullptr;
}

bool SetTextOn(void* textBlock, const wchar_t* text) {
    if (!textBlock || !text) return false;
    if (!g_setTextFn) {
        if (void* cls = R::ClassOf(textBlock))
            g_setTextFn = R::FindFunction(cls, L"SetText");
        if (g_setTextFn) {
            if (R::FindParamOffset(g_setTextFn, L"InText") >= 0) g_setTextParam = L"InText";
            else if (R::FindParamOffset(g_setTextFn, L"inText") >= 0) g_setTextParam = L"inText";
        }
    }
    if (!g_setTextFn || !g_setTextParam) return false;
    uint8_t ftext[ue_wrap::ftext_utils::kFTextSize];
    if (!ue_wrap::ftext_utils::MintFText(text, ftext)) return false;
    ue_wrap::ParamFrame f(g_setTextFn);
    if (!f.valid() || !f.SetRaw(g_setTextParam, ftext, sizeof(ftext))) return false;
    return ue_wrap::Call(textBlock, f);
}

void* DeskAudioComponent(int32_t off) {
    void* d = Instance();
    if (!d || off < 0) return nullptr;
    void* c = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(d) + off);
    return (c && R::IsLive(c)) ? c : nullptr;
}

bool CueSetSound(void* comp, void* sound) {
    if (!comp || !sound) return false;
    if (!g_setSoundFn) {
        if (void* cls = R::ClassOf(comp))
            g_setSoundFn = R::FindFunction(cls, L"SetSound");
    }
    if (!g_setSoundFn) return false;
    ue_wrap::ParamFrame f(g_setSoundFn);
    if (!f.valid() || !f.SetRaw(L"NewSound", &sound, sizeof(sound))) return false;
    return ue_wrap::Call(comp, f);
}

bool CueActivate(void* comp) {
    if (!comp) return false;
    if (!g_activateFn) {
        if (void* cls = R::ClassOf(comp))
            g_activateFn = R::FindFunction(cls, L"Activate");
    }
    if (!g_activateFn) return false;
    ue_wrap::ParamFrame f(g_activateFn);
    if (!f.valid()) return false;
    f.Set<bool>(L"bReset", true);
    return ue_wrap::Call(comp, f);
}

}  // namespace

bool ReadCompScalars(CompScalars& out) {
    void* d = Instance();
    if (!d || !g_coreResolved) return false;
    out.progress = *FieldPtr<float>(d, 6);
    out.downloading = *FieldPtr<float>(d, 21);
    out.decodeActive = *FieldPtr<bool>(d, 22);
    return true;
}

bool WriteCompScalars(float progress, float downloading) {
    void* d = Instance();
    if (!d || !g_coreResolved) return false;
    *FieldPtr<float>(d, 6) = progress;
    *FieldPtr<float>(d, 21) = downloading;
    return true;
}

void* CompDataPtr() {
    void* d = Instance();
    if (!d || g_fields[20].off < 0) return nullptr;
    return reinterpret_cast<uint8_t*>(d) + g_fields[20].off;
}

bool UnlatchDecode() {
    void* d = Instance();
    if (!d || !g_coreResolved) return false;
    bool* flag = FieldPtr<bool>(d, 22);
    if (!*flag) return true;  // not latched: nothing to do
    *flag = false;
    CompCueStop();
    PaintCompProcess(L"idle");
    return true;
}

bool UpdComp(bool hasData) {
    void* d = Instance();
    if (!d || !g_updCompFn) return false;
    ue_wrap::ParamFrame f(g_updCompFn);
    if (!f.valid()) return false;
    f.Set<bool>(L"Condition", hasData);
    return ue_wrap::Call(d, f);
}

bool PaintCompProgress(float progress) {
    void* tb = AtlasTextBlock(g_offTextCompProgress);
    if (!tb) return false;
    // The native paint: Conv_FloatToText(min 3 integral / 3,3 fractional) + "%".
    wchar_t buf[24];
    swprintf(buf, 24, L"%07.3f%%", progress);
    return SetTextOn(tb, buf);
}

bool PaintCompProcess(const wchar_t* text) {
    void* tb = AtlasTextBlock(g_offTextCompProcess);
    if (!tb) return false;
    return SetTextOn(tb, text);
}

bool CompCueStart() {
    void* c = DeskAudioComponent(g_offCueWorking);
    if (!c) return false;
    if (g_sndWorking) CueSetSound(c, g_sndWorking);
    return CueActivate(c);
}

bool CompCueStop() {
    void* c = DeskAudioComponent(g_offCueWorking);
    if (!c) return false;
    if (!g_sndWorkingEnd) return false;  // never Activate the LOOP as a stop
    if (!CueSetSound(c, g_sndWorkingEnd)) return false;
    return CueActivate(c);
}

bool CompBeepDone(bool maxed) {
    void* c = DeskAudioComponent(maxed ? g_offCueDone : g_offCueProg);
    if (!c) return false;
    return CueActivate(c);
}

bool ReadDishAim(DishAim& out) {
    void* w = UiCoordsInstance();
    if (!w || g_offViewCoordinate < 0 || g_offCoordinate0 < 0 ||
        g_offCoordinate1 < 0 || g_offCoordinate2 < 0 || g_offSelected < 0)
        return false;
    auto* p = reinterpret_cast<uint8_t*>(w);
    auto rd = [&](int32_t off, float& x, float& y) {
        std::memcpy(&x, p + off + 0, sizeof(float));
        std::memcpy(&y, p + off + 4, sizeof(float));
    };
    rd(g_offViewCoordinate, out.viewX, out.viewY);
    rd(g_offCoordinate0, out.c0X, out.c0Y);
    rd(g_offCoordinate1, out.c1X, out.c1Y);
    rd(g_offCoordinate2, out.c2X, out.c2Y);
    std::memcpy(&out.selected, p + g_offSelected, sizeof(int32_t));
    out.direction = (g_offDirection >= 0 && *(p + g_offDirection)) ? 1 : 0;
    return true;
}

bool WriteDishAim(const DishAim& in) {
    void* w = UiCoordsInstance();
    if (!w || g_offViewCoordinate < 0 || g_offCoordinate0 < 0 ||
        g_offCoordinate1 < 0 || g_offCoordinate2 < 0 || g_offSelected < 0)
        return false;
    auto* p = reinterpret_cast<uint8_t*>(w);
    auto wr = [&](int32_t off, float x, float y) {
        std::memcpy(p + off + 0, &x, sizeof(float));
        std::memcpy(p + off + 4, &y, sizeof(float));
    };
    wr(g_offViewCoordinate, in.viewX, in.viewY);
    wr(g_offCoordinate0, in.c0X, in.c0Y);
    wr(g_offCoordinate1, in.c1X, in.c1Y);
    wr(g_offCoordinate2, in.c2X, in.c2Y);
    std::memcpy(p + g_offSelected, &in.selected, sizeof(int32_t));
    if (g_offDirection >= 0) *(p + g_offDirection) = in.direction ? 1 : 0;
    // The widget's own repaint -- also rotates the physical pingDishes
    // (setRot) so the world dishes track the mirrored aim. (The remote's own
    // spaceRenderer tick re-runs this per frame anyway -- belt and braces.)
    if (g_updCursorLocationsFn) {
        ue_wrap::ParamFrame f(g_updCursorLocationsFn);
        if (f.valid()) ue_wrap::Call(w, f);
    }
    // The desk's az/alt TEXT pane derives from viewCoordinate but nothing
    // re-calls its painter on a mirror (phase-2 impl RE SS2.4/SS2.6:
    // updateCoordCoords belongs in the dish-aim apply).
    void* desk = Instance();
    if (desk && g_refresh[8].fn) {  // [8] = updateCoordCoords
        ue_wrap::ParamFrame f(g_refresh[8].fn);
        if (f.valid()) ue_wrap::Call(desk, f);
    }
    return true;
}

// ---- The v70 signal-catch consume surface ----

namespace {

uint8_t* CoordSignalPtr() {
    void* d = Instance();
    if (!d || g_offCoordSignalData < 0) return nullptr;
    return reinterpret_cast<uint8_t*>(d) + g_offCoordSignalData;
}

template <class T>
T SigRead(const uint8_t* base, int32_t off) {
    T v; std::memcpy(&v, base + off, sizeof(T)); return v;
}
template <class T>
void SigWrite(uint8_t* base, int32_t off, const T& v) {
    std::memcpy(base + off, &v, sizeof(T));
}

}  // namespace

bool ReadCoordSignal(CoordSignal& out) {
    uint8_t* p = CoordSignalPtr();
    if (!p) return false;
    out.x = SigRead<float>(p, kSig_coordinates + 0);
    out.y = SigRead<float>(p, kSig_coordinates + 4);
    out.z = SigRead<float>(p, kSig_coordinates + 8);
    out.type = SigRead<int32_t>(p, kSig_type);
    out.strength = SigRead<float>(p, kSig_strength);
    out.frequency = SigRead<float>(p, kSig_frequency);
    out.frequencySpread = SigRead<float>(p, kSig_freqSpread);
    out.polarity = SigRead<float>(p, kSig_polarity);
    out.polaritySpread = SigRead<float>(p, kSig_polSpread);
    out.objectName = R::ToString(SigRead<R::FName>(p, kSig_objectName));
    return true;
}

bool WriteCoordSignal(const CoordSignal& in) {
    uint8_t* p = CoordSignalPtr();
    if (!p) return false;
    SigWrite<float>(p, kSig_coordinates + 0, in.x);
    SigWrite<float>(p, kSig_coordinates + 4, in.y);
    SigWrite<float>(p, kSig_coordinates + 8, in.z);
    SigWrite<int32_t>(p, kSig_type, in.type);
    SigWrite<float>(p, kSig_strength, in.strength);
    SigWrite<float>(p, kSig_frequency, in.frequency);
    SigWrite<float>(p, kSig_freqSpread, in.frequencySpread);
    SigWrite<float>(p, kSig_polarity, in.polarity);
    SigWrite<float>(p, kSig_polSpread, in.polaritySpread);
    SigWrite<R::FName>(p, kSig_objectName,
                       ue_wrap::fname_utils::StringToFName(in.objectName));
    return true;
}

bool ClearCoordSignal() {
    // The @34134 reset literal: struct{vec(0), 0, +0f, +0f, 0.5f, +0f, 0.5f,
    // name'None'}. FName zero == None (POD, no heap).
    uint8_t* p = CoordSignalPtr();
    if (!p) return false;
    SigWrite<float>(p, kSig_coordinates + 0, 0.f);
    SigWrite<float>(p, kSig_coordinates + 4, 0.f);
    SigWrite<float>(p, kSig_coordinates + 8, 0.f);
    SigWrite<int32_t>(p, kSig_type, 0);
    SigWrite<float>(p, kSig_strength, 0.f);
    SigWrite<float>(p, kSig_frequency, 0.f);
    SigWrite<float>(p, kSig_freqSpread, 0.5f);
    SigWrite<float>(p, kSig_polarity, 0.f);
    SigWrite<float>(p, kSig_polSpread, 0.5f);
    SigWrite<R::FName>(p, kSig_objectName, R::FName{0, 0});
    return true;
}

bool ResetDownloadMachine() {
    void* d = Instance();
    if (!d || !g_coreResolved) return false;
    if (g_offDLRow < 0 || g_offRowSignalName < 0 || g_offRowMesh < 0 ||
        g_offDLFrData < 0 || g_offDLPoData < 0 || !g_initDownloadSignalFn)
        return false;
    auto* row = reinterpret_cast<uint8_t*>(d) + g_offDLRow;
    // The two load-bearing members (see the header note): mesh validity gates
    // the accrual + the playSignall screen; signalName gates the next
    // initDownloadSignal rebuild. FName/pointer writes are POD-safe.
    SigWrite<R::FName>(row, g_offRowSignalName, R::FName{0, 0});
    SigWrite<void*>(row, g_offRowMesh, nullptr);
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offDLFrData) = 0.f;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offDLPoData) = 0.f;
    *FieldPtr<float>(d, 5) = 0.f;  // [5] DL_resDetecPercent (@33982)
    // initDownloadSignal({0,0}, 0, -1): with signalName None this is the
    // native @34005 path -- rebuilds DL_SignalDownloadDLData empty (engine-
    // side struct assignment, no leaked FStrings) + downloadTexts repaint.
    ue_wrap::ParamFrame f(g_initDownloadSignalFn);
    if (!f.valid()) return false;
    struct { float X, Y; } loc{ 0.f, 0.f };
    if (!f.SetRaw(L"signalLocation", &loc, sizeof(loc))) return false;
    f.Set<float>(L"decoded", 0.f);
    f.Set<int32_t>(L"polarity", -1);
    return ue_wrap::Call(d, f);
}

bool ArmDownloadFromSignal(float decoded, int32_t polarity) {
    void* d = Instance();
    if (!d || !g_formDownloadFn) return false;
    ue_wrap::ParamFrame f(g_formDownloadFn);
    if (!f.valid()) return false;
    f.Set<float>(L"decoded", decoded);
    f.Set<int32_t>(L"polarity", polarity);
    return ue_wrap::Call(d, f);
}

bool ReadDownloadProgress(float& decoded, int32_t& polarity) {
    void* d = Instance();
    if (!d || g_offDLData < 0) return false;
    auto* dld = reinterpret_cast<uint8_t*>(d) + g_offDLData;
    // Fstruct_signalDataDynamic member offsets (ue_wrap/signal_dynamic.h).
    decoded = SigRead<float>(dld, ue_wrap::signal_dynamic::kOff_decoded);
    polarity = SigRead<int32_t>(dld, ue_wrap::signal_dynamic::kOff_polarity);
    return true;
}

bool DownloadMeshValid() {
    void* d = Instance();
    if (!d || g_offDLRow < 0 || g_offRowMesh < 0) return false;
    void* mesh = SigRead<void*>(reinterpret_cast<uint8_t*>(d) + g_offDLRow, g_offRowMesh);
    return mesh != nullptr && R::IsLive(mesh);
}

bool WriteResDetect(float v) {
    void* d = Instance();
    if (!d || !g_coreResolved) return false;
    *FieldPtr<float>(d, 5) = v;  // [5] DL_resDetecPercent
    return true;
}

bool PlayPingSuccess() {
    void* d = Instance();
    if (!d || !g_playPingSoundFn || !g_sndPingSuccess) return false;
    ue_wrap::ParamFrame f(g_playPingSoundFn);
    if (!f.valid()) return false;
    f.Set<void*>(L"NewSound", g_sndPingSuccess);
    return ue_wrap::Call(d, f);
}

}  // namespace ue_wrap::console_desk
