// ue_wrap/devices/cord.cpp -- see ue_wrap/devices/cord.h.

#include "ue_wrap/devices/cord.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/world_instances.h"

#include <atomic>
#include <string>
#include <utility>

namespace ue_wrap::cord {
namespace {
namespace R = ue_wrap::reflection;

std::atomic<bool> g_resolved{false};
// The world generation a resolve last failed in; 0 is none (generations start at 1).
std::atomic<uint32_t> g_failedGen{0};
void* g_cordCls = nullptr;
void* g_socketCls = nullptr;
void* g_propCls = nullptr;
int32_t g_cordSocketOff = -1;
int32_t g_cordParentOff = -1;
int32_t g_cordHeadOff = -1;
int32_t g_socketCordOff = -1;
int32_t g_propCordOff = -1;
void* g_connectFn = nullptr;
void* g_disconnectFn = nullptr;

bool IsA(void* actor, void* cls) {
    if (!actor || !cls || !R::IsLive(actor)) return false;
    void* actual = R::ClassOf(actor);
    return actual && R::IsDescendantOfAny(actual, &cls, 1);
}

void* ReadPtr(void* obj, int32_t off) {
    return *reinterpret_cast<void**>(reinterpret_cast<char*>(obj) + off);
}

// A pointer field's target when it is a live object, else null.
void* LivePtr(void* obj, int32_t off) {
    void* p = ReadPtr(obj, off);
    return (p && R::IsLive(p)) ? p : nullptr;
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;
    // A resolve that fails is not tried again in the same world: a class lookup that misses walks the object array,
    // and the classes load with a world or not at all (ue_wrap/devices/serverbox.cpp, CountUpgradeSpawners).
    const uint32_t gen = world_identity::Generation();
    if (g_failedGen.load(std::memory_order_acquire) == gen) return false;
    const auto failed = [gen]() {
        g_failedGen.store(gen, std::memory_order_release);
        return false;
    };
    void* cordCls = R::FindClass(L"cord_C");
    void* socketCls = R::FindClass(L"cordSocket_C");
    void* propCls = R::FindClass(L"prop_corded_C");
    if (!cordCls || !socketCls || !propCls) return failed();
    const int32_t cordSocketOff = R::FindPropertyOffset(cordCls, L"socket");
    const int32_t cordParentOff = R::FindPropertyOffset(cordCls, L"parent");
    const int32_t cordHeadOff = R::FindPropertyOffset(cordCls, L"head");
    const int32_t socketCordOff = R::FindPropertyOffset(socketCls, L"cord");
    const int32_t propCordOff = R::FindPropertyOffset(propCls, L"obj_cord");
    void* connectFn = R::FindFunction(cordCls, L"connect");
    void* disconnectFn = R::FindFunction(cordCls, L"disconnect");
    if (cordSocketOff < 0 || cordParentOff < 0 || cordHeadOff < 0 || socketCordOff < 0 || propCordOff < 0 ||
        !connectFn || !disconnectFn) {
        UE_LOGW("cord: reflected fields or functions are incomplete -- channel unavailable");
        return failed();
    }
    g_cordCls = cordCls;
    g_socketCls = socketCls;
    g_propCls = propCls;
    g_cordSocketOff = cordSocketOff;
    g_cordParentOff = cordParentOff;
    g_cordHeadOff = cordHeadOff;
    g_socketCordOff = socketCordOff;
    g_propCordOff = propCordOff;
    g_connectFn = connectFn;
    g_disconnectFn = disconnectFn;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("cord: resolved cord_C, cordSocket_C and prop_corded_C relationship fields");
    return true;
}

bool IsCord(void* actor) { return EnsureResolved() && IsA(actor, g_cordCls); }
bool IsSocket(void* actor) { return EnsureResolved() && IsA(actor, g_socketCls); }
bool IsCordedProp(void* actor) { return EnsureResolved() && IsA(actor, g_propCls); }

void* CordOf(void* prop) {
    if (!IsCordedProp(prop)) return nullptr;
    return LivePtr(prop, g_propCordOff);
}

void* PropOf(void* cord) {
    if (!IsCord(cord)) return nullptr;
    return LivePtr(cord, g_cordParentOff);
}

void* SocketOf(void* cord) {
    if (!IsCord(cord)) return nullptr;
    return LivePtr(cord, g_cordSocketOff);
}

void* CordIn(void* socket) {
    if (!IsSocket(socket)) return nullptr;
    return LivePtr(socket, g_socketCordOff);
}

bool HeadLocation(void* cord, ue_wrap::FVector& out) {
    if (!IsCord(cord)) return false;
    void* head = LivePtr(cord, g_cordHeadOff);
    return head && ue_wrap::engine::TryGetComponentLocation(head, out);
}

bool Connect(void* cord, void* socket) {
    if (!IsCord(cord) || !IsSocket(socket)) return false;
    ParamFrame p(g_connectFn);
    // load=false is the player's plug: connect owns the whole transition, cordPlugged included, and into a
    // socket that already holds a cord it unplugs THIS cord and leaves the occupant.
    return p.Set(L"Socket", socket) && p.Set(L"load", false) && Call(cord, p);
}

bool Disconnect(void* cord) {
    if (!IsCord(cord)) return false;
    ParamFrame p(g_disconnectFn);
    return Call(cord, p);
}

int32_t Sockets(void** out, int32_t cap) {
    if (!EnsureResolved() || !out || cap <= 0) return 0;
    return ue_wrap::world_instances::Find(L"cordSocket_C", out, cap);
}

void* ChildSocket(void* owner, const std::wstring& childName) {
    if (!EnsureResolved() || !owner || childName.empty() || !R::IsLive(owner) ||
        !ue_wrap::prop::IsDescendantOfProp(owner))
        return nullptr;
    int32_t off = -1;
    std::wstring type;
    // The name is a peer's word: only an object property that holds a live child-actor component is read.
    if (!R::PropertyOffsetAndClass(R::ClassOf(owner), childName.c_str(), off, type) || off < 0 ||
        type != L"ObjectProperty")
        return nullptr;
    void* comp = LivePtr(owner, off);
    if (!comp || R::ClassNameOf(comp) != L"ChildActorComponent") return nullptr;
    const int32_t childOff = R::FindPropertyOffset(R::ClassOf(comp), L"ChildActor");
    if (childOff < 0) return nullptr;
    void* actor = LivePtr(comp, childOff);
    return IsSocket(actor) ? actor : nullptr;
}

bool ChildSocketName(void* socket, std::wstring& ownerKey, std::wstring& childName) {
    ownerKey.clear();
    childName.clear();
    if (!IsSocket(socket)) return false;
    std::wstring component;
    void* owner = ue_wrap::engine::ParentActorOf(socket, &component);
    if (!owner || component.empty() || !R::IsLive(owner) || !ue_wrap::prop::IsDescendantOfProp(owner)) return false;
    std::wstring key = ue_wrap::prop::GetKeyString(owner);
    if (key.empty() || key == L"None") return false;
    ownerKey = std::move(key);
    childName = std::move(component);
    return true;
}

}  // namespace ue_wrap::cord
