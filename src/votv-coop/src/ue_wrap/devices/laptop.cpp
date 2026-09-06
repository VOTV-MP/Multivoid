// ue_wrap/laptop.cpp -- see ue_wrap/laptop.h. Offsets resolved live via
// reflection; Alpha 0.9.0-n fallbacks from CXXHeaderDump/laptop.hpp via the
// RE doc (votv-laptop-pc-RE-2026-07-17.md section 1).

#include "ue_wrap/devices/laptop.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cstring>

namespace ue_wrap::laptop {
namespace {

namespace R = reflection;

// Field IO extracted to ue_wrap/core/field_io at v121 (floppybox needed the
// same helpers -- RULE 2, one implementation; the free-what-we-replaced
// doctrine lives there now).
using field_io::FStringView;
using field_io::TArrayView;
using field_io::ReadFStringAt;
using field_io::WriteFStringField;
using field_io::ReadFStringArrayField;
using field_io::WriteFStringArrayField;
using field_io::ReadInt32ArrayField;
using field_io::WriteInt32ArrayField;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ---- resolved state ----
void*   g_cls          = nullptr;  // laptop_C
void*   g_discBaseCls  = nullptr;  // prop_floppyDisc_C
int32_t g_offPowered = -1, g_offIsOpened = -1, g_offAnim = -1;
int32_t g_offFloppyType = -1, g_offZip = -1, g_offReadWrites = -1;
int32_t g_offNametype = -1, g_offObjectData = -1, g_offFloppyData = -1;
int32_t g_offWidget = -1;
int32_t g_offDiscData = -1, g_offDiscReadWrites = -1;
void*   g_fnAction = nullptr;      // actionOptionIndex
void*   g_fnUpdButton = nullptr;   // updButton
void*   g_fnWidgetUpdFloppy = nullptr;  // ui_laptop.updFloppy
// v121 (OPEN-10 quad): buffer fields + the widget rebuild seam.
int32_t g_offFloppyBuffer = -1;     // laptop.floppyBuffer (TArray<FString>)
int32_t g_offFloppyBufUids = -1;    // laptop.floppyBufferUIDs (TArray<int32>)
int32_t g_offWidgetBufferSlots = -1;  // ui_laptop.bufferSlots (TArray<UUserWidget*>)
int32_t g_offBufRowData = -1;       // ui_bufferDatablock_C.data (FString)
void*   g_fnWidgetGenFloppyBuffer = nullptr;  // ui_laptop.genFloppyBuffer
void*   g_fnWidgetRemoveFromParent = nullptr; // UWidget::RemoveFromParent (declaring class!)
bool    g_resolved = false;
uint64_t g_nextResolveTryMs = 0;

void* g_inst = nullptr;
int32_t g_instIdx = -1;

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

    // v121 quad: laptop buffer fields + the widget rebuild seam.
    g_offFloppyBuffer  = R::FindPropertyOffset(cls, L"floppyBuffer");
    g_offFloppyBufUids = R::FindPropertyOffset(cls, L"floppyBufferUIDs");
    if (g_offFloppyBuffer < 0)  { UE_LOGW("laptop: floppyBuffer offset -- fallback 0x4B8"); g_offFloppyBuffer = 0x4B8; }
    if (g_offFloppyBufUids < 0) { UE_LOGW("laptop: floppyBufferUIDs offset -- fallback 0x4D8"); g_offFloppyBufUids = 0x4D8; }
    if (widgetCls) {
        g_offWidgetBufferSlots = R::FindPropertyOffset(widgetCls, L"bufferSlots");
        g_fnWidgetGenFloppyBuffer = R::FindFunction(widgetCls, L"genFloppyBuffer");
    }
    void* bufRowCls = R::FindClass(L"ui_bufferDatablock_C");
    g_offBufRowData = bufRowCls ? R::FindPropertyOffset(bufRowCls, L"data") : -1;
    // RemoveFromParent is DECLARED on the engine UWidget class -- FindFunction
    // is exact-owner (no SuperStruct climb, the FindFunction lesson).
    void* uwidgetCls = R::FindClass(L"Widget");
    g_fnWidgetRemoveFromParent = uwidgetCls ? R::FindFunction(uwidgetCls, L"RemoveFromParent") : nullptr;
    if (g_offWidgetBufferSlots < 0 || !g_fnWidgetGenFloppyBuffer ||
        g_offBufRowData < 0 || !g_fnWidgetRemoveFromParent)
        UE_LOGW("laptop: quad widget seam partial (bufferSlots=0x%X gen=%p rowData=0x%X "
                "removeFromParent=%p) -- quad rebuild degraded",
                g_offWidgetBufferSlots, g_fnWidgetGenFloppyBuffer, g_offBufRowData,
                g_fnWidgetRemoveFromParent);

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

// ---- the file-buffer quad (v121, OPEN-10) ----------------------------------

namespace {

void* WidgetOf(void* inst) {
    if (g_offWidget < 0) return nullptr;
    return *reinterpret_cast<void* const*>(
        reinterpret_cast<const uint8_t*>(inst) + g_offWidget);
}

}  // namespace

bool ReadQuad(BufferQuad& out) {
    void* l = Instance();
    if (!l) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(l);
    out.data       = ReadFStringArrayField(l, g_offFloppyData);
    out.buffer     = ReadFStringArrayField(l, g_offFloppyBuffer);
    out.bufferUids = ReadInt32ArrayField(l, g_offFloppyBufUids);
    out.readWrites = *reinterpret_cast<const int32_t*>(p + g_offReadWrites);
    return true;
}

bool ReadQuadInts(int32_t& fdNum, int32_t& fbNum, int32_t& uidNum, int32_t& rw) {
    void* l = Instance();
    if (!l) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(l);
    auto num = [&](int32_t off) -> int32_t {
        if (off < 0) return 0;
        const auto* v = reinterpret_cast<const TArrayView*>(p + off);
        return (v->data && v->num > 0) ? v->num : 0;
    };
    fdNum  = num(g_offFloppyData);
    fbNum  = num(g_offFloppyBuffer);
    uidNum = num(g_offFloppyBufUids);
    rw = *reinterpret_cast<const int32_t*>(p + g_offReadWrites);
    return true;
}

bool WriteQuadAndRebuild(const BufferQuad& in) {
    void* l = Instance();
    if (!l) return false;
    uint8_t* p = reinterpret_cast<uint8_t*>(l);
    bool wrote = true;
    wrote &= WriteFStringArrayField(l, g_offFloppyData, in.data);
    wrote &= WriteFStringArrayField(l, g_offFloppyBuffer, in.buffer);
    wrote &= WriteInt32ArrayField(l, g_offFloppyBufUids, in.bufferUids);
    *reinterpret_cast<int32_t*>(p + g_offReadWrites) = in.readWrites;
    if (!wrote)
        UE_LOGW("laptop: quad apply PARTIAL (an array mint failed) -- widget rebuilds "
                "from the actual fields; the next canonical re-converges");

    void* widget = WidgetOf(l);
    if (!widget || g_offWidgetBufferSlots < 0 || !g_fnWidgetGenFloppyBuffer ||
        !g_fnWidgetRemoveFromParent) {
        UE_LOGW("laptop: quad fields written but widget rebuild unreachable");
        return false;
    }
    // Teardown: RemoveFromParent each bufferSlots row (native removeBuffer
    // per-row semantics, measured @166-311) then num=0 (buffer kept for the
    // engine's Array_Add reuse -- no free, elements are engine-owned widgets).
    auto* slots = reinterpret_cast<TArrayView*>(
        reinterpret_cast<uint8_t*>(widget) + g_offWidgetBufferSlots);
    if (slots->data && slots->num > 0 && slots->num <= 4096) {
        for (int32_t i = 0; i < slots->num; ++i) {
            void* row = *reinterpret_cast<void* const*>(slots->data + i * 8);
            if (!row || !R::IsLive(row)) continue;
            ParamFrame f(g_fnWidgetRemoveFromParent);
            if (f.valid()) Call(row, f);
        }
    }
    slots->num = 0;
    // Rebuild: the native loadData recipe (genFloppyBuffer + updFloppy).
    {
        ParamFrame f(g_fnWidgetGenFloppyBuffer);
        if (f.valid()) Call(widget, f);
    }
    CallWidgetUpdFloppy(l);
    return true;
}

bool ReadWidgetBufferMirror(int32_t& outCount, uint64_t& outFnv) {
    void* l = Instance();
    if (!l) return false;
    void* widget = WidgetOf(l);
    if (!widget || g_offWidgetBufferSlots < 0 || g_offBufRowData < 0) return false;
    const auto* slots = reinterpret_cast<const TArrayView*>(
        reinterpret_cast<const uint8_t*>(widget) + g_offWidgetBufferSlots);
    outCount = (slots->data && slots->num > 0 && slots->num <= 4096) ? slots->num : 0;
    uint64_t h = 1469598103934665603ull;
    for (int32_t i = 0; i < outCount; ++i) {
        void* row = *reinterpret_cast<void* const*>(slots->data + i * 8);
        if (!row || !R::IsLive(row)) continue;
        const std::wstring s = ReadFStringAt(row, g_offBufRowData);
        const auto* bytes = reinterpret_cast<const uint8_t*>(s.data());
        for (size_t b = 0; b < s.size() * sizeof(wchar_t); ++b) {
            h ^= bytes[b];
            h *= 1099511628211ull;
        }
    }
    outFnv = h;
    return true;
}

void ResetCache() {
    g_inst = nullptr;
    g_instIdx = -1;
}

}  // namespace ue_wrap::laptop
