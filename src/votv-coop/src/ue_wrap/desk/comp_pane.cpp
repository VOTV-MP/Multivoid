// ue_wrap/comp_pane.cpp -- see ue_wrap/comp_pane.h.

#include "ue_wrap/desk/comp_pane.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/console_desk.h"

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace ue_wrap::comp_pane {
namespace {

namespace R = ue_wrap::reflection;

void* g_deskCls = nullptr;
void* g_atlasCls = nullptr;

// The REQUIRED set (the g_required latch): the 4 comp field offsets on the
// desk class. Everything below them resolves opportunistically in the same
// throttled pass with per-function null guards -- the pre-split console_desk
// semantics exactly (its core latch never covered verbs/sounds/atlas either).
int32_t g_offCompProgress = -1;        // comp_progress (float, 0..100)
int32_t g_offCompData0 = -1;           // comp_data_0 (Fstruct_signalDataDynamic 0x70)
int32_t g_offCompDownloading = -1;     // comp_downloading (float; B\s readout)
int32_t g_offCompIsDecodeActive = -1;  // comp_isDecodeActive (bool; the decode latch)
// Opportunistic:
int32_t g_offTextCompProgress = -1;    // atlas.text_comp_progress (UTextBlock*)
int32_t g_offTextCompProcess = -1;     // atlas.text_comp_process
int32_t g_offCueWorking = -1;          // desk.computerWorking_Cue (UAudioComponent*)
int32_t g_offCueProg = -1;             // desk.prog
int32_t g_offCueDone = -1;             // desk.Done
void* g_updCompFn = nullptr;           // updComp(bool Condition)
void* g_sndWorking = nullptr;          // SoundCue 'computerWorking_Cue' (the loop)
void* g_sndWorkingEnd = nullptr;       // SoundCue 'computerWorking_end' (the wind-down)

bool g_required = false;               // fast latch: steady state = one bool load
std::chrono::steady_clock::time_point g_nextResolve{};

void ResolvePass() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + std::chrono::seconds(2);

    if (!g_deskCls) g_deskCls = R::FindClass(L"analogDScreenTest_C");
    if (!g_deskCls) return;
    if (g_offCompProgress < 0)
        g_offCompProgress = R::FindPropertyOffset(g_deskCls, L"comp_progress");
    if (g_offCompData0 < 0)
        g_offCompData0 = R::FindPropertyOffset(g_deskCls, L"comp_data_0");
    if (g_offCompDownloading < 0)
        g_offCompDownloading = R::FindPropertyOffset(g_deskCls, L"comp_downloading");
    if (g_offCompIsDecodeActive < 0)
        g_offCompIsDecodeActive = R::FindPropertyOffset(g_deskCls, L"comp_isDecodeActive");
    if (g_offCueWorking < 0)
        g_offCueWorking = R::FindPropertyOffset(g_deskCls, L"computerWorking_Cue");
    if (g_offCueProg < 0) g_offCueProg = R::FindPropertyOffset(g_deskCls, L"prog");
    if (g_offCueDone < 0) g_offCueDone = R::FindPropertyOffset(g_deskCls, L"Done");
    if (!g_updCompFn) g_updCompFn = R::FindFunction(g_deskCls, L"updComp");
    if (!g_atlasCls) g_atlasCls = R::FindClass(L"ui_consolesAtlas_C");
    if (g_atlasCls) {
        if (g_offTextCompProgress < 0)
            g_offTextCompProgress = R::FindPropertyOffset(g_atlasCls, L"text_comp_progress");
        if (g_offTextCompProcess < 0)
            g_offTextCompProcess = R::FindPropertyOffset(g_atlasCls, L"text_comp_process");
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

    if (!g_required &&
        g_offCompProgress >= 0 && g_offCompData0 >= 0 &&
        g_offCompDownloading >= 0 && g_offCompIsDecodeActive >= 0) {
        g_required = true;
        UE_LOGI("comp_pane: resolved -- prog/data/dl/act=0x%X/0x%X/0x%X/0x%X, "
                "updComp=%s texts=%s/%s cues=%s/%s/%s sounds=%s/%s",
                g_offCompProgress, g_offCompData0, g_offCompDownloading,
                g_offCompIsDecodeActive,
                g_updCompFn ? "yes" : "NO",
                g_offTextCompProgress >= 0 ? "yes" : "NO",
                g_offTextCompProcess >= 0 ? "yes" : "NO",
                g_offCueWorking >= 0 ? "yes" : "NO",
                g_offCueProg >= 0 ? "yes" : "NO",
                g_offCueDone >= 0 ? "yes" : "NO",
                g_sndWorking ? "yes" : "NO", g_sndWorkingEnd ? "yes" : "NO");
    }
}

// The desk actor (owned by console_desk; one instance cache in the tree).
// Every public entry nudges the throttled resolver until the latch trips.
void* Desk() {
    if (!g_required) ResolvePass();
    return ue_wrap::console_desk::Instance();
}

template <class T>
T* OffPtr(void* obj, int32_t off) {
    return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(obj) + off);
}

void* AtlasTextBlock(int32_t off) {
    void* atlas = ue_wrap::console_desk::AtlasWidget();
    if (!atlas || off < 0) return nullptr;
    void* tb = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(atlas) + off);
    return (tb && R::IsLive(tb)) ? tb : nullptr;
}

void* DeskAudioComponent(int32_t off) {
    void* d = ue_wrap::console_desk::Instance();
    if (!d || off < 0) return nullptr;
    void* c = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(d) + off);
    return (c && R::IsLive(c)) ? c : nullptr;
}

}  // namespace

bool ReadCompScalars(CompScalars& out) {
    void* d = Desk();
    if (!d || !g_required) return false;
    out.progress = *OffPtr<float>(d, g_offCompProgress);
    out.downloading = *OffPtr<float>(d, g_offCompDownloading);
    out.decodeActive = *OffPtr<bool>(d, g_offCompIsDecodeActive);
    return true;
}

bool WriteCompScalars(float progress, float downloading) {
    void* d = Desk();
    if (!d || !g_required) return false;
    *OffPtr<float>(d, g_offCompProgress) = progress;
    *OffPtr<float>(d, g_offCompDownloading) = downloading;
    return true;
}

void* CompDataPtr() {
    void* d = Desk();
    if (!d || g_offCompData0 < 0) return nullptr;
    return reinterpret_cast<uint8_t*>(d) + g_offCompData0;
}

bool UnlatchDecode() {
    void* d = Desk();
    if (!d || !g_required) return false;
    bool* flag = OffPtr<bool>(d, g_offCompIsDecodeActive);
    if (!*flag) return true;  // not latched: nothing to do
    *flag = false;
    CompCueStop();
    PaintCompProcess(L"idle");
    return true;
}

bool UpdComp(bool hasData) {
    void* d = Desk();
    if (!d || !g_updCompFn) return false;
    ue_wrap::ParamFrame f(g_updCompFn);
    if (!f.valid()) return false;
    f.Set<bool>(L"Condition", hasData);
    return ue_wrap::Call(d, f);
}

bool PaintCompProgress(float progress) {
    if (!g_required) ResolvePass();
    void* tb = AtlasTextBlock(g_offTextCompProgress);
    if (!tb) return false;
    // The native paint: Conv_FloatToText(min 3 integral / 3,3 fractional) + "%".
    wchar_t buf[24];
    swprintf(buf, 24, L"%07.3f%%", progress);
    return ue_wrap::component_calls::SetText(tb, buf);
}

bool PaintCompProcess(const wchar_t* text) {
    if (!g_required) ResolvePass();
    void* tb = AtlasTextBlock(g_offTextCompProcess);
    if (!tb) return false;
    return ue_wrap::component_calls::SetText(tb, text);
}

bool CompCueStart() {
    if (!g_required) ResolvePass();
    void* c = DeskAudioComponent(g_offCueWorking);
    if (!c) return false;
    if (g_sndWorking) ue_wrap::component_calls::SetSound(c, g_sndWorking);
    return ue_wrap::component_calls::Activate(c);
}

bool CompCueStop() {
    if (!g_required) ResolvePass();
    void* c = DeskAudioComponent(g_offCueWorking);
    if (!c) return false;
    if (!g_sndWorkingEnd) return false;  // never Activate the LOOP as a stop
    if (!ue_wrap::component_calls::SetSound(c, g_sndWorkingEnd)) return false;
    return ue_wrap::component_calls::Activate(c);
}

bool CompBeepDone(bool maxed) {
    if (!g_required) ResolvePass();
    void* c = DeskAudioComponent(maxed ? g_offCueDone : g_offCueProg);
    if (!c) return false;
    return ue_wrap::component_calls::Activate(c);
}

}  // namespace ue_wrap::comp_pane
