// ue_wrap/devices/cord.cpp -- see ue_wrap/devices/cord.h.

#include "ue_wrap/devices/cord.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>

namespace ue_wrap::cord {
namespace {
namespace R = ue_wrap::reflection;

std::atomic<bool> g_resolved{false};
void* g_cordCls = nullptr;
void* g_socketCls = nullptr;
void* g_propCls = nullptr;
int32_t g_keyOff = -1;
int32_t g_propKeyOff = -1;
int32_t g_propSocketOff = -1;
int32_t g_propCordOff = -1;
void* g_connectFn = nullptr;
void* g_disconnectFn = nullptr;

bool IsA(void* actor, void* cls) {
    if (!actor || !cls || !R::IsLive(actor)) return false;
    void* actual = R::ClassOf(actor);
    return actual && R::IsDescendantOfAny(actual, &cls, 1);
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;
    void* cordCls = R::FindClass(L"cord_C");
    void* socketCls = R::FindClass(L"cordSocket_C");
    void* propCls = R::FindClass(L"prop_corded_C");
    if (!cordCls || !socketCls || !propCls) return false;
    const int32_t keyOff = R::FindPropertyOffset(cordCls, L"Key");
    const int32_t propKeyOff = R::FindPropertyOffset(propCls, L"Key");
    const int32_t propSocketOff = R::FindPropertyOffset(propCls, L"Socket");
    const int32_t propCordOff = R::FindPropertyOffset(propCls, L"obj_cord");
    void* connectFn = R::FindFunction(cordCls, L"Connect");
    void* disconnectFn = R::FindFunction(cordCls, L"disconnect");
    if (keyOff < 0 || propKeyOff < 0 || propSocketOff < 0 || propCordOff < 0 ||
        !connectFn || !disconnectFn) {
        UE_LOGW("cord: reflected fields or functions are incomplete -- channel unavailable");
        return false;
    }
    g_cordCls = cordCls;
    g_socketCls = socketCls;
    g_propCls = propCls;
    g_keyOff = keyOff;
    g_propKeyOff = propKeyOff;
    g_propSocketOff = propSocketOff;
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

std::wstring Key(void* actor) {
    if (!EnsureResolved() || !actor) return {};
    const int32_t off = IsCordedProp(actor) ? g_propKeyOff : g_keyOff;
    const auto& key = *reinterpret_cast<const R::FName*>(reinterpret_cast<const char*>(actor) + off);
    return R::ToString(key);
}

void* SocketOf(void* prop) {
    if (!EnsureResolved() || !IsCordedProp(prop)) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<char*>(prop) + g_propSocketOff);
}

void* CordOf(void* prop) {
    if (!EnsureResolved() || !IsCordedProp(prop)) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<char*>(prop) + g_propCordOff);
}

bool ApplyLink(void* prop, void* socket) {
    if (!IsCordedProp(prop) || !IsSocket(socket)) return false;
    void* cord = CordOf(prop);
    if (!IsCord(cord)) return false;
    ParamFrame p(g_connectFn);
    // Connect owns the full Blueprint transition, including cordPlugged callbacks.
    // Calling those callbacks again here causes the game to undo/re-enter the link.
    return p.Set(L"Socket", socket) && p.Set(L"load", true) && Call(cord, p);
}

bool ApplyUnplug(void* prop) {
    if (!IsCordedProp(prop)) return false;
    void* cord = CordOf(prop);
    if (!IsCord(cord)) return false;
    ParamFrame p(g_disconnectFn);
    // disconnect likewise dispatches cordUnplugged itself.
    return Call(cord, p);
}

}  // namespace ue_wrap::cord
