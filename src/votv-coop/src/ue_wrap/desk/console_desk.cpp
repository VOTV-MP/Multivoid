// ue_wrap/desk/console_desk.cpp -- see ue_wrap/desk/console_desk.h.

#include "ue_wrap/desk/console_desk.h"

#include "ue_wrap/core/object_index.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/coords_panel.h"
#include "ue_wrap/desk/signal_dynamic.h"
#include "ue_wrap/world/world_singleton.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace ue_wrap::console_desk {
namespace {

namespace R = ue_wrap::reflection;

void* g_cls = nullptr;
void* g_instance = nullptr;
int32_t g_instanceIdx = -1;

// Field offsets, resolved by property name (recook-robust). Named variables, each bound to
// its wire name by the resolve rows below: a positional table with magic indices would let a
// removed row silently shift every later index, while a named dereference cannot shift.
int32_t g_offDlPoFilterOffset = -1;     // float
int32_t g_offDlFrFilterOffset = -1;     // float
int32_t g_offDlPoFilterSpeed = -1;      // float
int32_t g_offDlFrFilterSpeed = -1;      // float
int32_t g_offDlDownloading = -1;        // float
int32_t g_offDlResDetecPercent = -1;    // float
int32_t g_offCoordCooldown = -1;        // float
int32_t g_offPlayVolume = -1;           // int32
int32_t g_offDlPolarityDir = -1;        // int32
int32_t g_offCompMaxLevel = -1;         // int32
int32_t g_offPlaySelectIndex = -1;      // int32
int32_t g_offDlActiveFrFilter = -1;     // bool
int32_t g_offDlActivePoFilter = -1;     // bool
int32_t g_offActivePlay = -1;           // bool
int32_t g_offActiveDownload = -1;       // bool
int32_t g_offActiveCoords = -1;         // bool
int32_t g_offActiveComp = -1;           // bool
int32_t g_offCoordIsPing = -1;          // bool
int32_t g_offRemapValue = -1;           // FVector2D: X the lower bound, Y the upper
int32_t g_offVisualizerMode = -1;       // bool
// The derived download-allowed flag is not a field here (see the header), and the comp-pane
// fields live in ue_wrap/desk/comp_pane; the comp max level stays, since it rides the
// desk-state marshal.
int32_t g_offCoordCoordLog2Text = -1;   // FString -- the LIVE log (coord_coordLogText is dead)

struct FieldSlot { const wchar_t* name; int32_t* off; };
FieldSlot g_fields[] = {
    { L"remapValue",          &g_offRemapValue },
    { L"visualizerMode",      &g_offVisualizerMode },
    { L"DL_poFilterOffset",   &g_offDlPoFilterOffset },
    { L"DL_FrFilterOffset",   &g_offDlFrFilterOffset },
    { L"DL_poFilterSpeed",    &g_offDlPoFilterSpeed },
    { L"DL_FrFilterSpeed",    &g_offDlFrFilterSpeed },
    { L"DL_downloading",      &g_offDlDownloading },
    { L"DL_resDetecPercent",  &g_offDlResDetecPercent },
    { L"coord_cooldown",      &g_offCoordCooldown },
    { L"play_volume",         &g_offPlayVolume },
    { L"DL_PolarityDir",      &g_offDlPolarityDir },
    { L"comp_maxLevel",       &g_offCompMaxLevel },
    { L"play_selectIndex",    &g_offPlaySelectIndex },
    { L"DL_activeFrFilter",   &g_offDlActiveFrFilter },
    { L"DL_activePoFilter",   &g_offDlActivePoFilter },
    { L"active_play",         &g_offActivePlay },
    { L"active_download",     &g_offActiveDownload },
    { L"active_coords",       &g_offActiveCoords },
    { L"active_comp",         &g_offActiveComp },
    { L"coord_isPing",        &g_offCoordIsPing },
    { L"coord_coordLog2Text", &g_offCoordCoordLog2Text },
};

// The parameterless screen-refresh verbs WriteScalars runs after the raw writes, the update family
// the game's own apply chain uses, so a mirror repaints through the blueprint's own painters (LEDs,
// toggles, text panes). The comp repaint takes a condition and is dispatched separately.
//
// One of them writes PLAYER state: updToggles ends by nulling the local player's lookAtComponent,
// the game invalidating its look-at cache after a real press. Called on a clock, that invalidation
// rebuilds the whole action row under a held aim at the clock's rate (the measurement is in
// docs/coop-dispatch-visibility.md), so a verb here runs only on an edge that changed a scalar IT
// paints, and never from a periodic caller. That is why callers name PAINTERS rather than taking
// the whole chain: a cooldown charge, which ships every 250 ms while a peer holds the scan, paints
// nothing, since no verb below reads coord_cooldown. Each verb's scalar dependencies, read off its
// bytecode, are on its row in the array; none of the nine reads a SIM output, which is why
// WriteSimOutputs runs none of them.
struct RefreshSlot { const wchar_t* name; void* fn; };
// ORDER IS THE CONTRACT: slot i is Painter bit i (console_desk.h). A caller selects verbs by those
// bits, so re-ordering this array silently re-points every selection.
RefreshSlot g_refresh[] = {
    { L"updText",            nullptr },   // kPaintText
    { L"updToggles",         nullptr },   // kPaintToggles
    { L"updPolarity",        nullptr },   // kPaintPolarity
    { L"updVolume",          nullptr },   // kPaintVolume
    { L"updCoordLights",     nullptr },   // kPaintCoordLights
    { L"updPlaybackLights",  nullptr },   // kPaintPlaybackLights
    { L"updPolarityLights",  nullptr },   // kPaintPolarityLights
    { L"updMaxLevelLights",  nullptr },   // kPaintMaxLevelLights
    { L"updateCoordCoords",  nullptr },   // kPaintCoordCoords
    { L"updRemap",           nullptr },   // kPaintRemap
};
constexpr size_t kRefreshCount = sizeof(g_refresh) / sizeof(g_refresh[0]);
static_assert(kPaintAll == (1u << kRefreshCount) - 1u,
              "a painter verb was added or removed without its Painter bit");
constexpr size_t kRefreshCoordCoords = 8;  // kPaintCoordCoords' slot, dispatched alone below
void* g_writeToCoordLogFn = nullptr;  // writeToCoordLog_2 (the LIVE log writer)

// The signal-catch consume surface. The signal-spawn struct's member offsets come from the
// struct dump (GUID-mangled member names, so the dump offsets are authoritative); the desk's
// coordinate signal is the same struct.
constexpr int32_t kSig_coordinates = 0x00;  // FVector
constexpr int32_t kSig_type        = 0x0C;  // int32
constexpr int32_t kSig_strength    = 0x10;  // float
constexpr int32_t kSig_frequency   = 0x14;  // float
constexpr int32_t kSig_freqSpread  = 0x18;  // float
constexpr int32_t kSig_polarity    = 0x1C;  // float
constexpr int32_t kSig_polSpread   = 0x20;  // float
constexpr int32_t kSig_objectName  = 0x24;  // FName (8)

int32_t g_offCoordSignalData = -1;   // desk.coord_signalData (the signal-spawn struct)
int32_t g_offDLRow = -1;             // desk.DL_signalDownloadData (struct_signal_data row)
int32_t g_offDLData = -1;            // desk.DL_SignalDownloadDLData (Fstruct_signalDataDynamic)
int32_t g_offDLFrData = -1;          // desk.DL_frData (needle)
int32_t g_offDLPoData = -1;          // desk.DL_poData (needle)
// The signal-data row's member offsets, GUID-mangled and prefix-resolved on the live
// user-defined struct.
int32_t g_offRowSignalName = -1;     // signalName_50_... (FName)
int32_t g_offRowMesh = -1;           // mesh_9_... (UStaticMesh*)
// The rest of the space-object struct, for the catch's literal (WriteCatchReset).
int32_t g_offRowDisplayName = -1;    // displayName_32_... (FText)
int32_t g_offRowRing = -1;           // ring_11_... (object pointer)
int32_t g_offRowAsActor = -1;        // asActor_56_... (class pointer)
int32_t g_offRowRot[4] = {-1, -1, -1, -1};  // rotation_init_, rotation_1_, rotation_2_, rotation_3_ (FRotator)
int32_t g_offRowType = -1;           // objectType_53_... (enum byte)
int32_t g_offRowFreq = -1;           // signal_frequency_52_... (enum byte)
int32_t g_offRowQual = -1;           // signal_qualiy_51_... (enum byte)
void* g_formDownloadFn = nullptr;        // formDownload(decoded, polarity)
void* g_playPingSoundFn = nullptr;       // playPingSound(NewSound)

// The desk widget to atlas chain, the desk-half seams. The comp-pane texts, cues and sounds
// live in ue_wrap/desk/comp_pane; the component calls (set text, set sound, activate,
// visibility, volume, parameterless call) in ue_wrap/core/component_calls.
int32_t g_offWidget = -1;            // desk.Widget (Uui_consolesAtlas_C*)
void* g_atlasCls = nullptr;
int32_t g_offAtlasUiCoords = -1;     // atlas.ui_coordinates

// The coordinates-panel widget surface lives in ue_wrap/desk/coords_panel; the desk half of
// its instance chain is AtlasUiCoordsSlot below.
void* g_intComsUnfocusedFn = nullptr;   // the desk's unfocus verb, the reset-on-release target

// The desk-input apply surface (the desk input sync): the scan charge's target, the playback
// volume's component, and the verbs the playback and scan mirrors replay.
int32_t g_offMaxCooldown = -1;       // coord_maxCooldown (the scan-charge target)
int32_t g_offPrecMult = -1;          // DL_precMult (the dishes' average precision, setPrec's)
int32_t g_offSignalSound = -1;       // signalSound (UAudioComponent* -- playback volume)
void* g_stopSoundFn = nullptr;           // desk stopSound()
void* g_playSignalFn = nullptr;          // desk playSignal (the deck-playback mirror replay)
void* g_spawnDirsFn = nullptr;           // desk spawnDirs() (the scan arrows)
void* g_setCanvasFn = nullptr;           // desk setCanvas(visualizerMode), the signal view's draw
void* g_endCanvasFn = nullptr;           // desk endCanvas(), which closes it

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
        if (*f.off < 0) *f.off = R::FindPropertyOffset(g_cls, f.name);
        if (*f.off < 0) all = false;
    }
    for (auto& r : g_refresh) {
        if (!r.fn) r.fn = R::FindFunction(g_cls, r.name);
    }
    if (!g_intComsUnfocusedFn) g_intComsUnfocusedFn = R::FindFunction(g_cls, L"intComs_unfocused");
    if (!g_writeToCoordLogFn) g_writeToCoordLogFn = R::FindFunction(g_cls, L"writeToCoordLog_2");

    if (g_offCoordSignalData < 0)
        g_offCoordSignalData = R::FindPropertyOffset(g_cls, L"coord_signalData");
    if (g_offDLRow < 0) g_offDLRow = R::FindPropertyOffset(g_cls, L"DL_signalDownloadData");
    if (g_offDLData < 0) g_offDLData = R::FindPropertyOffset(g_cls, L"DL_SignalDownloadDLData");
    if (g_offDLFrData < 0) g_offDLFrData = R::FindPropertyOffset(g_cls, L"DL_frData");
    if (g_offDLPoData < 0) g_offDLPoData = R::FindPropertyOffset(g_cls, L"DL_poData");
    if (!g_formDownloadFn) g_formDownloadFn = R::FindFunction(g_cls, L"formDownload");
    if (!g_playPingSoundFn) g_playPingSoundFn = R::FindFunction(g_cls, L"playPingSound");
    if (g_offRowSignalName < 0 || g_offRowMesh < 0 || g_offRowQual < 0) {
        // The objects table's row type, reached through the property's own struct pointer: a global
        // find by name and class returns null on the live build, while the property chain is
        // deterministic. Members are GUID-mangled, so prefix-resolved.
        if (void* rowStruct = R::PropertyInnerStruct(g_cls, L"DL_signalDownloadData")) {
            struct { int32_t* off; const wchar_t* prefix; } members[] = {
                {&g_offRowSignalName, L"signalName_"}, {&g_offRowMesh, L"mesh_"},
                {&g_offRowDisplayName, L"displayName_"}, {&g_offRowRing, L"ring_"},
                {&g_offRowAsActor, L"asActor_"}, {&g_offRowRot[0], L"rotation_init_"},
                {&g_offRowRot[1], L"rotation_1_"}, {&g_offRowRot[2], L"rotation_2_"},
                {&g_offRowRot[3], L"rotation_3_"}, {&g_offRowType, L"objectType_"},
                {&g_offRowFreq, L"signal_frequency_"}, {&g_offRowQual, L"signal_qualiy_"},
            };
            for (auto& m : members)
                if (*m.off < 0) *m.off = R::FindPropertyOffsetByPrefix(rowStruct, m.prefix);
        }
    }

    if (g_offWidget < 0) g_offWidget = R::FindPropertyOffset(g_cls, L"Widget");
    if (!g_atlasCls) g_atlasCls = R::FindClass(L"ui_consolesAtlas_C");
    if (g_atlasCls) {
        if (g_offAtlasUiCoords < 0)
            g_offAtlasUiCoords = R::FindPropertyOffset(g_atlasCls, L"ui_coordinates");
    }

    // The desk-input apply surface.
    if (g_offMaxCooldown < 0) g_offMaxCooldown = R::FindPropertyOffset(g_cls, L"coord_maxCooldown");
    if (g_offPrecMult < 0)   g_offPrecMult = R::FindPropertyOffset(g_cls, L"DL_precMult");
    if (g_offSignalSound < 0) g_offSignalSound = R::FindPropertyOffset(g_cls, L"signalSound");
    if (!g_stopSoundFn) g_stopSoundFn = R::FindFunction(g_cls, L"stopSound");
    if (!g_playSignalFn) g_playSignalFn = R::FindFunction(g_cls, L"playSignal");
    if (!g_spawnDirsFn) g_spawnDirsFn = R::FindFunction(g_cls, L"spawnDirs");
    if (!g_setCanvasFn) g_setCanvasFn = R::FindFunction(g_cls, L"setCanvas");
    if (!g_endCanvasFn) g_endCanvasFn = R::FindFunction(g_cls, L"endCanvas");

    if (all && !g_coreResolved) {
        g_coreResolved = true;
        // A one-shot name-to-offset dump, diffable against the previous build's log and the header
        // reference values.
        char dump[900]; size_t dp = 0; int di = 0;
        for (auto& f : g_fields) {
            dp += snprintf(dump + dp, sizeof(dump) - dp, "%ls=0x%X ", f.name, *f.off);
            if (++di == 12) { UE_LOGI("console_desk: offsets[1/2] -- %s", dump); dp = 0; dump[0] = 0; }
        }
        UE_LOGI("console_desk: offsets[2/2] -- %s", dump);
        int fns = 0;
        for (auto& r : g_refresh) if (r.fn) ++fns;
        UE_LOGI("console_desk: resolved -- 19/19 fields, %d/9 refresh verbs, "
                "writeToCoordLog_2=%s, widget=%s, "
                "catch sig/row/dld=0x%X/0x%X/0x%X rowName/mesh=0x%X/0x%X "
                "form/ping=%s/%s",
                fns,
                g_writeToCoordLogFn ? "yes" : "NO",
                g_offWidget >= 0 ? "yes" : "NO",
                g_offCoordSignalData, g_offDLRow, g_offDLData,
                g_offRowSignalName, g_offRowMesh,
                g_formDownloadFn ? "yes" : "NO", g_playPingSoundFn ? "yes" : "NO");
    }
}

// A typed field dereference with no validity check: every call site is behind its function's
// resolved gate or an explicit offset check.
template <class T>
T* OffPtr(void* obj, int32_t off) {
    return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(obj) + off);
}

// The engine string view: data, count including the terminator, capacity.
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
    // A singleton placed actor, found in the object index's list of its exact class, which the
    // engine's create and delete notifications keep current: a miss costs that list, never a walk of
    // the object array, and every tick's callers miss for as long as no desk exists. Cached and
    // liveness-revalidated.
    struct Found { void* obj; int32_t idx; } found{nullptr, -1};
    if (void* cls = ue_wrap::object_index::ClassByName(L"analogDScreenTest_C")) {
        ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t index) {
            auto* f = static_cast<Found*>(ctx);
            if (f->obj || !obj) return;
            if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
            if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            f->obj = obj;
            f->idx = index;
        }, &found);
    }
    g_instance = found.obj;
    g_instanceIdx = found.idx;
    return g_instance;
}

bool ReadScalars(Scalars& out) {
    void* d = Instance();
    if (!d || !g_coreResolved) return false;
    out.dlPoFilterOffset  = *OffPtr<float>(d, g_offDlPoFilterOffset);
    out.dlFrFilterOffset  = *OffPtr<float>(d, g_offDlFrFilterOffset);
    out.dlPoFilterSpeed   = *OffPtr<float>(d, g_offDlPoFilterSpeed);
    out.dlFrFilterSpeed   = *OffPtr<float>(d, g_offDlFrFilterSpeed);
    out.dlDownloading     = *OffPtr<float>(d, g_offDlDownloading);
    out.dlResDetecPercent = *OffPtr<float>(d, g_offDlResDetecPercent);
    out.coordCooldown     = *OffPtr<float>(d, g_offCoordCooldown);
    out.playVolume        = *OffPtr<int32_t>(d, g_offPlayVolume);
    out.dlPolarityDir     = *OffPtr<int32_t>(d, g_offDlPolarityDir);
    out.compMaxLevel      = *OffPtr<int32_t>(d, g_offCompMaxLevel);
    out.playSelectIndex   = *OffPtr<int32_t>(d, g_offPlaySelectIndex);
    out.dlActiveFrFilter  = *OffPtr<bool>(d, g_offDlActiveFrFilter);
    out.dlActivePoFilter  = *OffPtr<bool>(d, g_offDlActivePoFilter);
    out.activePlay        = *OffPtr<bool>(d, g_offActivePlay);
    out.activeDownload    = *OffPtr<bool>(d, g_offActiveDownload);
    out.activeCoords      = *OffPtr<bool>(d, g_offActiveCoords);
    out.activeComp        = *OffPtr<bool>(d, g_offActiveComp);
    out.coordIsPing       = *OffPtr<bool>(d, g_offCoordIsPing);
    out.remapMin          = *OffPtr<float>(d, g_offRemapValue);
    out.remapMax          = *OffPtr<float>(d, g_offRemapValue + static_cast<int32_t>(sizeof(float)));
    out.visualizerMode    = *OffPtr<bool>(d, g_offVisualizerMode);
    return true;
}

bool ReadFreqPolData(float& frData, float& poData) {
    void* d = Instance();
    if (!d || g_offDLFrData < 0 || g_offDLPoData < 0) return false;
    frData = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offDLFrData);
    poData = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offDLPoData);
    return true;
}

bool WriteScalars(const Scalars& in, uint32_t painters) {
    void* d = Instance();
    if (!d || !g_coreResolved) return false;
    *OffPtr<float>(d, g_offDlPoFilterOffset)  = in.dlPoFilterOffset;
    *OffPtr<float>(d, g_offDlFrFilterOffset)  = in.dlFrFilterOffset;
    *OffPtr<float>(d, g_offDlPoFilterSpeed)   = in.dlPoFilterSpeed;
    *OffPtr<float>(d, g_offDlFrFilterSpeed)   = in.dlFrFilterSpeed;
    *OffPtr<float>(d, g_offDlDownloading)     = in.dlDownloading;
    *OffPtr<float>(d, g_offDlResDetecPercent) = in.dlResDetecPercent;
    *OffPtr<float>(d, g_offCoordCooldown)     = in.coordCooldown;
    *OffPtr<int32_t>(d, g_offPlayVolume)      = in.playVolume;
    *OffPtr<int32_t>(d, g_offDlPolarityDir)   = in.dlPolarityDir;
    *OffPtr<int32_t>(d, g_offCompMaxLevel)    = in.compMaxLevel;
    *OffPtr<int32_t>(d, g_offPlaySelectIndex) = in.playSelectIndex;
    *OffPtr<bool>(d, g_offDlActiveFrFilter) = in.dlActiveFrFilter;
    *OffPtr<bool>(d, g_offDlActivePoFilter) = in.dlActivePoFilter;
    *OffPtr<bool>(d, g_offCoordIsPing)      = in.coordIsPing;
    *OffPtr<float>(d, g_offRemapValue)      = in.remapMin;
    *OffPtr<float>(d, g_offRemapValue + static_cast<int32_t>(sizeof(float))) = in.remapMax;
    // visualizerMode is not written here: setCanvas writes it and draws (ApplyVisualizerMode).
    // Repaint through the blueprint's own painters -- only the ones the caller named. The comp-pane
    // repaint lives in ue_wrap/desk/comp_pane.
    for (size_t i = 0; i < kRefreshCount; ++i) {
        if (!(painters & (1u << i)) || !g_refresh[i].fn) continue;
        ue_wrap::ParamFrame f(g_refresh[i].fn);
        if (f.valid()) ue_wrap::Call(d, f);
    }
    return true;
}

std::wstring ReadCoordLogTail(size_t maxChars) {
    void* d = Instance();
    if (!d || g_offCoordCoordLog2Text < 0) return {};
    auto* s = reinterpret_cast<FStringView*>(
        reinterpret_cast<uint8_t*>(d) + g_offCoordCoordLog2Text);
    if (!s->data || s->num <= 1) return {};
    const size_t len = static_cast<size_t>(s->num - 1);  // num includes the NUL
    const size_t take = len > maxChars ? maxChars : len;
    return std::wstring(s->data + (len - take), take);
}

bool CoordLogTailEquals(const std::wstring& expected, size_t maxChars) {
    void* d = Instance();
    if (!d || g_offCoordCoordLog2Text < 0) return expected.empty();
    auto* s = reinterpret_cast<FStringView*>(
        reinterpret_cast<uint8_t*>(d) + g_offCoordCoordLog2Text);
    if (!s->data || s->num <= 1) return expected.empty();
    const size_t len = static_cast<size_t>(s->num - 1);
    const size_t take = len > maxChars ? maxChars : len;
    if (take != expected.size()) return false;
    return std::wmemcmp(s->data + (len - take), expected.data(), take) == 0;
}

bool AppendCoordLog(const std::wstring& suffix) {
    void* d = Instance();
    if (!d || !g_writeToCoordLogFn || suffix.empty()) return false;
    // The coordinate-log writer: the frame's string may point at our buffer for the call's
    // duration, since the blueprint assignment copies the data into the engine-side field with
    // the engine allocator. The blueprint caps the log at 1000 characters itself.
    ue_wrap::ParamFrame f(g_writeToCoordLogFn);
    if (!f.valid()) return false;
    FStringView v{ const_cast<wchar_t*>(suffix.c_str()),
                   static_cast<int32_t>(suffix.size() + 1),
                   static_cast<int32_t>(suffix.size() + 1) };
    if (!f.SetRaw(L"B", &v, sizeof(v))) return false;
    return ue_wrap::Call(d, f);
}

// The desk widget to atlas chain, the desk-half seams. The refiner pane surface lives in
// ue_wrap/desk/comp_pane.

namespace {

void* DeskAudioComponent(int32_t off) {
    void* d = Instance();
    if (!d || off < 0) return nullptr;
    void* c = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(d) + off);
    return (c && R::IsLive(c)) ? c : nullptr;
}

}  // namespace

// The atlas widget instance (the desk's widget, liveness-checked). Public as the desk-half
// seam for the comp pane's text-block chain; the desk owns its own field chain.
void* AtlasWidget() {
    void* d = Instance();
    if (!d || g_offWidget < 0) return nullptr;
    void* w = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(d) + g_offWidget);
    return (w && R::IsLive(w)) ? w : nullptr;
}

// The desk half of the coordinates-panel instance chain: the raw slot value from the atlas,
// the atlas liveness-checked and the widget itself unvalidated; ue_wrap::coords_panel does
// the class validation and caching half.
void* AtlasUiCoordsSlot() {
    void* atlas = AtlasWidget();
    if (!atlas || g_offAtlasUiCoords < 0) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(atlas) + g_offAtlasUiCoords);
}

// The desk's azimuth and altitude text repaint verb, dispatched for the coordinates panel's
// committed apply (the desk owns its verb table). Declines silently on an unresolved desk or
// verb.
bool CallUpdateCoordCoords() {
    void* desk = Instance();
    if (!desk || !g_refresh[kRefreshCoordCoords].fn) return false;
    ue_wrap::ParamFrame f(g_refresh[kRefreshCoordCoords].fn);
    if (!f.valid()) return false;
    return ue_wrap::Call(desk, f);
}

// Replay the desk's native unfocus verb, the reset-on-release target. The cursor-opacity
// setter dims to a quarter and never hides, so the correct reset is the game's own unfocus,
// not an invented hide or centre; the mirror calls it when the desk claim releases so the
// cursor reverts to exactly the native unoccupied look.
bool CallIntComsUnfocused() {
    void* desk = Instance();
    if (!desk || !g_intComsUnfocusedFn) return false;
    ue_wrap::ParamFrame f(g_intComsUnfocusedFn);
    if (!f.valid()) return false;
    ue_wrap::Call(desk, f);
    return true;
}

// The signal-catch consume surface.

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

bool ReadCoordSignal(CoordSignal& out) { return ReadSignalAt(CoordSignalPtr(), out); }

bool ReadSignalAt(const void* data, CoordSignal& out) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
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

bool WriteCatchReset() {
    void* d = Instance();
    if (!d || g_offDLRow < 0 || g_offDLData < 0 || g_offRowSignalName < 0 || g_offRowMesh < 0 ||
        g_offRowDisplayName < 0 || g_offRowRing < 0 || g_offRowAsActor < 0 || g_offRowType < 0 ||
        g_offRowFreq < 0 || g_offRowQual < 0)
        return false;
    for (int32_t off : g_offRowRot)
        if (off < 0) return false;
    auto* row = reinterpret_cast<uint8_t*>(d) + g_offDLRow;
    // The text first: the one member a failure can leave unwritten, before any other changes. A raw overwrite: the
    // reference the old text held is never released (the objects table's own keeps that text alive), and the minted
    // text's leaks as MintFText says -- one of each per catch.
    if (!ue_wrap::ftext_utils::MintFText(L"none", row + g_offRowDisplayName)) return false;
    SigWrite<R::FName>(row, g_offRowSignalName, R::FName{0, 0});
    SigWrite<void*>(row, g_offRowMesh, nullptr);
    SigWrite<void*>(row, g_offRowRing, nullptr);
    SigWrite<void*>(row, g_offRowAsActor, nullptr);
    // The literal's rotators (pitch, yaw, roll): the second one is (0, 10, 0).
    for (int i = 0; i < 4; ++i) {
        SigWrite<float>(row, g_offRowRot[i] + 0, 0.f);
        SigWrite<float>(row, g_offRowRot[i] + 4, i == 1 ? 10.f : 0.f);
        SigWrite<float>(row, g_offRowRot[i] + 8, 0.f);
    }
    SigWrite<uint8_t>(row, g_offRowType, 0);
    SigWrite<uint8_t>(row, g_offRowFreq, 0);
    SigWrite<uint8_t>(row, g_offRowQual, 0);
    return ue_wrap::signal_dynamic::WriteStructLive(reinterpret_cast<uint8_t*>(d) + g_offDLData,
                                                    ue_wrap::signal_dynamic::Row{});
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
    // The dynamic signal-data member offsets (ue_wrap/desk/signal_dynamic.h).
    decoded = SigRead<float>(dld, ue_wrap::signal_dynamic::kOff_decoded);
    polarity = SigRead<int32_t>(dld, ue_wrap::signal_dynamic::kOff_polarity);
    return true;
}

bool ReadDLSignalKey(uint64_t& out) {
    void* d = Instance();
    if (!d || g_offDLData < 0) return false;
    auto* dld = reinterpret_cast<uint8_t*>(d) + g_offDLData;
    out = SigRead<uint64_t>(dld, ue_wrap::signal_dynamic::kOff_signal);
    return true;
}

namespace {

// One function's parameter offsets, looked up once per function pointer the gate hands over.
struct ParamSlots {
    void*   fn = nullptr;
    int32_t a = -1, b = -1;
};

bool WriteAt(uint8_t* locals, int32_t off, const void* v) {
    if (!locals || off < 0) return false;
    std::memcpy(locals + off, v, 4);
    return true;
}

}  // namespace

bool WriteFormDownloadArgs(void* function, uint8_t* locals, float decoded, int32_t polarity) {
    static ParamSlots s;
    if (function != s.fn) {
        s.fn = function;
        s.a = function ? R::FindParamOffset(function, L"decoded") : -1;
        s.b = function ? R::FindParamOffset(function, L"polarity") : -1;
    }
    if (!locals || s.a < 0 || s.b < 0) return false;
    return WriteAt(locals, s.a, &decoded) && WriteAt(locals, s.b, &polarity);
}

bool WriteInitDownloadPolarity(void* function, uint8_t* locals, int32_t polarity) {
    static ParamSlots s;
    if (function != s.fn) {
        s.fn = function;
        s.a = function ? R::FindParamOffset(function, L"polarity") : -1;
    }
    return WriteAt(locals, s.a, &polarity);
}

std::wstring ReadCoordLogWrite(void* function, const uint8_t* locals) {
    static ParamSlots s;
    if (function != s.fn) {
        s.fn = function;
        s.a = function ? R::FindParamOffset(function, L"B") : -1;
    }
    if (!locals || s.a < 0) return std::wstring();
    return ue_wrap::field_io::ReadFStringAt(locals, s.a);
}

bool ReadDownloadRow(ue_wrap::signal_dynamic::Row& out) {
    void* d = Instance();
    if (!d || g_offDLData < 0) return false;
    return ue_wrap::signal_dynamic::ReadStruct(reinterpret_cast<uint8_t*>(d) + g_offDLData, out);
}

bool CallDeleteActiveSignal() {
    void* const gm = ue_wrap::world_singleton::Gamemode();
    void* const fn = gm ? R::FindDispatchFunctionCached(R::ClassOf(gm), L"deleteActiveSignal") : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(gm, f);
}

bool DownloadMeshValid() {
    void* d = Instance();
    if (!d || g_offDLRow < 0 || g_offRowMesh < 0) return false;
    void* mesh = SigRead<void*>(reinterpret_cast<uint8_t*>(d) + g_offDLRow, g_offRowMesh);
    return mesh != nullptr && R::IsLive(mesh);
}

bool ReadSimOutputs(SimOutputs& out) {
    void* d = Instance();
    if (!d || !g_coreResolved || g_offDLFrData < 0 || g_offDLPoData < 0 || g_offDLData < 0)
        return false;
    out.poOffset  = *OffPtr<float>(d, g_offDlPoFilterOffset);
    out.frOffset  = *OffPtr<float>(d, g_offDlFrFilterOffset);
    out.rate      = *OffPtr<float>(d, g_offDlDownloading);
    out.resDetec  = *OffPtr<float>(d, g_offDlResDetecPercent);
    // The coordinate cooldown is not part of the sim vector; the desk input sync owns it.
    out.frData = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offDLFrData);
    out.poData = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offDLPoData);
    auto* dld = reinterpret_cast<uint8_t*>(d) + g_offDLData;
    out.decoded = SigRead<float>(dld, ue_wrap::signal_dynamic::kOff_decoded);
    return true;
}

bool WriteSimOutputs(const SimOutputs& in) {
    void* d = Instance();
    if (!d || !g_coreResolved || g_offDLFrData < 0 || g_offDLPoData < 0 || g_offDLData < 0)
        return false;
    *OffPtr<float>(d, g_offDlPoFilterOffset) = in.poOffset;
    *OffPtr<float>(d, g_offDlFrFilterOffset) = in.frOffset;
    *OffPtr<float>(d, g_offDlDownloading) = in.rate;
    *OffPtr<float>(d, g_offDlResDetecPercent) = in.resDetec;
    // The coordinate cooldown is not written here: a 10 Hz overwrite erased a client presser's
    // charge. The desk input sync owns it.
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offDLFrData) = in.frData;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offDLPoData) = in.poData;
    auto* dld = reinterpret_cast<uint8_t*>(d) + g_offDLData;
    *reinterpret_cast<float*>(dld + ue_wrap::signal_dynamic::kOff_decoded) = in.decoded;
    // The widget's own tick paints every field above, and the refresh chain paints none of them.
    // The pulse that used to run that chain here is deleted, not conditioned -- see the chain's
    // comment for what it was doing to the player instead. The write is not a crossing's apply:
    // the needle's crossing and canDL run their painters from the host's edges (desk_sim_sync).
    return true;
}

// The desk-input apply surface.

bool ReadMaxCooldown(float& out) {
    void* d = Instance();
    if (!d || g_offMaxCooldown < 0) return false;
    out = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offMaxCooldown);
    return true;
}

bool ReadPrecMult(float& out) {
    void* d = Instance();
    if (!d || g_offPrecMult < 0) return false;
    out = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offPrecMult);
    return true;
}

bool ApplyPlayVolumeEffects(int32_t value) {
    // The atlas volume setter's live-apply half: the signal sound's volume multiplier, the value
    // over ten clamped to a tenth and five. The raw field write is the caller's.
    void* comp = DeskAudioComponent(g_offSignalSound);
    if (!comp) return false;
    float mult = static_cast<float>(value) / 10.0f;
    if (mult < 0.1f) mult = 0.1f;
    if (mult > 5.0f) mult = 5.0f;
    return ue_wrap::component_calls::SetVolumeMultiplier(comp, mult);
}

bool ApplyVisualizerMode(bool spectrogram) {
    void* d = Instance();
    if (!d || !g_setCanvasFn || !g_endCanvasFn) return false;
    ue_wrap::ParamFrame f(g_setCanvasFn);
    if (!f.valid() || !f.Set<bool>(L"visualizerMode", spectrogram) || !ue_wrap::Call(d, f)) return false;
    return ue_wrap::component_calls::CallParamless(d, g_endCanvasFn);
}

bool PlayScanEffects() {
    void* d = Instance();
    if (!d || !g_spawnDirsFn) return false;
    // A null guard: the scan-arrows verb dereferences the coordinates widget's canvas, so skip
    // the replay while the desk's screen widget is not live yet (the caller logs once).
    if (!ue_wrap::coords_panel::Instance()) return false;
    // The beep no longer plays here: the presser's organic ping sound rides the desk
    // sound-effect audio lane, and this replay keeps only the visual (the arrow widgets).
    return ue_wrap::component_calls::CallParamless(d, g_spawnDirsFn);
}

bool CallDeckPlaySignal() {
    void* d = Instance();
    return d && ue_wrap::component_calls::CallParamless(d, g_playSignalFn);
}

bool CallDeckStopSound() {
    void* d = Instance();
    return d && ue_wrap::component_calls::CallParamless(d, g_stopSoundFn);
}

}  // namespace ue_wrap::console_desk
