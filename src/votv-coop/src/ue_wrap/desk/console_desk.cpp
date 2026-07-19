// ue_wrap/console_desk.cpp -- see ue_wrap/console_desk.h.

#include "ue_wrap/desk/console_desk.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/coords_panel.h"
#include "ue_wrap/desk/signal_dynamic.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace ue_wrap::console_desk {
namespace {

namespace R = ue_wrap::reflection;

void* g_cls = nullptr;
void* g_instance = nullptr;
int32_t g_instanceIdx = -1;

// Field offsets, FindPropertyOffset-resolved (recook-robust). NAMED variables
// self-bound to their wire name by the resolve rows below (2026-07-19: the
// positional g_fields table + magic indices were retired -- a removed row
// could silently shift every later index; a named deref is lexically checked
// by the correspondence script and cannot shift).
int32_t g_offDlPoFilterOffset = -1;     // float
int32_t g_offDlFrFilterOffset = -1;     // float
int32_t g_offDlPoFilterSpeed = -1;      // float
int32_t g_offDlFrFilterSpeed = -1;      // float
int32_t g_offDlDownloading = -1;        // float
int32_t g_offDlResDetecPercent = -1;    // float
int32_t g_offCompProgress = -1;         // float
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
// (canDL left the set in v70 -- DERIVED state, see the Scalars header note.)
int32_t g_offCoordCoordLog2Text = -1;   // FString -- the LIVE log (coord_coordLogText is dead)
int32_t g_offCompData0 = -1;            // Fstruct_signalDataDynamic (0x70) -- the loaded signal
int32_t g_offCompDownloading = -1;      // float -- per-tick decode increment (B\s readout)
int32_t g_offCompIsDecodeActive = -1;   // bool -- the decode latch (mirrors never write true)

struct FieldSlot { const wchar_t* name; int32_t* off; };
FieldSlot g_fields[] = {
    { L"DL_poFilterOffset",   &g_offDlPoFilterOffset },
    { L"DL_FrFilterOffset",   &g_offDlFrFilterOffset },
    { L"DL_poFilterSpeed",    &g_offDlPoFilterSpeed },
    { L"DL_FrFilterSpeed",    &g_offDlFrFilterSpeed },
    { L"DL_downloading",      &g_offDlDownloading },
    { L"DL_resDetecPercent",  &g_offDlResDetecPercent },
    { L"comp_progress",       &g_offCompProgress },
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
    { L"comp_data_0",         &g_offCompData0 },
    { L"comp_downloading",    &g_offCompDownloading },
    { L"comp_isDecodeActive", &g_offCompIsDecodeActive },
};
// Scalars marshal: comp_progress and comp_maxLevel both resolve here;
// comp_progress moved OFF the DeskState marshal in v65 (it rides CompState
// from the simulating peer -- see the comp section below).

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

// ---- comp pane substrate (v65) ----
int32_t g_offWidget = -1;            // desk.Widget (Uui_consolesAtlas_C*)
void* g_atlasCls = nullptr;
int32_t g_offTextCompProgress = -1;  // atlas.text_comp_progress (UTextBlock*)
int32_t g_offTextCompProcess = -1;   // atlas.text_comp_process
int32_t g_offAtlasUiCoords = -1;     // atlas.ui_coordinates (Uui_coordinates_C* @0x0588)
int32_t g_offCueWorking = -1;        // desk.computerWorking_Cue (UAudioComponent*)
int32_t g_offCueProg = -1;           // desk.prog
int32_t g_offCueDone = -1;           // desk.Done
// (SetText/SetSound/Activate/SetActive/SetVisibility/SetVolumeMultiplier +
// CallParamless moved to ue_wrap/core/component_calls, 2026-07-19 promotion.)
void* g_sndWorking = nullptr;        // SoundCue 'computerWorking_Cue' (the loop)
void* g_sndWorkingEnd = nullptr;     // SoundCue 'computerWorking_end' (the wind-down)

// (The ui_coordinates coords-panel widget surface moved to
// ue_wrap/desk/coords_panel, 2026-07-19 one-class-per-file split; the desk
// half of its instance chain is AtlasUiCoordsSlot below.)
void* g_intComsUnfocusedFn = nullptr;   // v109: desk intComs_unfocused -- reset-on-release target

// ---- v112 desk-INPUT apply surface (coop/desk_input_sync) ----
// The active_* setter-event side effects (uber [1113-1156]) replicated per
// field: hum + light components, the per-unit extra verbs, the scan effects.
int32_t g_offMaxCooldown = -1;       // coord_maxCooldown @0x0C08 (scan-charge target)
int32_t g_offActiveConsole = -1;     // active_console (bool; the comp setter mirrors active_comp)
int32_t g_offHumPlay = -1;           // computerHum_play   (UAudioComponent*)
int32_t g_offHumDownl = -1;          // computerHum_downl
int32_t g_offHumCoords = -1;         // computerHum_coords
int32_t g_offLightPlay = -1;         // light_play  (scene component -- SetVisibility)
int32_t g_offLightDown = -1;         // light_down
int32_t g_offLightCoord = -1;        // light_coord
int32_t g_offLightComp = -1;         // light_comp
int32_t g_offSignalSound = -1;       // signalSound (UAudioComponent* -- playback volume)
void* g_stopSoundFn = nullptr;           // desk stopSound() (the active_play setter runs it)
void* g_playSignalFn = nullptr;          // desk playSignal() (L6 deck-playback mirror replay)
void* g_finFn = nullptr;                 // desk fin() (OnAudioFinished delegate cb -- L6 PE bracket)
void* g_downloadPlaySignallFn = nullptr; // desk download_playSignall() (the active_download setter)
void* g_setMatsFn = nullptr;             // desk setMats() (screen materials -- the comp setter)
void* g_spawnDirsFn = nullptr;           // desk spawnDirs() (the scan arrows)

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
    if (!g_updCompFn) g_updCompFn = R::FindFunction(g_cls, L"updComp");
    if (!g_intComsUnfocusedFn) g_intComsUnfocusedFn = R::FindFunction(g_cls, L"intComs_unfocused");
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

    // ---- v112 desk-INPUT apply surface ----
    if (g_offMaxCooldown < 0) g_offMaxCooldown = R::FindPropertyOffset(g_cls, L"coord_maxCooldown");
    if (g_offActiveConsole < 0) g_offActiveConsole = R::FindPropertyOffset(g_cls, L"active_console");
    if (g_offHumPlay < 0)    g_offHumPlay = R::FindPropertyOffset(g_cls, L"computerHum_play");
    if (g_offHumDownl < 0)   g_offHumDownl = R::FindPropertyOffset(g_cls, L"computerHum_downl");
    if (g_offHumCoords < 0)  g_offHumCoords = R::FindPropertyOffset(g_cls, L"computerHum_coords");
    if (g_offLightPlay < 0)  g_offLightPlay = R::FindPropertyOffset(g_cls, L"light_play");
    if (g_offLightDown < 0)  g_offLightDown = R::FindPropertyOffset(g_cls, L"light_down");
    if (g_offLightCoord < 0) g_offLightCoord = R::FindPropertyOffset(g_cls, L"light_coord");
    if (g_offLightComp < 0)  g_offLightComp = R::FindPropertyOffset(g_cls, L"light_comp");
    if (g_offSignalSound < 0) g_offSignalSound = R::FindPropertyOffset(g_cls, L"signalSound");
    if (!g_stopSoundFn) g_stopSoundFn = R::FindFunction(g_cls, L"stopSound");
    if (!g_playSignalFn) g_playSignalFn = R::FindFunction(g_cls, L"playSignal");
    if (!g_finFn) g_finFn = R::FindFunction(g_cls, L"fin");
    if (!g_downloadPlaySignallFn)
        g_downloadPlaySignallFn = R::FindFunction(g_cls, L"download_playSignall");
    if (!g_setMatsFn)  g_setMatsFn = R::FindFunction(g_cls, L"setMats");
    if (!g_spawnDirsFn) g_spawnDirsFn = R::FindFunction(g_cls, L"spawnDirs");

    if (all && !g_coreResolved) {
        g_coreResolved = true;
        // One-shot name:offset dump (conversion acceptance: diffable against
        // the offsets the previous build logged + the header reference values).
        char dump[900]; size_t dp = 0; int di = 0;
        for (auto& f : g_fields) {
            dp += snprintf(dump + dp, sizeof(dump) - dp, "%ls=0x%X ", f.name, *f.off);
            if (++di == 12) { UE_LOGI("console_desk: offsets[1/2] -- %s", dump); dp = 0; dump[0] = 0; }
        }
        UE_LOGI("console_desk: offsets[2/2] -- %s", dump);
        int fns = 0;
        for (auto& r : g_refresh) if (r.fn) ++fns;
        UE_LOGI("console_desk: resolved -- 23/23 fields, %d/9 refresh verbs (+updComp=%s), "
                "writeToCoordLog_2=%s, "
                "comp widget/texts/cues=%s/%s/%s sounds=%s/%s, "
                "catch sig/row/dld=0x%X/0x%X/0x%X rowName/mesh=0x%X/0x%X "
                "form/init/ping=%s/%s/%s",
                fns, g_updCompFn ? "yes" : "NO",
                g_writeToCoordLogFn ? "yes" : "NO",
                g_offWidget >= 0 ? "yes" : "NO",
                (g_offTextCompProgress >= 0 && g_offTextCompProcess >= 0) ? "yes" : "NO",
                (g_offCueWorking >= 0 && g_offCueProg >= 0 && g_offCueDone >= 0) ? "yes" : "NO",
                g_sndWorking ? "yes" : "NO", g_sndWorkingEnd ? "yes" : "NO",
                g_offCoordSignalData, g_offDLRow, g_offDLData,
                g_offRowSignalName, g_offRowMesh,
                g_formDownloadFn ? "yes" : "NO", g_initDownloadSignalFn ? "yes" : "NO",
                g_playPingSoundFn ? "yes" : "NO");
    }
}

// Typed field deref. NO validity check -- every call site is behind its
// function's g_coreResolved gate or an explicit off<0 check (R2 census,
// /qf 2026-07-19).
template <class T>
T* OffPtr(void* obj, int32_t off) {
    return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(obj) + off);
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
    return true;
}

bool ReadFreqPolData(float& frData, float& poData) {
    void* d = Instance();
    if (!d || g_offDLFrData < 0 || g_offDLPoData < 0) return false;
    frData = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offDLFrData);
    poData = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offDLPoData);
    return true;
}

bool WriteScalars(const Scalars& in) {
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
    *OffPtr<bool>(d, g_offActivePlay)       = in.activePlay;
    *OffPtr<bool>(d, g_offActiveDownload)   = in.activeDownload;
    *OffPtr<bool>(d, g_offActiveCoords)     = in.activeCoords;
    *OffPtr<bool>(d, g_offActiveComp)       = in.activeComp;
    *OffPtr<bool>(d, g_offCoordIsPing)      = in.coordIsPing;
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

void* DeskAudioComponent(int32_t off) {
    void* d = Instance();
    if (!d || off < 0) return nullptr;
    void* c = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(d) + off);
    return (c && R::IsLive(c)) ? c : nullptr;
}

}  // namespace

bool ReadCompScalars(CompScalars& out) {
    void* d = Instance();
    if (!d || !g_coreResolved) return false;
    out.progress = *OffPtr<float>(d, g_offCompProgress);
    out.downloading = *OffPtr<float>(d, g_offCompDownloading);
    out.decodeActive = *OffPtr<bool>(d, g_offCompIsDecodeActive);
    return true;
}

bool WriteCompScalars(float progress, float downloading) {
    void* d = Instance();
    if (!d || !g_coreResolved) return false;
    *OffPtr<float>(d, g_offCompProgress) = progress;
    *OffPtr<float>(d, g_offCompDownloading) = downloading;
    return true;
}

void* CompDataPtr() {
    void* d = Instance();
    if (!d || g_offCompData0 < 0) return nullptr;
    return reinterpret_cast<uint8_t*>(d) + g_offCompData0;
}

bool UnlatchDecode() {
    void* d = Instance();
    if (!d || !g_coreResolved) return false;
    bool* flag = OffPtr<bool>(d, g_offCompIsDecodeActive);
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
    return ue_wrap::component_calls::SetText(tb, buf);
}

bool PaintCompProcess(const wchar_t* text) {
    void* tb = AtlasTextBlock(g_offTextCompProcess);
    if (!tb) return false;
    return ue_wrap::component_calls::SetText(tb, text);
}

bool CompCueStart() {
    void* c = DeskAudioComponent(g_offCueWorking);
    if (!c) return false;
    if (g_sndWorking) ue_wrap::component_calls::SetSound(c, g_sndWorking);
    return ue_wrap::component_calls::Activate(c);
}

bool CompCueStop() {
    void* c = DeskAudioComponent(g_offCueWorking);
    if (!c) return false;
    if (!g_sndWorkingEnd) return false;  // never Activate the LOOP as a stop
    if (!ue_wrap::component_calls::SetSound(c, g_sndWorkingEnd)) return false;
    return ue_wrap::component_calls::Activate(c);
}

bool CompBeepDone(bool maxed) {
    void* c = DeskAudioComponent(maxed ? g_offCueDone : g_offCueProg);
    if (!c) return false;
    return ue_wrap::component_calls::Activate(c);
}

// The desk half of the coords_panel instance chain (2026-07-19 split): the raw
// desk.Widget -> atlas.ui_coordinates slot value, atlas liveness-checked, the
// widget itself UNVALIDATED -- ue_wrap::coords_panel does the class-validate +
// cache half (widget-concept logic).
void* AtlasUiCoordsSlot() {
    void* atlas = AtlasWidget();
    if (!atlas || g_offAtlasUiCoords < 0) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(atlas) + g_offAtlasUiCoords);
}

// The desk's az/alt text repaint verb, dispatched for coords_panel's committed
// apply (the desk owns its verb table). Declines silently on an unresolved
// desk/verb -- the pre-split WriteDishCommitted tail's exact guard order.
bool CallUpdateCoordCoords() {
    void* desk = Instance();
    if (!desk || !g_refresh[8].fn) return false;  // [8] = updateCoordCoords
    ue_wrap::ParamFrame f(g_refresh[8].fn);
    if (!f.valid()) return false;
    return ue_wrap::Call(desk, f);
}

// v109: replay the desk's native UNFOCUS verb -- the reset-on-release target.
// Measured: setCursorOpacity dims to 0.25 (never hides), so the correct reset is
// the game's OWN intComs_unfocused, NOT an invented hide/center. The mirror calls
// it when the desk claim releases so the cursor reverts to exactly the native
// unoccupied look.
bool CallIntComsUnfocused() {
    void* desk = Instance();
    if (!desk || !g_intComsUnfocusedFn) return false;
    ue_wrap::ParamFrame f(g_intComsUnfocusedFn);
    if (!f.valid()) return false;
    ue_wrap::Call(desk, f);
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
    *OffPtr<float>(d, g_offDlResDetecPercent) = 0.f;  // (@33982)
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

bool DeleteSignalActor() {
    // objectRenderer_C is a per-world singleton; resolve lazily + call the
    // reflected deleteSignalActor() (Public|BlueprintCallable -- impl-RE SS7).
    static void* sCls = nullptr;
    static void* sFn = nullptr;
    if (!sCls) sCls = R::FindClass(L"objectRenderer_C");
    if (sCls && !sFn) sFn = R::FindFunction(sCls, L"deleteSignalActor");
    if (!sFn) return false;
    for (void* obj : R::FindObjectsByClass(L"objectRenderer_C")) {
        if (!obj || !R::IsLive(obj) ||
            R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        ue_wrap::ParamFrame f(sFn);
        if (!f.valid()) return false;
        return ue_wrap::Call(obj, f);
    }
    return false;
}

bool ReadDLSignalKey(uint64_t& out) {
    void* d = Instance();
    if (!d || g_offDLData < 0) return false;
    auto* dld = reinterpret_cast<uint8_t*>(d) + g_offDLData;
    out = SigRead<uint64_t>(dld, ue_wrap::signal_dynamic::kOff_signal);
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
    *OffPtr<float>(d, g_offDlResDetecPercent) = v;
    return true;
}

bool ReadSimOutputs(SimOutputs& out) {
    void* d = Instance();
    if (!d || !g_coreResolved || g_offDLFrData < 0 || g_offDLPoData < 0 || g_offDLData < 0)
        return false;
    out.poOffset  = *OffPtr<float>(d, g_offDlPoFilterOffset);
    out.frOffset  = *OffPtr<float>(d, g_offDlFrFilterOffset);
    out.rate      = *OffPtr<float>(d, g_offDlDownloading);
    out.resDetec  = *OffPtr<float>(d, g_offDlResDetecPercent);
    // (v112: coord_cooldown left the sim vector -- desk_input_sync owns it.)
    out.frData = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offDLFrData);
    out.poData = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offDLPoData);
    auto* dld = reinterpret_cast<uint8_t*>(d) + g_offDLData;
    out.decoded = SigRead<float>(dld, ue_wrap::signal_dynamic::kOff_decoded);
    return true;
}

bool WriteSimOutputs(const SimOutputs& in, bool repaint) {
    void* d = Instance();
    if (!d || !g_coreResolved || g_offDLFrData < 0 || g_offDLPoData < 0 || g_offDLData < 0)
        return false;
    *OffPtr<float>(d, g_offDlPoFilterOffset) = in.poOffset;
    *OffPtr<float>(d, g_offDlFrFilterOffset) = in.frOffset;
    *OffPtr<float>(d, g_offDlDownloading) = in.rate;
    *OffPtr<float>(d, g_offDlResDetecPercent) = in.resDetec;
    // (v112: coord_cooldown is NOT written here -- the 10 Hz overwrite erased a
    // client presser's charge (BUGS-v111 bug 1); desk_input_sync owns it.)
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offDLFrData) = in.frData;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offDLPoData) = in.poData;
    auto* dld = reinterpret_cast<uint8_t*>(d) + g_offDLData;
    *reinterpret_cast<float*>(dld + ue_wrap::signal_dynamic::kOff_decoded) = in.decoded;
    // Repaint only on the throttled pulse (never per-Tick): the interp stream
    // raw-writes every 60 Hz for smoothness and the widget's own Tick repaints
    // the self-painting screens; this pulse (~3 Hz) covers the upd*-only fields.
    if (repaint) {
        for (auto& r : g_refresh) {
            if (!r.fn) continue;
            ue_wrap::ParamFrame f(r.fn);
            if (f.valid()) ue_wrap::Call(d, f);
        }
    }
    return true;
}

// ---- v112 desk-INPUT apply surface ----

bool ReadMaxCooldown(float& out) {
    void* d = Instance();
    if (!d || g_offMaxCooldown < 0) return false;
    out = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(d) + g_offMaxCooldown);
    return true;
}

namespace {

}  // namespace

bool ApplyActiveToggleEffects(int unit, bool value) {
    void* d = Instance();
    if (!d || !g_coreResolved) return false;
    // The native setter events' side-effect blocks, replicated per field (uber
    // [1113-1156]). The fused native setter (powerChanged) runs ALL FIVE units'
    // blocks incl. an unconditional stopSound -- too broad for one field.
    switch (unit) {
    case 0: {  // active_play: stopSound() + light_play + computerHum_play
        ue_wrap::component_calls::CallParamless(d, g_stopSoundFn);
        ue_wrap::component_calls::SetVisibility(DeskAudioComponent(g_offLightPlay), value);
        ue_wrap::component_calls::SetActive(DeskAudioComponent(g_offHumPlay), value);
        return true;
    }
    case 1: {  // active_download: download_playSignall() + light_down + computerHum_downl
        ue_wrap::component_calls::CallParamless(d, g_downloadPlaySignallFn);
        ue_wrap::component_calls::SetVisibility(DeskAudioComponent(g_offLightDown), value);
        ue_wrap::component_calls::SetActive(DeskAudioComponent(g_offHumDownl), value);
        return true;
    }
    case 2: {  // active_coords: light_coord + computerHum_coords
        ue_wrap::component_calls::SetVisibility(DeskAudioComponent(g_offLightCoord), value);
        ue_wrap::component_calls::SetActive(DeskAudioComponent(g_offHumCoords), value);
        return true;
    }
    case 3: {  // active_comp: light_comp + active_console mirror + setMats()
        ue_wrap::component_calls::SetVisibility(DeskAudioComponent(g_offLightComp), value);
        if (g_offActiveConsole >= 0)
            *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(d) + g_offActiveConsole) = value;
        ue_wrap::component_calls::CallParamless(d, g_setMatsFn);
        // (The computerWorking cue transitions stay owned by coop/comp_sync --
        // its CompCueStart/Stop edges; not duplicated here.)
        return true;
    }
    default:
        return false;
    }
}

bool ApplyPlayVolumeEffects(int32_t value) {
    // The atlas setSignalVolume live-apply half: signalSound.SetVolumeMultiplier
    // (FClamp(v/10, 0.1, 5)) -- the raw play_volume field write is the caller's.
    void* comp = DeskAudioComponent(g_offSignalSound);
    if (!comp) return false;
    float mult = static_cast<float>(value) / 10.0f;
    if (mult < 0.1f) mult = 0.1f;
    if (mult > 5.0f) mult = 5.0f;
    return ue_wrap::component_calls::SetVolumeMultiplier(comp, mult);
}

bool PlayScanEffects() {
    void* d = Instance();
    if (!d || !g_spawnDirsFn) return false;
    // Null-guard: spawnDirs derefs ui_coordinates.CanvasPanel_245 -- skip the
    // replay while the desk's screen widget isn't live yet (the caller logs once).
    if (!ue_wrap::coords_panel::Instance()) return false;
    // v115 (RULE 2): the beepLong1 no longer plays here -- the presser's
    // organic playPingSound rides the DeskSndFx audio-seam lane; this replay
    // keeps only the VISUAL (the ui_coordArrow widgets).
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

void* DeckFinFn() { return g_finFn; }

}  // namespace ue_wrap::console_desk
