// ue_wrap/devices/floppy_slot.cpp -- see ue_wrap/devices/floppy_slot.h.

#include "ue_wrap/devices/floppy_slot.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine_component.h"  // SetStaticMesh

#include <chrono>

namespace ue_wrap::floppy_slot {
namespace {

namespace R = reflection;

using field_io::FStringView;
using field_io::TArrayView;
using field_io::ReadFStringAt;
using field_io::WriteFStringField;
using field_io::ReadFStringArrayField;
using field_io::WriteFStringArrayField;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// A garbage TArray/FString header must never drive a loop or a read: both counts below are
// element counts the engine wrote, and anything outside this is not one.
constexpr int32_t kMaxCount = 200000;

// One device class's slot. `hasZip` and `hasNametype` say which members the CLASS declares, so
// an absent member is a fact about the game's class and an unresolved one is a fact about our
// resolve -- the two never look alike. There are no offset fallbacks: a member that does not
// resolve means this is not the class the wrapper was written against, and a guessed offset
// would write into whatever now lives there.
struct Desc {
    const wchar_t* className;
    bool           hasZip;
    bool           hasNametype;
    bool           refreshWidget;  // laptop: Widget -> ui_laptop.updFloppy
    bool           refreshMesh;    // box: floppyMesh -> lib_C::floppyFromType + SetStaticMesh

    int32_t offType       = -1;
    int32_t offReadWrites = -1;
    int32_t offData       = -1;
    int32_t offObjectData = -1;
    int32_t offZip        = -1;
    int32_t offNametype   = -1;
    int32_t offWidget     = -1;
    int32_t offMesh       = -1;
    void*   fnUpdFloppy   = nullptr;

    bool     resolved  = false;
    uint64_t nextTryMs = 0;
};

Desc g_desc[kDeviceKindCount] = {
    { L"laptop_C",    /*zip*/ true,  /*nametype*/ true,  /*widget*/ true,  /*mesh*/ false },
    { L"serverBox_C", /*zip*/ false, /*nametype*/ false, /*widget*/ false, /*mesh*/ true  },
};

Desc* DescOf(DeviceKind kind) {
    const auto i = static_cast<uint8_t>(kind);
    return i < kDeviceKindCount ? &g_desc[i] : nullptr;
}

// ---- the box's mesh swap ---------------------------------------------------------------------
//
// lib_C::floppyFromType(type, getMesh, getType, __WorldContext, out class, out staticMeshes,
// out typeName) indexes a seven-entry mesh list, so a type is a valid index or it is not a type.

void* g_libCdo = nullptr;
void* g_fnFloppyFromType = nullptr;

bool EnsureLib() {
    if (g_libCdo && g_fnFloppyFromType) return true;
    if (!g_libCdo) g_libCdo = R::FindClassDefaultObject(L"lib_C");
    if (!g_fnFloppyFromType) {
        if (void* cls = R::FindClass(L"lib_C"))
            g_fnFloppyFromType = R::FindFunction(cls, L"floppyFromType");
    }
    return g_libCdo && g_fnFloppyFromType;
}

// The static mesh the game would show for this slot type, or null for an empty slot.
void* MeshForType(void* worldContext, int32_t type) {
    // An empty slot shows no mesh. floppyFromType would answer the same, by indexing its list out
    // of range -- which is the game's own array-bounds warning, once per box, on every join.
    if (type < 0) return nullptr;
    if (!EnsureLib()) return nullptr;
    ParamFrame f(g_fnFloppyFromType);
    if (!f.valid()) return nullptr;
    const bool getMesh = true, getType = false;
    if (!f.Set(L"type", type) || !f.Set(L"getMesh", getMesh) || !f.Set(L"getType", getType) ||
        !f.Set(L"__WorldContext", worldContext))
        return nullptr;
    if (!Call(g_libCdo, f)) return nullptr;
    return f.Get<void*>(L"staticMeshes");
}

// Refresh whatever the device shows for its slot. The laptop's widget redraws itself; the box
// has no refresh verb of its own, so the mesh is swapped here exactly as insertFloppy and
// ejectFloppy do it.
void Refresh(const Desc& d, void* device, int32_t type) {
    if (d.refreshWidget && d.fnUpdFloppy && d.offWidget >= 0) {
        void* widget = *reinterpret_cast<void* const*>(
            reinterpret_cast<const uint8_t*>(device) + d.offWidget);
        if (widget) {
            ParamFrame f(d.fnUpdFloppy);
            if (f.valid()) Call(widget, f);
        }
    }
    if (d.refreshMesh && d.offMesh >= 0) {
        void* mesh = *reinterpret_cast<void* const*>(
            reinterpret_cast<const uint8_t*>(device) + d.offMesh);
        if (mesh) engine::SetStaticMesh(mesh, MeshForType(device, type));
    }
}

// ---- the digest ------------------------------------------------------------------------------

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime  = 1099511628211ull;

void Mix(uint64_t& h, const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= kFnvPrime; }
}

// An FString's own bytes, straight out of its header -- no wstring minted.
void MixFString(uint64_t& h, const void* base, int32_t off) {
    if (off < 0) return;
    const auto* v = reinterpret_cast<const FStringView*>(
        reinterpret_cast<const uint8_t*>(base) + off);
    const int32_t n = (v->data && v->num > 0 && v->num <= kMaxCount) ? v->num : 0;
    Mix(h, &n, sizeof(n));
    if (n > 0) Mix(h, v->data, static_cast<size_t>(n) * sizeof(wchar_t));
}

}  // namespace

bool EnsureResolved(DeviceKind kind) {
    Desc* d = DescOf(kind);
    if (!d) return false;
    if (d->resolved) return true;
    const uint64_t now = NowMs();
    if (now < d->nextTryMs) return false;
    d->nextTryMs = now + 1000;

    void* cls = R::FindClass(d->className);
    if (!cls) return false;  // world not loaded yet

    d->offType       = R::FindPropertyOffset(cls, L"floppyType");
    d->offReadWrites = R::FindPropertyOffset(cls, L"floppyReadwrites");
    d->offData       = R::FindPropertyOffset(cls, L"floppyData");
    d->offObjectData = R::FindPropertyOffset(cls, L"floppyObjectData");
    if (d->hasZip)      d->offZip      = R::FindPropertyOffset(cls, L"zip");
    if (d->hasNametype) d->offNametype = R::FindPropertyOffset(cls, L"floppyNametype");
    if (d->refreshWidget) {
        d->offWidget = R::FindPropertyOffset(cls, L"Widget");
        void* widgetCls = R::FindClass(L"ui_laptop_C");
        d->fnUpdFloppy = widgetCls ? R::FindFunction(widgetCls, L"updFloppy") : nullptr;
    }
    if (d->refreshMesh) d->offMesh = R::FindPropertyOffset(cls, L"floppyMesh");

    const bool ok = d->offType >= 0 && d->offReadWrites >= 0 && d->offData >= 0 &&
                    d->offObjectData >= 0 && (!d->hasZip || d->offZip >= 0) &&
                    (!d->hasNametype || d->offNametype >= 0) &&
                    (!d->refreshWidget || d->offWidget >= 0) &&
                    (!d->refreshMesh || d->offMesh >= 0);
    if (!ok) {
        UE_LOGW("floppy_slot: %ls resolution incomplete (type=%d rw=%d data=%d json=%d zip=%d "
                "nametype=%d widget=%d mesh=%d) -- the slot stays off for this device",
                d->className, d->offType, d->offReadWrites, d->offData, d->offObjectData,
                d->offZip, d->offNametype, d->offWidget, d->offMesh);
        return false;
    }
    d->resolved = true;
    UE_LOGI("floppy_slot: %ls resolved (type=0x%X rw=0x%X data=0x%X json=0x%X)",
            d->className, d->offType, d->offReadWrites, d->offData, d->offObjectData);
    return true;
}

bool ReadScalars(DeviceKind kind, void* device, Scalars& out) {
    const Desc* d = DescOf(kind);
    if (!device || !d || !d->resolved) return false;
    const auto* p = reinterpret_cast<const uint8_t*>(device);
    out.floppyType = *reinterpret_cast<const int32_t*>(p + d->offType);
    out.readWrites = *reinterpret_cast<const int32_t*>(p + d->offReadWrites);
    out.zip        = d->offZip >= 0 && p[d->offZip] != 0;
    return true;
}

bool ReadContent(DeviceKind kind, void* device, Content& out) {
    const Desc* d = DescOf(kind);
    if (!device || !d || !d->resolved) return false;
    out.nametype   = d->offNametype >= 0 ? ReadFStringAt(device, d->offNametype) : std::wstring();
    out.objectData = ReadFStringAt(device, d->offObjectData);
    out.data       = ReadFStringArrayField(device, d->offData);
    return true;
}

bool ReadDigest(DeviceKind kind, void* device, uint64_t& out) {
    const Desc* d = DescOf(kind);
    if (!device || !d || !d->resolved) return false;
    const auto* p = reinterpret_cast<const uint8_t*>(device);
    uint64_t h = kFnvOffset;
    Mix(h, p + d->offType, sizeof(int32_t));
    // An empty slot is a type, and its other fields are residue an eject left for its own deferred
    // spawn. Hashing them would raise an edge for a change nobody carries or applies: the game's
    // own eject drops that residue a second later, and a poll reading it would publish twice.
    if (*reinterpret_cast<const int32_t*>(p + d->offType) < 0) { out = h; return true; }
    Mix(h, p + d->offReadWrites, sizeof(int32_t));
    if (d->offZip >= 0) Mix(h, p + d->offZip, 1);
    MixFString(h, device, d->offNametype);
    MixFString(h, device, d->offObjectData);
    const auto* arr = reinterpret_cast<const TArrayView*>(p + d->offData);
    const int32_t rows = (arr->data && arr->num > 0 && arr->num <= kMaxCount) ? arr->num : 0;
    Mix(h, &rows, sizeof(rows));
    for (int32_t i = 0; i < rows; ++i)
        MixFString(h, arr->data, i * static_cast<int32_t>(sizeof(FStringView)));
    out = h;
    return true;
}

bool WriteSlot(DeviceKind kind, void* device, const Scalars& st, const Content& content) {
    const Desc* d = DescOf(kind);
    if (!device || !d || !d->resolved) return false;
    auto* p = reinterpret_cast<uint8_t*>(device);
    *reinterpret_cast<int32_t*>(p + d->offType)       = st.floppyType;
    *reinterpret_cast<int32_t*>(p + d->offReadWrites) = st.readWrites;
    if (d->offZip >= 0) p[d->offZip] = st.zip ? 1 : 0;
    if (d->offNametype >= 0) WriteFStringField(device, d->offNametype, content.nametype);
    WriteFStringField(device, d->offObjectData, content.objectData);
    WriteFStringArrayField(device, d->offData, content.data);
    Refresh(*d, device, st.floppyType);
    return true;
}

// Emptying a slot writes what the device's OWN eject writes at the moment it empties it, and
// nothing else. The eject is two-phase: it clears floppyType and floppyData, and about a second
// later, when the carrier's timeline finishes, the deferred spawn reads floppyReadwrites and
// floppyObjectData to rebuild the disc from. Zeroing those here as well destroys the second
// phase's inputs, and the disc comes back with no content under a freshly minted key. They are
// residue the next insert overwrites; the type is what every busy and eject gate reads.
bool ClearSlot(DeviceKind kind, void* device) {
    const Desc* d = DescOf(kind);
    if (!device || !d || !d->resolved) return false;
    auto* p = reinterpret_cast<uint8_t*>(device);
    *reinterpret_cast<int32_t*>(p + d->offType) = -1;
    WriteFStringArrayField(device, d->offData, {});
    Refresh(*d, device, -1);
    return true;
}

void ResetCache() {
    for (auto& d : g_desc) {
        d.resolved = false;
        d.nextTryMs = 0;
        d.offType = d.offReadWrites = d.offData = d.offObjectData = -1;
        d.offZip = d.offNametype = d.offWidget = d.offMesh = -1;
        d.fnUpdFloppy = nullptr;
    }
    g_libCdo = nullptr;
    g_fnFloppyFromType = nullptr;
}

}  // namespace ue_wrap::floppy_slot
