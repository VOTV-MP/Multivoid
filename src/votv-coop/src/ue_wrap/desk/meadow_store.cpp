// ue_wrap/desk/meadow_store.cpp -- see header.

#include "ue_wrap/desk/meadow_store.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"

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
int32_t g_offSlotSignals = -1;          // saveSlot_C::savedSignals_0
int32_t g_offWidgetLaptop = -1;         // ui_laptop_C::laptop (device back-ptr)
void* g_addSignalFn = nullptr;          // ui_laptop_C::addSignal(data)
void* g_removeSignalFn = nullptr;       // ui_laptop_C::removeSignal(index)
void* g_genSignalListFn = nullptr;      // ui_laptop_C::genSignalList() (zero-arg rebuild)

std::chrono::steady_clock::time_point g_nextResolve{};
bool g_coreResolved = false;

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
        void* gm = world_singleton::Gamemode();
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

// The drill's drivers' members, resolved at their first use.
int32_t g_offSlots = -1;           // ui_laptop_C::slots (TArray<uicomp_signalSlot_C*>, one per row)
int32_t g_offActiveSlot = -1;      // ui_laptop_C::activeSlot (the selection sortSignal moves)
int32_t g_offPlayerInterface = -1; // mainGamemode_C::playerInterface (ui_UI_C)
int32_t g_offSignalNameWin = -1;   // ui_UI_C::umg_signalName (the rename window, ui_signalName_C)
int32_t g_offNameBox = -1;         // ui_signalName_C::Etxt_name (UEditableTextBox)
int32_t g_offActiveInterface = -1; // mainPlayer_C::activeInterface (what the rename window's handler focuses)
// The rename window's button handler: its stub enters the window's ubergraph at the rename. The name is
// the function's own, as the bytecode listing gives it; the pseudo-C++ drops its "K2Node_".
constexpr const wchar_t* kRenameClick =
    L"BndEvt__button_tab_upgrades_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature";

// A driver's missing link, said: the drill's ABANDONED line then names the step, this line the link.
bool Missing(const char* driver, const char* link) {
    UE_LOGW("meadow_store: %s -- %s is not there", driver, link);
    return false;
}

// The live object at `obj`'s member `off` (resolved by name on `obj`'s class when -1), or nullptr.
void* Member(void* obj, int32_t& off, const wchar_t* name) {
    if (!obj) return nullptr;
    if (off < 0) off = R::FindPropertyOffset(R::ClassOf(obj), name);
    if (off < 0) return nullptr;
    void* p = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(obj) + off);
    return (p && R::IsLive(p)) ? p : nullptr;
}

// Row `index`'s slot widget in the laptop's list, or nullptr.
void* SlotWidget(void* w, int32_t index) {
    if (g_offSlots < 0) g_offSlots = R::FindPropertyOffset(R::ClassOf(w), L"slots");
    if (g_offSlots < 0) return nullptr;
    const TArrayView* a = reinterpret_cast<const TArrayView*>(reinterpret_cast<uint8_t*>(w) + g_offSlots);
    if (index < 0 || index >= a->num || !a->data) return nullptr;
    void* s = reinterpret_cast<void* const*>(a->data)[index];
    return (s && R::IsLive(s)) ? s : nullptr;
}

TArrayView* Rows() {
    void* gm = world_singleton::Gamemode();
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
    void* gm = world_singleton::Gamemode();
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

bool MoveRow(int32_t index, int32_t delta) {
    void* w = Widget();
    if (!w) return Missing("MoveRow", "the laptop widget");
    void* slot = SlotWidget(w, index);
    if (g_offActiveSlot < 0) g_offActiveSlot = R::FindPropertyOffset(R::ClassOf(w), L"activeSlot");
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(w), L"sortSignal");
    if (!slot) return Missing("MoveRow", "the row's slot widget");
    if (g_offActiveSlot < 0 || !fn) return Missing("MoveRow", "activeSlot or sortSignal");
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(w) + g_offActiveSlot) = slot;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<int32_t>(L"add", delta) && ue_wrap::Call(w, f);
}

namespace {

// The rename window's commit on a row, inside the laptop: its init on the row's slot widget, the name into
// its text box, then its button.
bool CommitRename(void* w, int32_t index, const wchar_t* name) {
    void* slot = SlotWidget(w, index);
    void* ui = Member(world_singleton::Gamemode(), g_offPlayerInterface, L"playerInterface");
    void* win = Member(ui, g_offSignalNameWin, L"umg_signalName");
    void* box = Member(win, g_offNameBox, L"Etxt_name");
    if (!slot) return Missing("RenameRow", "the row's slot widget");
    if (!win) return Missing("RenameRow", "the rename window (playerInterface.umg_signalName)");
    if (!box) return Missing("RenameRow", "the rename window's text box");
    void* initFn = R::FindDispatchFunctionCached(R::ClassOf(win), L"init");
    void* clickFn = R::FindDispatchFunctionCached(R::ClassOf(win), kRenameClick);
    void* setTextFn = R::FindDispatchFunctionCached(R::ClassOf(box), L"SetText");
    if (!initFn || !clickFn || !setTextFn) return Missing("RenameRow", "init, the button's handler or SetText");
    {
        ue_wrap::ParamFrame f(initFn);
        if (!f.valid() || !f.Set<void*>(L"rename", slot) || !ue_wrap::Call(win, f))
            return Missing("RenameRow", "a call of init");
    }
    uint8_t text[ue_wrap::ftext_utils::kFTextSize];
    if (!ue_wrap::ftext_utils::MintFText(name, text)) return Missing("RenameRow", "an FText of the name");
    {
        ue_wrap::ParamFrame f(setTextFn);
        if (!f.valid() || !f.SetRaw(L"InText", text, ue_wrap::ftext_utils::kFTextSize) || !ue_wrap::Call(box, f))
            return Missing("RenameRow", "a call of SetText");
    }
    ue_wrap::ParamFrame f(clickFn);
    if (!f.valid() || !ue_wrap::Call(win, f)) return Missing("RenameRow", "a call of the button's handler");
    return true;
}

}  // namespace

bool RenameRow(void* player, int32_t index, const wchar_t* name) {
    void* w = Widget();
    if (!w) return Missing("RenameRow", "the laptop widget");
    void* device = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(w) + g_offWidgetLaptop);
    if (!player || !R::IsLive(player)) return Missing("RenameRow", "the local player");
    if (!device || !R::IsLive(device)) return Missing("RenameRow", "the laptop device");
    void* enterFn = R::FindDispatchFunctionCached(R::ClassOf(device), L"enter");
    void* exitFn = R::FindDispatchFunctionCached(R::ClassOf(player), L"Enter Interface");
    if (!enterFn || !exitFn) return Missing("RenameRow", "the laptop's enter or the player's Enter Interface");
    {
        ue_wrap::ParamFrame f(enterFn);
        if (!f.valid() || !f.Set<void*>(L"self2", player) || !ue_wrap::Call(device, f))
            return Missing("RenameRow", "a call of the laptop's enter");
    }
    const bool renamed = Member(player, g_offActiveInterface, L"activeInterface")
                             ? CommitRename(w, index, name)
                             : Missing("RenameRow", "the player's interface after the laptop's enter");
    // The player's own exit from an interface, the call its exit input makes: no interface, "Exit player".
    static wchar_t kExitFrom[] = L"Exit player";
    R::FString from{};
    from.Data = kExitFrom;
    from.Num = static_cast<int32_t>(sizeof(kExitFrom) / sizeof(kExitFrom[0]));
    from.Max = from.Num;
    ue_wrap::ParamFrame f(exitFn);
    if (!f.valid() || !f.Set<void*>(L"activeInterface", nullptr) || !f.SetRaw(L"callFrom", &from, sizeof(from)) ||
        !ue_wrap::Call(player, f))
        return Missing("RenameRow", "a call of the player's exit from the laptop");
    return renamed;
}

}  // namespace ue_wrap::meadow_store
