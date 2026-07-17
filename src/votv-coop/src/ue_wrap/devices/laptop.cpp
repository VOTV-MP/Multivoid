// ue_wrap/laptop.cpp -- see ue_wrap/laptop.h. Offsets resolved live via
// reflection; Alpha 0.9.0-n fallbacks from CXXHeaderDump/laptop.hpp via the
// RE doc (votv-laptop-pc-RE-2026-07-17.md section 1).

#include "ue_wrap/devices/laptop.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fstring_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cstring>

namespace ue_wrap::laptop {
namespace {

namespace R = reflection;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct FStringView { wchar_t* data; int32_t num; int32_t max; };
struct TArrayView  { uint8_t* data; int32_t num; int32_t max; };

std::wstring ReadFStringAt(const void* base, int32_t off) {
    const auto* v = reinterpret_cast<const FStringView*>(
        reinterpret_cast<const uint8_t*>(base) + off);
    if (!v->data || v->num <= 1) return std::wstring();
    return std::wstring(v->data, static_cast<size_t>(v->num - 1));
}

// ---- resolved state ----
void*   g_cls          = nullptr;  // laptop_C
void*   g_discBaseCls  = nullptr;  // prop_floppyDisc_C
void*   g_zipDiscCls   = nullptr;  // prop_floppyDisc_Wh_C
int32_t g_offPowered = -1, g_offIsOpened = -1, g_offAnim = -1;
int32_t g_offFloppyType = -1, g_offZip = -1, g_offReadWrites = -1;
int32_t g_offNametype = -1, g_offObjectData = -1, g_offFloppyData = -1;
int32_t g_offWidget = -1;
int32_t g_offDiscData = -1, g_offDiscReadWrites = -1;
void*   g_fnAction = nullptr;      // actionOptionIndex
void*   g_fnUpdButton = nullptr;   // updButton
void*   g_fnWidgetUpdFloppy = nullptr;  // ui_laptop.updFloppy
bool    g_resolved = false;
uint64_t g_nextResolveTryMs = 0;

void* g_inst = nullptr;
int32_t g_instIdx = -1;

// Free the engine-allocated buffer behind an FString header slot (no-op on
// null/empty). The fstring_utils PIN doctrine ("leak it, the engine's later
// reassign frees it") holds only when writing into a FRESH buffer; these
// laptop fields are overwritten REPEATEDLY on the same live instance across
// a session, and on a non-presser peer no native reassign ever runs between
// our writes -- so WE must free what WE replaced (perf audit v116 finding 1;
// EngineFree is GMalloc-matched, engine_heap.cpp doctrine).
void FreeFStringSlot(void* header16) {
    auto* v = reinterpret_cast<FStringView*>(header16);
    if (v->data) R::EngineFree(v->data);
    v->data = nullptr; v->num = 0; v->max = 0;
}

void FreeFStringArraySlot(TArrayView* view) {
    if (view->data) {
        for (int32_t i = 0; i < view->num; ++i)
            FreeFStringSlot(view->data + i * 16);
        R::EngineFree(view->data);
    }
    view->data = nullptr; view->num = 0; view->max = 0;
}

bool WriteFStringField(void* base, int32_t off, const std::wstring& s) {
    if (off < 0) return false;
    uint8_t* slot = reinterpret_cast<uint8_t*>(base) + off;
    const FStringView old = *reinterpret_cast<const FStringView*>(slot);
    if (!fstring_utils::MintFString(s, slot)) return false;  // failure leaves the old value intact
    if (old.data) R::EngineFree(old.data);
    return true;
}

// Engine-side TArray<FString> mint: EngineAlloc the element buffer (16 B per
// FString header), MintFString each element into it, then swap the header and
// free the REPLACED buffer + its element strings (see FreeFStringSlot note).
// Empty input writes the null array (old buffer freed the same way).
bool WriteFStringArrayField(void* base, int32_t off, const std::vector<std::wstring>& in) {
    if (off < 0) return false;
    auto* view = reinterpret_cast<TArrayView*>(reinterpret_cast<uint8_t*>(base) + off);
    if (in.empty()) {
        FreeFStringArraySlot(view);
        return true;
    }
    const size_t bytes = in.size() * 16;
    uint8_t* buf = static_cast<uint8_t*>(R::EngineAlloc(bytes));
    if (!buf) return false;
    std::memset(buf, 0, bytes);
    for (size_t i = 0; i < in.size(); ++i) {
        if (!fstring_utils::MintFString(in[i], buf + i * 16)) {
            // Roll back the partial mint (elements already minted + the buffer);
            // the old array stays untouched.
            for (size_t j = 0; j < i; ++j) FreeFStringSlot(buf + j * 16);
            R::EngineFree(buf);
            return false;
        }
    }
    const TArrayView old = *view;
    view->data = buf;
    view->num = static_cast<int32_t>(in.size());
    view->max = static_cast<int32_t>(in.size());
    TArrayView oldCopy = old;
    FreeFStringArraySlot(&oldCopy);
    return true;
}

std::vector<std::wstring> ReadFStringArrayField(const void* base, int32_t off) {
    std::vector<std::wstring> out;
    if (off < 0) return out;
    const auto* view = reinterpret_cast<const TArrayView*>(
        reinterpret_cast<const uint8_t*>(base) + off);
    if (!view->data || view->num <= 0 || view->num > 4096) return out;
    for (int32_t i = 0; i < view->num; ++i)
        out.push_back(ReadFStringAt(view->data + i * 16, 0));
    return out;
}

bool CallWidgetUpdFloppy(void* inst) {
    if (!g_fnWidgetUpdFloppy || g_offWidget < 0) return false;
    void* widget = *reinterpret_cast<void* const*>(
        reinterpret_cast<const uint8_t*>(inst) + g_offWidget);
    if (!widget) return false;
    ParamFrame f(g_fnWidgetUpdFloppy);
    if (!f.valid()) return false;
    return Call(widget, f);
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved) return true;
    const uint64_t now = NowMs();
    if (now < g_nextResolveTryMs) return false;
    g_nextResolveTryMs = now + 1000;

    void* cls = R::FindClass(L"laptop_C");
    void* discCls = R::FindClass(L"prop_floppyDisc_C");
    if (!cls || !discCls) return false;
    g_zipDiscCls = R::FindClass(L"prop_floppyDisc_Wh_C");  // optional (zip slot)

    struct Row { const wchar_t* name; int32_t* slot; int32_t fallback; };
    const Row rows[] = {
        { L"powered",          &g_offPowered,     0x42B },
        { L"isOpened",         &g_offIsOpened,    0x418 },
        { L"Anim",             &g_offAnim,        0x42A },
        { L"floppyType",       &g_offFloppyType,  0x450 },
        { L"zip",              &g_offZip,         0x4F8 },
        { L"floppyReadwrites", &g_offReadWrites,  0x4E8 },
        { L"floppyNametype",   &g_offNametype,    0x4A0 },
        { L"floppyObjectData", &g_offObjectData,  0x4C8 },
        { L"floppyData",       &g_offFloppyData,  0x458 },
        { L"Widget",           &g_offWidget,      0x420 },
    };
    for (const Row& r : rows) {
        *r.slot = R::FindPropertyOffset(cls, r.name);
        if (*r.slot < 0) {
            UE_LOGW("laptop: offset '%ls' not found -- fallback 0x%X", r.name, r.fallback);
            *r.slot = r.fallback;
        }
    }
    g_offDiscData       = R::FindPropertyOffset(discCls, L"Data");
    g_offDiscReadWrites = R::FindPropertyOffset(discCls, L"readWrites");
    if (g_offDiscData < 0)       { UE_LOGW("laptop: disc Data offset -- fallback 0x368"); g_offDiscData = 0x368; }
    if (g_offDiscReadWrites < 0) { UE_LOGW("laptop: disc readWrites offset -- fallback 0x37C"); g_offDiscReadWrites = 0x37C; }

    g_fnAction    = R::FindFunction(cls, L"actionOptionIndex");
    g_fnUpdButton = R::FindFunction(cls, L"updButton");
    void* widgetCls = R::FindClass(L"ui_laptop_C");
    g_fnWidgetUpdFloppy = widgetCls ? R::FindFunction(widgetCls, L"updFloppy") : nullptr;
    if (!g_fnAction)
        UE_LOGW("laptop: actionOptionIndex not found -- power replay disabled");

    g_cls = cls; g_discBaseCls = discCls;
    g_resolved = true;
    UE_LOGI("laptop: resolved (isOpened=0x%X floppyType=0x%X objectData=0x%X action=%p)",
            g_offIsOpened, g_offFloppyType, g_offObjectData, g_fnAction);
    return true;
}

void* Instance() {
    if (!g_resolved) return nullptr;
    if (g_inst && R::IsLiveByIndex(g_inst, g_instIdx)) return g_inst;
    g_inst = nullptr; g_instIdx = -1;
    for (void* obj : R::FindObjectsByClass(L"laptop_C")) {
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) {
            g_inst = obj;
            g_instIdx = R::InternalIndexOf(obj);
            return obj;
        }
    }
    return nullptr;
}

bool ReadPower(PowerState& out) {
    void* l = Instance();
    if (!l) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(l);
    out.powered  = p[g_offPowered]  != 0;
    out.isOpened = p[g_offIsOpened] != 0;
    out.anim     = p[g_offAnim]     != 0;
    return true;
}

bool CallPowerToggle() {
    void* l = Instance();
    if (!l || !g_fnAction) return false;
    // Empty frame: player=null, hit zeroed, action=b8 semantics ride the
    // 'action' byte param; lookAt null. In-game precedent: beginplayTurnOn's
    // auto-press (uber@815) invokes the same handler with no player context.
    ParamFrame f(g_fnAction);
    if (!f.valid()) return false;
    const uint8_t b8 = 8;
    if (!f.SetRaw(L"action", &b8, sizeof(b8))) {
        // Param name differs? decline loudly -- never guess a byte slot.
        UE_LOGW("laptop: actionOptionIndex 'action' param not found -- power replay declined");
        return false;
    }
    return Call(l, f);
}

bool ReadSlot(SlotState& out) {
    void* l = Instance();
    if (!l) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(l);
    out.floppyType = *reinterpret_cast<const int32_t*>(p + g_offFloppyType);
    out.zip        = p[g_offZip] != 0;
    out.readWrites = *reinterpret_cast<const int32_t*>(p + g_offReadWrites);
    return true;
}

bool ReadSlotContent(SlotContent& out) {
    void* l = Instance();
    if (!l) return false;
    out.nametype   = ReadFStringAt(l, g_offNametype);
    out.objectData = ReadFStringAt(l, g_offObjectData);
    out.data       = ReadFStringArrayField(l, g_offFloppyData);
    return true;
}

bool WriteSlotScalars(const SlotState& st) {
    void* l = Instance();
    if (!l) return false;
    uint8_t* p = reinterpret_cast<uint8_t*>(l);
    *reinterpret_cast<int32_t*>(p + g_offFloppyType) = st.floppyType;
    p[g_offZip] = st.zip ? 1 : 0;
    *reinterpret_cast<int32_t*>(p + g_offReadWrites) = st.readWrites;
    CallWidgetUpdFloppy(l);
    return true;
}

bool WriteSlot(const SlotState& st, const SlotContent& content) {
    void* l = Instance();
    if (!l) return false;
    uint8_t* p = reinterpret_cast<uint8_t*>(l);
    *reinterpret_cast<int32_t*>(p + g_offFloppyType) = st.floppyType;
    p[g_offZip] = st.zip ? 1 : 0;
    *reinterpret_cast<int32_t*>(p + g_offReadWrites) = st.readWrites;
    WriteFStringField(l, g_offNametype, content.nametype);
    WriteFStringField(l, g_offObjectData, content.objectData);
    WriteFStringArrayField(l, g_offFloppyData, content.data);
    CallWidgetUpdFloppy(l);
    return true;
}

bool ClearSlot() {
    void* l = Instance();
    if (!l) return false;
    uint8_t* p = reinterpret_cast<uint8_t*>(l);
    *reinterpret_cast<int32_t*>(p + g_offFloppyType) = -1;
    *reinterpret_cast<int32_t*>(p + g_offReadWrites) = -1;
    p[g_offZip] = 0;
    WriteFStringField(l, g_offNametype, std::wstring());
    WriteFStringField(l, g_offObjectData, std::wstring());
    WriteFStringArrayField(l, g_offFloppyData, {});
    CallWidgetUpdFloppy(l);
    return true;
}

bool IsDiscClass(void* cls) {
    if (!cls || !g_discBaseCls) return false;
    if (cls == g_discBaseCls) return true;
    void* base[1] = { g_discBaseCls };
    return R::IsDescendantOfAny(cls, base, 1);
}

bool IsZipDiscClass(void* cls) {
    if (!cls || !g_zipDiscCls) return false;
    if (cls == g_zipDiscCls) return true;
    void* base[1] = { g_zipDiscCls };
    return R::IsDescendantOfAny(cls, base, 1);
}

bool ReadDiscContent(void* discActor, DiscContent& out) {
    if (!discActor || !g_resolved) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(discActor);
    out.readWrites = *reinterpret_cast<const int32_t*>(p + g_offDiscReadWrites);
    out.data       = ReadFStringArrayField(discActor, g_offDiscData);
    return true;
}

bool WriteDiscContent(void* discActor, const DiscContent& in) {
    if (!discActor || !g_resolved) return false;
    uint8_t* p = reinterpret_cast<uint8_t*>(discActor);
    *reinterpret_cast<int32_t*>(p + g_offDiscReadWrites) = in.readWrites;
    return WriteFStringArrayField(discActor, g_offDiscData, in.data);
}

void ResetCache() {
    g_inst = nullptr;
    g_instIdx = -1;
}

}  // namespace ue_wrap::laptop
