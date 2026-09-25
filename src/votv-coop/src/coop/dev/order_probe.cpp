// coop/dev/order_probe.cpp -- see coop/dev/order_probe.h.

#include "coop/dev/order_probe.h"

#include "coop/config/config.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/world/order_economy.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace coop::dev::order_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace OE = ue_wrap::order_economy;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_installed = false;

constexpr const wchar_t* kLaptop = L"ui_laptop_C";
constexpr const wchar_t* kDrone  = L"drone_C";
constexpr const wchar_t* kCycle  = L"daynightCycle_C";
constexpr const wchar_t* kMake   = L"makeAnOrder";
constexpr const wchar_t* kAdd    = L"addOrderCart";
constexpr const wchar_t* kRemove = L"removeOrderCart";
constexpr const wchar_t* kSend   = L"sendShop";
constexpr const wchar_t* kCheck  = L"checkOrders";
constexpr const wchar_t* kDeliver = L"compileOrder";
constexpr const wchar_t* kHour   = L"func_newHour";
constexpr int kTagMake = 0x4F504D4B, kTagAdd = 0x4F504144, kTagRemove = 0x4F505245;  // 'OPMK' 'OPAD' 'OPRE'
constexpr int kTagSend = 0x4F505344, kTagCheck = 0x4F504348, kTagDeliver = 0x4F504456;  // 'OPSD' 'OPCH' 'OPDV'
constexpr int kTagHour = 0x4F50484F;                                                     // 'OPHO'

const char* Side() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return "?";
    return s->role() == coop::net::Role::Host ? "host" : "client";
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back((c >= 0 && c < 128) ? static_cast<char>(c) : '?');
    return s;
}

// The Blueprint function that made the call, or "-" for one through ProcessEvent (ours, an event, a timer).
std::string Caller(const sg::Call& c) {
    return c.callerFunction ? Narrow(R::ToString(R::NameOf(c.callerFunction))) : std::string("-");
}

// The item count of the store order at `order`, whose type is `orderStruct`: its items array, the member
// named items_<guid>. -1 when either is missing.
int ItemsOf(void* orderStruct, const uint8_t* order) {
    const int32_t off = (orderStruct && order) ? R::FindPropertyOffsetByPrefix(orderStruct, L"items_") : -1;
    if (off < 0) return -1;
    int32_t num = 0;
    std::memcpy(&num, order + off + 8, sizeof(num));  // TArray: data @0, Num @8
    return num;
}

sg::Verdict OnMakeAnOrder(const sg::Call& c) {
    const int32_t autoOff = R::FindParamOffset(c.function, L"automatic");
    const bool automatic = autoOff >= 0 && c.locals && c.locals[autoOff] != 0;
    const int32_t itemOff = R::FindParamOffset(c.function, L"NewItem");
    const uint8_t* order = itemOff >= 0 ? sg::OutParamPtr(c, itemOff) : nullptr;  // passed by reference
    if (!order && itemOff >= 0 && c.locals) order = c.locals + itemOff;
    UE_LOGI("[ORDER-PROBE] %s makeAnOrder automatic=%d items=%d queue=%d caller=%s%s", Side(), automatic ? 1 : 0,
            ItemsOf(R::PropertyInnerStruct(c.function, L"NewItem"), order), OE::OrderCount(), Caller(c).c_str(),
            c.fromOurCode ? " (ours)" : "");
    return sg::Verdict::Run;
}

void OnQueuePost(const sg::Call& c) {
    UE_LOGI("[ORDER-PROBE] %s %s done, queue=%d caller=%s", Side(), c.tag == kTagAdd ? "addOrderCart" : "removeOrderCart",
            OE::OrderCount(), Caller(c).c_str());
}

sg::Verdict OnDroneVerb(const sg::Call& c) {
    void* cls = c.object ? R::ClassOf(c.object) : nullptr;
    int32_t activeOff = -1;
    uint8_t activeMask = 0;
    const bool active = cls && R::FindBoolProperty(cls, L"active", activeOff, activeMask) &&
                        (reinterpret_cast<const uint8_t*>(c.object)[activeOff] & activeMask) != 0;
    const int32_t orderOff = cls ? R::FindPropertyOffset(cls, L"order") : -1;
    const int items = orderOff >= 0 ? ItemsOf(R::PropertyInnerStruct(cls, L"order"),
                                              reinterpret_cast<const uint8_t*>(c.object) + orderOff)
                                    : -1;
    const char* verb = c.tag == kTagSend ? "sendShop" : c.tag == kTagCheck ? "checkOrders" : "compileOrder (a delivery)";
    UE_LOGI("[ORDER-PROBE] %s drone %s active=%d carries=%d queue=%d caller=%s", Side(), verb, active ? 1 : 0, items,
            OE::OrderCount(), Caller(c).c_str());
    return sg::Verdict::Run;
}

sg::Verdict OnNewHour(const sg::Call& c) {
    int32_t t[3] = {};
    const int32_t off = R::FindParamOffset(c.function, L"time");
    if (off >= 0 && c.locals) std::memcpy(t, c.locals + off, sizeof(t));
    UE_LOGI("[ORDER-PROBE] %s daynightCycle func_newHour time=(%d,%d,%d) queue=%d", Side(), t[0], t[1], t[2],
            OE::OrderCount());
    return sg::Verdict::Run;
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::order_probe);
    return s;
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!IsEnabled() || g_installed) return;
    g_installed = sg::WatchClassName(kLaptop, kMake, kTagMake, &OnMakeAnOrder, nullptr) &&
                  sg::WatchClassName(kLaptop, kAdd, kTagAdd, nullptr, &OnQueuePost) &&
                  sg::WatchClassName(kLaptop, kRemove, kTagRemove, nullptr, &OnQueuePost) &&
                  sg::WatchClassName(kDrone, kSend, kTagSend, &OnDroneVerb, nullptr) &&
                  sg::WatchClassName(kDrone, kCheck, kTagCheck, &OnDroneVerb, nullptr) &&
                  sg::WatchClassName(kDrone, kDeliver, kTagDeliver, &OnDroneVerb, nullptr) &&
                  sg::WatchClassName(kCycle, kHour, kTagHour, &OnNewHour, nullptr);
    UE_LOGI("[ORDER-PROBE] watches %s: makeAnOrder, addOrderCart, removeOrderCart, drone sendShop/checkOrders/"
            "compileOrder, daynightCycle func_newHour", g_installed ? "registered" : "NOT all registered");
}

}  // namespace coop::dev::order_probe
