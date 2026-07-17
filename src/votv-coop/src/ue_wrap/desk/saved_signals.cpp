// ue_wrap/saved_signals.cpp -- see header.

#include "ue_wrap/desk/saved_signals.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cstring>

namespace ue_wrap::saved_signals {

namespace R = ue_wrap::reflection;
namespace SD = ue_wrap::signal_dynamic;

namespace {

struct TArrayView { uint8_t* data; int32_t num; int32_t max; };

void* g_gamemodeCls = nullptr;
void* g_gamemode = nullptr;
int32_t g_gamemodeIdx = -1;
int32_t g_offSavedSignals = -1;  // mainGamemode_C::savedSignals_0 (@0x0968)
void* g_saveSignalFn = nullptr;
void* g_deleteSignalFn = nullptr;

std::chrono::steady_clock::time_point g_nextResolve{};
bool g_coreResolved = false;

void ResolvePass() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + std::chrono::seconds(2);
    if (!g_gamemodeCls) g_gamemodeCls = R::FindClass(L"mainGamemode_C");
    if (!g_gamemodeCls) return;
    if (g_offSavedSignals < 0)
        g_offSavedSignals = R::FindPropertyOffset(g_gamemodeCls, L"savedSignals_0");
    if (!g_saveSignalFn) g_saveSignalFn = R::FindFunction(g_gamemodeCls, L"saveSignal");
    if (!g_deleteSignalFn) g_deleteSignalFn = R::FindFunction(g_gamemodeCls, L"deleteSignal");
    const bool core = g_offSavedSignals >= 0 && g_saveSignalFn && g_deleteSignalFn;
    if (core && !g_coreResolved) {
        g_coreResolved = true;
        UE_LOGI("saved_signals: resolved (savedSignals_0=0x%X saveSignal=yes deleteSignal=yes)",
                g_offSavedSignals);
    }
}

void* Gamemode() {
    if (g_gamemode && R::IsLiveByIndex(g_gamemode, g_gamemodeIdx)) return g_gamemode;
    g_gamemode = nullptr;
    if (!g_gamemodeCls) return nullptr;
    for (void* obj : R::FindObjectsByClass(L"mainGamemode_C")) {
        if (obj && R::IsLive(obj)) {
            g_gamemode = obj;
            g_gamemodeIdx = R::InternalIndexOf(obj);
            break;
        }
    }
    return g_gamemode;
}

TArrayView* Rows() {
    void* gm = Gamemode();
    if (!gm || g_offSavedSignals < 0) return nullptr;
    return reinterpret_cast<TArrayView*>(reinterpret_cast<uint8_t*>(gm) + g_offSavedSignals);
}

}  // namespace

bool EnsureResolved() {
    ResolvePass();
    return g_coreResolved;
}

int32_t Count() {
    TArrayView* a = Rows();
    if (!a) return -1;
    if (a->num < 0 || a->num > 4096) return -1;  // sanity
    return a->num;
}

bool ReadRow(int32_t index, SD::Row& out) {
    TArrayView* a = Rows();
    if (!a || index < 0 || index >= a->num) return false;
    return SD::ReadStruct(a->data + static_cast<size_t>(index) * SD::kStride, out);
}

bool ReadRowKey(int32_t index, RowKey& out) {
    TArrayView* a = Rows();
    if (!a || index < 0 || index >= a->num) return false;
    const uint8_t* rp = a->data + static_cast<size_t>(index) * SD::kStride;
    std::memcpy(&out.namePtr, rp + SD::kOff_name, sizeof(out.namePtr));
    std::memcpy(&out.idPtr, rp + SD::kOff_id, sizeof(out.idPtr));
    std::memcpy(&out.date, rp + SD::kOff_date, sizeof(out.date));
    std::memcpy(&out.level, rp + SD::kOff_level, sizeof(out.level));
    out.isCopy = *(rp + SD::kOff_isCopy);
    return true;
}

bool ApplySaveSignal(const SD::Row& row) {
    void* gm = Gamemode();
    if (!gm || !g_saveSignalFn) return false;
    ue_wrap::ParamFrame f(g_saveSignalFn);
    if (!f.valid()) return false;
    uint8_t sig[SD::kStride];
    if (!SD::BuildParamBytes(row, sig)) return false;  // row outlives the call (FStrings alias it)
    if (!f.SetRaw(L"signal", sig, SD::kStride)) return false;
    f.Set<bool>(L"new", false);          // dead param (RE: never read)
    f.Set<bool>(L"checkOnly", false);
    f.Set<float>(L"downloadedAtQuality", row.downloadedAtQuality);
    f.Set<bool>(L"selfQuality", true);   // keep the row's own quality verbatim
    if (!ue_wrap::Call(gm, f)) return false;
    const bool succ = f.Get<bool>(L"succ");
    if (!succ)
        UE_LOGW("saved_signals: saveSignal rejected row '%ls' (decoded %.2f / size %.2f)",
                row.name.c_str(), row.decoded, row.size);
    return succ;
}

bool DeleteSignal(int32_t index) {
    TArrayView* a = Rows();
    if (!a || index < 0 || index >= a->num) return false;
    void* gm = Gamemode();
    if (!gm || !g_deleteSignalFn) return false;
    ue_wrap::ParamFrame f(g_deleteSignalFn);
    if (!f.valid()) return false;
    if (!f.Set<int32_t>(L"IndexToRemove", index)) return false;
    return ue_wrap::Call(gm, f);
}

}  // namespace ue_wrap::saved_signals
