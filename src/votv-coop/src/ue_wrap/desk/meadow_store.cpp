// ue_wrap/meadow_store.cpp -- see header.

#include "ue_wrap/desk/meadow_store.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cstring>
#include <vector>

namespace ue_wrap::meadow_store {

namespace R = ue_wrap::reflection;
namespace SD = ue_wrap::signal_dynamic;

namespace {

struct TArrayView { uint8_t* data; int32_t num; int32_t max; };

void* g_gamemodeCls = nullptr;
void* g_laptopWidgetCls = nullptr;      // ui_laptop_C
int32_t g_offGmLaptop = -1;             // mainGamemode_C::laptop (the widget ptr)
int32_t g_offGmSaveSlot = -1;           // mainGamemode_C::saveSlot
int32_t g_offSlotSignals = -1;          // saveSlot_C::savedSignals_0 (@0x680 measured)
int32_t g_offWidgetLaptop = -1;         // ui_laptop_C::laptop (device back-ptr)
void* g_addSignalFn = nullptr;          // ui_laptop_C::addSignal(data)
void* g_removeSignalFn = nullptr;       // ui_laptop_C::removeSignal(index)
void* g_genSignalListFn = nullptr;      // ui_laptop_C::genSignalList() (zero-arg rebuild)

void* g_gamemode = nullptr;
int32_t g_gamemodeIdx = -1;

std::chrono::steady_clock::time_point g_nextResolve{};
bool g_coreResolved = false;

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

void ResolvePass() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + std::chrono::seconds(2);
    if (!g_gamemodeCls) g_gamemodeCls = R::FindClass(L"mainGamemode_C");
    if (!g_laptopWidgetCls) g_laptopWidgetCls = R::FindClass(L"ui_laptop_C");
    if (!g_gamemodeCls || !g_laptopWidgetCls) return;
    if (g_offGmLaptop < 0)
        g_offGmLaptop = R::FindPropertyOffset(g_gamemodeCls, L"laptop");
    if (g_offGmSaveSlot < 0)
        g_offGmSaveSlot = R::FindPropertyOffset(g_gamemodeCls, L"saveSlot");
    if (g_offWidgetLaptop < 0)
        g_offWidgetLaptop = R::FindPropertyOffset(g_laptopWidgetCls, L"laptop");
    if (!g_addSignalFn)
        g_addSignalFn = R::FindFunction(g_laptopWidgetCls, L"addSignal");
    if (!g_removeSignalFn)
        g_removeSignalFn = R::FindFunction(g_laptopWidgetCls, L"removeSignal");
    if (!g_genSignalListFn)
        g_genSignalListFn = R::FindFunction(g_laptopWidgetCls, L"genSignalList");
    // The store offset resolves off the LIVE saveSlot object's class (avoids a
    // FindClass on the save-slot class name; ClassOf is authoritative).
    if (g_offSlotSignals < 0 && g_offGmSaveSlot >= 0) {
        void* gm = Gamemode();
        if (gm) {
            void* slotObj = *reinterpret_cast<void**>(
                reinterpret_cast<uint8_t*>(gm) + g_offGmSaveSlot);
            if (slotObj && R::IsLive(slotObj))
                g_offSlotSignals = R::FindPropertyOffset(R::ClassOf(slotObj), L"savedSignals_0");
        }
    }
    const bool core = g_offGmLaptop >= 0 && g_offGmSaveSlot >= 0 &&
                      g_offSlotSignals >= 0 && g_offWidgetLaptop >= 0 &&
                      g_addSignalFn && g_removeSignalFn && g_genSignalListFn;
    if (core && !g_coreResolved) {
        g_coreResolved = true;
        UE_LOGI("meadow_store: resolved (laptop=0x%X saveSlot=0x%X savedSignals_0=0x%X "
                "widget.laptop=0x%X addSignal=yes removeSignal=yes)",
                g_offGmLaptop, g_offGmSaveSlot, g_offSlotSignals, g_offWidgetLaptop);
    }
}

TArrayView* Rows() {
    void* gm = Gamemode();
    if (!gm || g_offGmSaveSlot < 0 || g_offSlotSignals < 0) return nullptr;
    void* slotObj = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(gm) + g_offGmSaveSlot);
    if (!slotObj || !R::IsLive(slotObj)) return nullptr;
    return reinterpret_cast<TArrayView*>(
        reinterpret_cast<uint8_t*>(slotObj) + g_offSlotSignals);
}

}  // namespace

bool EnsureResolved() {
    ResolvePass();
    return g_coreResolved;
}

void* LaptopWidgetClass() {
    return g_laptopWidgetCls;
}

// Throttled (5 s) diagnostic for the gate's exits -- a silent null here would
// be a dead guard (every exit instrumented).
void LogWidgetGate(const char* why) {
    static std::chrono::steady_clock::time_point next{};
    const auto now = std::chrono::steady_clock::now();
    if (now < next) return;
    next = now + std::chrono::seconds(5);
    UE_LOGW("meadow_store: widget gate NULL -- %s", why);
}

void* Widget() {
    void* gm = Gamemode();
    if (!gm || g_offGmLaptop < 0 || g_offWidgetLaptop < 0) return nullptr;
    void* w = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(gm) + g_offGmLaptop);
    if (!w || !R::IsLive(w)) { LogWidgetGate("gamemode.laptop null/dead"); return nullptr; }
    void* dev = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(w) + g_offWidgetLaptop);
    if (!dev || !R::IsLive(dev)) { LogWidgetGate("widget.laptop back-ptr null/dead"); return nullptr; }
    return w;
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

bool ApplyAddSignal(const SD::Row& row) {
    void* w = Widget();
    if (!w || !g_addSignalFn) return false;
    ue_wrap::ParamFrame f(g_addSignalFn);
    if (!f.valid()) { UE_LOGW("meadow_store: addSignal ParamFrame invalid"); return false; }
    uint8_t sig[SD::kStride];
    if (!SD::BuildParamBytes(row, sig)) {
        UE_LOGW("meadow_store: addSignal BuildParamBytes failed ('%ls')", row.name.c_str());
        return false;
    }
    if (!f.SetRaw(L"data", sig, SD::kStride)) {
        UE_LOGW("meadow_store: addSignal SetRaw('data', %d B) failed", SD::kStride);
        return false;
    }
    if (!ue_wrap::Call(w, f)) {
        UE_LOGW("meadow_store: addSignal ProcessEvent call failed");
        return false;
    }
    return true;
}

bool ApplyRemoveSignal(int32_t index) {
    TArrayView* a = Rows();
    if (!a || index < 0 || index >= a->num) return false;
    void* w = Widget();
    if (!w || !g_removeSignalFn) return false;
    ue_wrap::ParamFrame f(g_removeSignalFn);
    if (!f.valid()) return false;
    if (!f.Set<int32_t>(L"index", index)) return false;
    return ue_wrap::Call(w, f);
}

bool ReorderRows(const int32_t* srcIdx, int32_t n) {
    TArrayView* a = Rows();
    if (!a || !srcIdx || n != a->num || n <= 0) return false;
    std::vector<bool> seen(static_cast<size_t>(n), false);
    for (int32_t i = 0; i < n; ++i) {
        const int32_t s = srcIdx[i];
        if (s < 0 || s >= n || seen[static_cast<size_t>(s)]) return false;
        seen[static_cast<size_t>(s)] = true;
    }
    const size_t stride = static_cast<size_t>(SD::kStride);
    std::vector<uint8_t> tmp(static_cast<size_t>(n) * stride);
    for (int32_t i = 0; i < n; ++i)
        std::memcpy(tmp.data() + static_cast<size_t>(i) * stride,
                    a->data + static_cast<size_t>(srcIdx[i]) * stride, stride);
    std::memcpy(a->data, tmp.data(), tmp.size());
    return true;
}

bool ApplyGenSignalList() {
    void* w = Widget();
    if (!w || !g_genSignalListFn) return false;
    ue_wrap::ParamFrame f(g_genSignalListFn);
    if (!f.valid()) return false;
    return ue_wrap::Call(w, f);
}

}  // namespace ue_wrap::meadow_store
