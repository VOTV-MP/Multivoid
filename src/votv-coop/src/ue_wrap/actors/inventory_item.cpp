// ue_wrap/actors/inventory_item.cpp -- see ue_wrap/actors/inventory_item.h.

#include "ue_wrap/actors/inventory_item.h"

#include "ue_wrap/actors/save_record.h"   // ReadArr, WriteArrHeader, AllocZeroed, the FName field helpers
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/fstring_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"

#include <chrono>
#include <cstring>
#include <cwctype>

namespace ue_wrap::inventory_item {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;

// FSoftObjectPtr (TPersistentObjectPtr<FSoftObjectPath>): FWeakObjectPtr, int32 TagAtLastTest and its padding, then
// FSoftObjectPath {FName AssetPathName, FString SubPathString}. A zeroed weak pointer is null, and the path is what
// the engine resolves the class from.
constexpr int32_t kSoftPtrSize = 0x28;
constexpr int32_t kSoftPath    = 0x10;
constexpr int32_t kSoftSubPath = 0x18;
constexpr int32_t kAlign = 16;

struct Member {
    const wchar_t* prefix;
    const wchar_t* fieldClass;   // the FFieldClass name the member must have
    int32_t size;                // its ElementSize
    int32_t off = -1;
};

// struct_betterSave
enum B { kClass, kTransform, kName, kTag, kKey, kAttachKey, kActorData, kIndex, kBCount };
Member g_b[kBCount] = {
    {L"class_",     L"SoftClassProperty", kSoftPtrSize},
    {L"transform_", L"StructProperty",    static_cast<int32_t>(sizeof(ue_wrap::FTransform))},
    {L"name_",      L"NameProperty",      8},
    {L"tag_",       L"NameProperty",      8},
    {L"key_",       L"NameProperty",      8},
    {L"attachKey_", L"NameProperty",      8},
    {L"actorData_", L"ArrayProperty",     16},
    {L"index_",     L"IntProperty",       4},
};
// struct_inventoryItem
enum I { kPosition, kRotation, kCompartment, kData, kAmount, kICount };
Member g_i[kICount] = {
    {L"position_",    L"StructProperty", 8},
    {L"rotation_",    L"FloatProperty",  4},
    {L"compartment_", L"ByteProperty",   1},
    {L"data_",        L"StructProperty", 0},   // the betterSave struct's size, checked against it
    {L"amount_",      L"ByteProperty",   1},
};

bool g_resolved = false;
int32_t g_stride = 0;
int32_t g_betterSize = 0;

bool PrefixMatch(const std::wstring& name, const wchar_t* prefix) {
    size_t i = 0;
    for (; prefix[i]; ++i)
        if (i >= name.size() || std::towlower(name[i]) != std::towlower(prefix[i])) return false;
    return true;
}

// Fill each member's offset from `st`, checking its size and its FField class. False, with one line, on the first
// that does not check out.
bool ResolveMembers(void* st, Member* members, size_t n, const char* structName) {
    const std::vector<R::StructFieldInfo> fields = R::EnumerateStructFields(st);
    for (size_t m = 0; m < n; ++m) {
        Member& mem = members[m];
        const R::StructFieldInfo* hit = nullptr;
        for (const auto& f : fields)
            if (PrefixMatch(f.name, mem.prefix)) { hit = &f; break; }
        int32_t off = -1;
        std::wstring cls;
        if (!hit || !R::PropertyOffsetAndClass(st, hit->name.c_str(), off, cls) || cls != mem.fieldClass ||
            (mem.size > 0 && hit->size != mem.size)) {
            UE_LOGW("inventory_item: %s.%ls does not check out (found=%d class=%ls size=%d, want %ls/%d) -- "
                    "inventory entries are not read or written", structName, mem.prefix, hit ? 1 : 0, cls.c_str(),
                    hit ? hit->size : -1, mem.fieldClass, mem.size);
            return false;
        }
        mem.off = off;
        mem.size = hit->size;
    }
    return true;
}

std::wstring ReadSoftPath(const uint8_t* p) {
    std::wstring path = SR::ReadFNameAt(p, kSoftPath);
    const std::wstring sub = ue_wrap::field_io::ReadFStringAt(p, kSoftSubPath);
    if (!path.empty() && !sub.empty()) path += L":" + sub;
    return path;
}

bool WriteSoftPath(uint8_t* p, const std::wstring& full) {
    const size_t colon = full.find(L':');
    SR::WriteFNameField(p + kSoftPath, colon == std::wstring::npos ? full : full.substr(0, colon));
    if (colon == std::wstring::npos) return true;
    return ue_wrap::fstring_utils::MintFString(full.substr(colon + 1), p + kSoftSubPath);
}

void ReadBetterSave(const uint8_t* p, Item& out) {
    out.classPath = ReadSoftPath(p + g_b[kClass].off);
    ue_wrap::FTransform t{};
    std::memcpy(&t, p + g_b[kTransform].off, sizeof(t));
    out.xform = {t.RotX, t.RotY, t.RotZ, t.RotW, t.TX, t.TY, t.TZ, t.SX, t.SY, t.SZ};
    out.name      = SR::ReadFNameAt(p, g_b[kName].off);
    out.tag       = SR::ReadFNameAt(p, g_b[kTag].off);
    out.key       = SR::ReadFNameAt(p, g_b[kKey].off);
    out.attachKey = SR::ReadFNameAt(p, g_b[kAttachKey].off);
    const SR::Arr bytes = SR::ReadArr(p, g_b[kActorData].off);
    out.actorData.assign(bytes.data, bytes.data + bytes.num);
    std::memcpy(&out.index, p + g_b[kIndex].off, sizeof(out.index));
}

bool WriteBetterSave(uint8_t* p, const Item& in) {
    if (!in.classPath.empty() && !WriteSoftPath(p + g_b[kClass].off, in.classPath)) return false;
    ue_wrap::FTransform t{};
    t.RotX = in.xform[0]; t.RotY = in.xform[1]; t.RotZ = in.xform[2]; t.RotW = in.xform[3];
    t.TX = in.xform[4]; t.TY = in.xform[5]; t.TZ = in.xform[6];
    t.SX = in.xform[7]; t.SY = in.xform[8]; t.SZ = in.xform[9];
    std::memcpy(p + g_b[kTransform].off, &t, sizeof(t));
    SR::WriteFNameField(p + g_b[kName].off, in.name);
    SR::WriteFNameField(p + g_b[kTag].off, in.tag);
    SR::WriteFNameField(p + g_b[kKey].off, in.key);
    SR::WriteFNameField(p + g_b[kAttachKey].off, in.attachKey);
    if (!in.actorData.empty()) {
        void* buf = SR::AllocZeroed(in.actorData.size(), 1);
        if (!buf) return false;
        std::memcpy(buf, in.actorData.data(), in.actorData.size());
        SR::WriteArrHeader(p, g_b[kActorData].off, buf, static_cast<int32_t>(in.actorData.size()));
    }
    std::memcpy(p + g_b[kIndex].off, &in.index, sizeof(in.index));
    return true;
}

}  // namespace

std::wstring ClassLeaf(const Item& it) {
    const size_t colon = it.classPath.find(L':');
    const std::wstring path = colon == std::wstring::npos ? it.classPath : it.classPath.substr(0, colon);
    const size_t dot = path.rfind(L'.');
    return dot == std::wstring::npos ? path : path.substr(dot + 1);
}

bool Resolve() {
    if (g_resolved) return true;
    static std::chrono::steady_clock::time_point s_lastTry{};
    const auto now = std::chrono::steady_clock::now();
    if (s_lastTry != std::chrono::steady_clock::time_point{} && now - s_lastTry < std::chrono::seconds(1)) return false;
    s_lastTry = now;

    void* const item = R::FindObject(L"struct_inventoryItem", L"UserDefinedStruct");
    if (!item) return false;
    const std::vector<R::StructFieldInfo> fields = R::EnumerateStructFields(item);
    void* better = nullptr;
    for (const auto& f : fields)
        if (PrefixMatch(f.name, L"data_")) { better = R::PropertyInnerStruct(item, f.name.c_str()); break; }
    if (!better) {
        UE_LOGW("inventory_item: struct_inventoryItem has no data_ struct member -- inventory entries are not read or "
                "written");
        return false;
    }
    // PropertiesSize leaves out the tail padding the struct's 16-byte alignment (its FTransform) adds, which the
    // member's ElementSize includes.
    g_betterSize = (R::StructSize(better) + kAlign - 1) / kAlign * kAlign;
    g_i[kData].size = g_betterSize;
    if (!ResolveMembers(better, g_b, kBCount, "struct_betterSave") ||
        !ResolveMembers(item, g_i, kICount, "struct_inventoryItem"))
        return false;
    const int32_t size = R::StructSize(item);
    g_stride = (size + kAlign - 1) / kAlign * kAlign;
    g_resolved = g_stride > 0;
    if (g_resolved) UE_LOGI("inventory_item: resolved (entry %d B, stride %d B, betterSave %d B)", size, g_stride,
                            g_betterSize);
    return g_resolved;
}

int32_t Stride() { return g_resolved ? g_stride : 0; }

bool Read(const void* base, Item& out) {
    if (!g_resolved || !base) return false;
    const uint8_t* const p = static_cast<const uint8_t*>(base);
    out = Item{};
    ReadBetterSave(p + g_i[kData].off, out);
    std::memcpy(&out.posX, p + g_i[kPosition].off, sizeof(float));
    std::memcpy(&out.posY, p + g_i[kPosition].off + 4, sizeof(float));
    std::memcpy(&out.rotation, p + g_i[kRotation].off, sizeof(float));
    out.compartment = p[g_i[kCompartment].off];
    out.amount = p[g_i[kAmount].off];
    return true;
}

bool Write(uint8_t* base, const Item& in) {
    if (!g_resolved || !base) return false;
    std::memcpy(base + g_i[kPosition].off, &in.posX, sizeof(float));
    std::memcpy(base + g_i[kPosition].off + 4, &in.posY, sizeof(float));
    std::memcpy(base + g_i[kRotation].off, &in.rotation, sizeof(float));
    base[g_i[kCompartment].off] = in.compartment;
    base[g_i[kAmount].off] = in.amount;
    return WriteBetterSave(base + g_i[kData].off, in);
}

bool Capture(void* actor, Item& out) {
    if (!actor || !Resolve()) return false;
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(actor), L"betterGetData");
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    const int32_t off = f.ParamOffset(L"betterData");
    if (!f.valid() || off < 0 || off + g_betterSize > f.FrameSize() || !ue_wrap::Call(actor, f)) return false;
    // The frame's own nested buffers are left to it, the out-parameter doctrine of fstring_utils.h.
    out = Item{};
    ReadBetterSave(static_cast<const uint8_t*>(f.data()) + off, out);
    out.amount = 1;
    return true;
}

}  // namespace ue_wrap::inventory_item
