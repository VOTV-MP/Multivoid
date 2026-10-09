// ue_wrap/devices/laptop.cpp -- see ue_wrap/devices/laptop.h. Offsets resolved live via
// reflection; the Alpha 0.9.0-n fallbacks come from the CXX header dump.

#include "ue_wrap/devices/laptop.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"

#include <chrono>
#include <cstring>

namespace ue_wrap::laptop {
namespace {

namespace R = reflection;

// Field IO lives in ue_wrap/core/field_io, shared with floppybox -- one implementation, and the
// free-what-we-replaced doctrine is documented there.
using field_io::FStringView;
using field_io::TArrayView;
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
int32_t g_offPowered = -1, g_offIsOpened = -1, g_offAnim = -1;
int32_t g_offReadWrites = -1, g_offFloppyData = -1;
int32_t g_offWidget = -1;
// The buffer fields.
int32_t g_offFloppyBuffer = -1;     // laptop.floppyBuffer (TArray<FString>)
int32_t g_offFloppyBufUids = -1;    // laptop.floppyBufferUIDs (TArray<int32>)
int32_t g_offFloppyProcess = -1;   // laptop.floppyProcess (bool): the slot timeline is moving a disc
bool    g_resolved = false;
uint64_t g_nextResolveTryMs = 0;

// The widget seam is resolved from the widget and its rows where they are used: an instance's class
// is loaded by construction, so nothing waits on a widget class by name, and its verbs go through
// the dispatch cache, which climbs to the declaring class (RemoveFromParent is UWidget's).
R::InstanceOffset g_widgetBufferSlots{L"bufferSlots"};  // ui_laptop.bufferSlots (TArray<UUserWidget*>)
R::InstanceOffset g_widgetNearestActor{L"nearestActor"};  // ui_laptop.nearestActor (AActor*)

void* WidgetOf(void* inst) {
    if (g_offWidget < 0) return nullptr;
    return *reinterpret_cast<void* const*>(
        reinterpret_cast<const uint8_t*>(inst) + g_offWidget);
}

bool CallWidgetUpdFloppy(void* inst) {
    void* widget = WidgetOf(inst);
    return widget && component_calls::CallParamlessNamed(widget, L"updFloppy");
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved) return true;
    const uint64_t now = NowMs();
    if (now < g_nextResolveTryMs) return false;
    g_nextResolveTryMs = now + 1000;

    void* cls = R::FindClass(L"laptop_C");
    if (!cls) return false;

    struct Row { const wchar_t* name; int32_t* slot; int32_t fallback; };
    const Row rows[] = {
        { L"powered",          &g_offPowered,     0x42B },
        { L"isOpened",         &g_offIsOpened,    0x418 },
        { L"Anim",             &g_offAnim,        0x42A },
        { L"floppyReadwrites", &g_offReadWrites,  0x4E8 },
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
    const bool action = R::FindDispatchFunctionCached(cls, L"actionOptionIndex") != nullptr;
    if (!action) UE_LOGW("laptop: actionOptionIndex not found -- power replay disabled");

    // The buffer fields.
    g_offFloppyBuffer  = R::FindPropertyOffset(cls, L"floppyBuffer");
    g_offFloppyBufUids = R::FindPropertyOffset(cls, L"floppyBufferUIDs");
    if (g_offFloppyBuffer < 0)  { UE_LOGW("laptop: floppyBuffer offset -- fallback 0x4B8"); g_offFloppyBuffer = 0x4B8; }
    if (g_offFloppyBufUids < 0) { UE_LOGW("laptop: floppyBufferUIDs offset -- fallback 0x4D8"); g_offFloppyBufUids = 0x4D8; }
    // No fallback: a busy flag read from a guessed offset would answer for a field it is not.
    g_offFloppyProcess = R::FindPropertyOffset(cls, L"floppyProcess");
    if (g_offFloppyProcess < 0) UE_LOGW("laptop: floppyProcess not found -- the slot reads as never busy");

    g_resolved = true;
    UE_LOGI("laptop: resolved (isOpened=0x%X floppyData=0x%X readWrites=0x%X action=%d)",
            g_offIsOpened, g_offFloppyData, g_offReadWrites, action ? 1 : 0);
    return true;
}

void* Instance() {
    if (!g_resolved) return nullptr;
    return world_singleton::Find(L"laptop_C");
}

void* TerminalInUse() {
    void* l = Instance();
    void* widget = l ? WidgetOf(l) : nullptr;
    const int32_t off = (widget && R::IsLive(widget)) ? g_widgetNearestActor.Of(widget) : -1;
    if (off < 0) return nullptr;
    void* a = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(widget) + off);
    return (a && R::IsLive(a)) ? a : nullptr;
}

bool FloppyBusy() {
    void* l = Instance();
    if (!l || g_offFloppyProcess < 0) return false;
    return *(reinterpret_cast<const uint8_t*>(l) + g_offFloppyProcess) != 0;
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

bool CallInsertDisc(void* disc) {
    void* l = Instance();
    void* cls = l ? R::ClassOf(l) : nullptr;
    void* fn = cls ? R::FindDispatchFunctionCached(cls, L"processFloppy") : nullptr;
    const int32_t offHitbox = cls ? R::FindPropertyOffset(cls, L"floppyHitbox") : -1;
    if (!fn || offHitbox < 0 || !disc) return false;
    void* hitbox = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(l) + offHitbox);
    if (!hitbox) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"slot", hitbox) && f.Set<void*>(L"player", nullptr) &&
           f.Set<void*>(L"manual", disc) && f.Set<bool>(L"errorNotif", false) && Call(l, f);
}

bool CallEjectDisc() {
    void* l = Instance();
    void* fn = l ? R::FindDispatchFunctionCached(R::ClassOf(l), L"ejectFloppy") : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(l, f);
}

bool CallPowerToggle() {
    void* l = Instance();
    void* fn = l ? R::FindDispatchFunctionCached(R::ClassOf(l), L"actionOptionIndex") : nullptr;
    if (!fn) return false;
    // Empty frame: player=null, hit zeroed, action=b8 semantics ride the
    // 'action' byte param; lookAt null. In-game precedent: beginplayTurnOn's
    // auto-press (uber@815) invokes the same handler with no player context.
    ParamFrame f(fn);
    if (!f.valid()) return false;
    const uint8_t b8 = 8;
    if (!f.SetRaw(L"action", &b8, sizeof(b8))) {
        // Param name differs? decline loudly -- never guess a byte slot.
        UE_LOGW("laptop: actionOptionIndex 'action' param not found -- power replay declined");
        return false;
    }
    return Call(l, f);
}

// ---- the file-buffer quad --------------------------------------------------

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
    const int32_t slotsOff = g_widgetBufferSlots.Of(widget);
    if (slotsOff < 0 || !R::FindDispatchFunctionCached(R::ClassOf(widget), L"genFloppyBuffer")) {
        UE_LOGW("laptop: quad fields written but widget rebuild unreachable");
        return false;
    }
    // Teardown: RemoveFromParent each bufferSlots row (native removeBuffer
    // per-row semantics, measured @166-311) then num=0 (buffer kept for the
    // engine's Array_Add reuse -- no free, elements are engine-owned widgets).
    auto* slots = reinterpret_cast<TArrayView*>(reinterpret_cast<uint8_t*>(widget) + slotsOff);
    if (slots->data && slots->num > 0 && slots->num <= 4096) {
        for (int32_t i = 0; i < slots->num; ++i) {
            void* row = *reinterpret_cast<void* const*>(slots->data + i * 8);
            if (!row || !R::IsLive(row)) continue;
            component_calls::CallParamlessNamed(row, L"RemoveFromParent");
        }
    }
    slots->num = 0;
    // Rebuild: the native loadData recipe (genFloppyBuffer + updFloppy).
    component_calls::CallParamlessNamed(widget, L"genFloppyBuffer");
    CallWidgetUpdFloppy(l);
    return true;
}

}  // namespace ue_wrap::laptop
