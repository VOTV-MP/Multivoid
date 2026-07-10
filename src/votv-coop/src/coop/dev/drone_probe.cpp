// coop/dev/drone_probe.cpp -- see coop/dev/drone_probe.h.

#include "coop/dev/drone_probe.h"

#include "coop/config/config.h"
#include "ue_wrap/call.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace coop::dev::drone_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

bool ProbeEnabled() {
    static const bool s_enabled = coop::config::IsIniKeyTrue("drone_probe");
    return s_enabled;
}

// ---- observer identification table -----------------------------------------
// One shared callback identifies which UFunction fired by comparing the dispatched
// `function` ptr against the cached set. A silent verb = BP-internal (the trap).
struct WatchedFn { void* fn = nullptr; const char* name = nullptr; bool isTick = false; };
std::array<WatchedFn, 32> g_watched{};
size_t g_watchedCount = 0;
std::atomic<bool> g_receiveTickLogged{false};

void OnDroneVerb(void* self, void* function, void* /*params*/) {
    if (!ProbeEnabled()) return;
    for (size_t i = 0; i < g_watchedCount; ++i) {
        if (g_watched[i].fn != function) continue;
        const std::wstring cls = (self && R::IsLive(self)) ? R::ClassNameOf(self) : std::wstring(L"?");
        if (g_watched[i].isTick) {
            // ReceiveTick fires every frame -> log ONCE (confirms the observer mechanism
            // works AND the drone is ticking), then stay silent.
            if (!g_receiveTickLogged.exchange(true)) {
                UE_LOGI("[drone_probe] OBSERVER-OK: %s fired (self=%p cls='%ls') -- "
                        "ProcessEvent observers work + the drone ticks",
                        g_watched[i].name, self, cls.c_str());
            }
            return;
        }
        UE_LOGI("[drone_probe] VERB FIRED: %s (self=%p cls='%ls') -- ProcessEvent-DISPATCHED = OBSERVABLE",
                g_watched[i].name, self, cls.c_str());
        return;
    }
}

void RegisterOn(const wchar_t* className, const wchar_t* fnName, const char* label, bool isTick) {
    void* cls = R::FindClass(className);
    if (!cls) return;  // class not loaded yet (caller retries) / not present
    void* fn = R::FindFunction(cls, fnName);
    if (!fn) {
        UE_LOGW("[drone_probe] UFunction '%ls::%ls' not found (BP may name it differently)", className, fnName);
        return;
    }
    for (size_t i = 0; i < g_watchedCount; ++i) if (g_watched[i].fn == fn) return;  // dedup
    if (!GT::RegisterPostObserver(fn, &OnDroneVerb)) {
        UE_LOGW("[drone_probe] RegisterPostObserver failed for %s (table full?)", label);
        return;
    }
    if (g_watchedCount < g_watched.size())
        g_watched[g_watchedCount++] = WatchedFn{fn, label, isTick};
}

bool g_installed = false;

// ---- state poll cache ------------------------------------------------------
void* g_gmCache = nullptr;
void* ResolveGamemode() {
    if (g_gmCache && R::IsLive(g_gmCache)) return g_gmCache;
    g_gmCache = R::FindObjectByClass(L"mainGamemode_C");
    return g_gmCache;
}

int32_t Off(void* cls, const wchar_t* name) { return cls ? R::FindPropertyOffset(cls, name) : -1; }

template <typename T>
T ReadAt(void* obj, int32_t off) {
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(obj) + off);
}

// Transition tracking (so we log on CHANGE, not every tick).
int g_lastActive = -2, g_lastHasOrder = -2, g_lastHasSack = -2, g_lastFlyingType = -2, g_lastPickedUp = -2;
int g_lastOrdersNum = -2;   // saveSlot.orders.Num edge tracking (the economy signal -- see Tick)
bool g_dronePresenceLogged = false;
bool g_droneAbsenceLogged  = false;
int  g_radarLast = -2;
uint64_t g_tickCounter = 0;

// ---- autonomous trigger (ini drone_probe_drive=1) -- "run the probe yourself" --------------
// The drone verbs are unobservable to a PASSIVE hook (EX_LocalVirtualFunction -> ProcessInternal,
// below our ProcessEvent detour) BUT are freely CALLABLE actively via ProcessEvent (all are
// FUNC_BlueprintEvent|FUNC_BlueprintCallable). So instead of asking a human to fly a delivery, we
// drive the game's OWN delivery path on the game thread (this Tick runs in net_pump, GT-serial):
//   HOST   -> Make Default Order(out) then laptop.makeAnOrder(order, automatic=true)  == func_newHour
//   CLIENT -> Make Default Order(out) then laptop.addOrderCart(order)  (commit-only, no fly)
// One-shot, fired after a settle delay so the world + link are stable. RE: the "autonomous trigger"
// agent pass in votv-delivery-drone-RE-and-coop-sync-design-2026-06-03.md.
bool DriveEnabled() {
    static const bool s_on = coop::config::IsIniKeyTrue("drone_probe_drive");
    return s_on;
}
bool g_driveFired  = false;
int  g_settleTicks = 0;
constexpr int32_t  kOrderStructSize = 0x18;             // Fstruct_storeOrder (items TArray@0x00 + time@0x10)
constexpr uint64_t kCpfOutOrReturn  = 0x100u | 0x400u;  // CPF_OutParm | CPF_ReturnParm

// Build a default order via daynightCycle "Make Default Order" (a no-input, pure-default builder
// that fills 6-8 real Aprop_C line items). Copies the 0x18 struct into `out`; the inner items
// TArray points at engine-allocated heap we intentionally leak for this one-shot (makeAnOrder/
// addOrderCart deep-copy the struct into saveSlot.orders). False if unresolved.
bool BuildDefaultOrder(void* gm, void* gmCls, uint8_t out[kOrderStructSize]) {
    const int32_t offDnc = Off(gmCls, L"daynightCycle");
    if (offDnc < 0) return false;
    void* dnc = ReadAt<void*>(gm, offDnc);
    if (!dnc || !R::IsLive(dnc)) return false;
    void* fn = R::FindFunction(R::ClassOf(dnc), L"Make Default Order");
    if (!fn) { UE_LOGW("[drone_probe] drive: 'Make Default Order' UFunction not found"); return false; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    if (!ue_wrap::Call(dnc, f)) { UE_LOGW("[drone_probe] drive: Make Default Order call failed"); return false; }
    // The order is the function's OUT/return struct param; locate it via the FProperty chain,
    // then read it BY NAME through GetRaw -- which bounds-checks the offset against the frame
    // size (the manual f.data()+offset memcpy skipped that guard -- audit 2026-06-09).
    for (const auto& p : R::FunctionParams(fn)) {
        if ((p.flags & kCpfOutOrReturn) && p.size >= 0x10) {
            const int32_t n = p.size < kOrderStructSize ? p.size : kOrderStructSize;
            std::memset(out, 0, kOrderStructSize);
            if (f.GetRaw(p.name.c_str(), out, n)) return true;
            break;
        }
    }
    UE_LOGW("[drone_probe] drive: Make Default Order exposed no OUT struct param");
    return false;
}

// HOST: fly one real delivery (== the game's func_newHour). Preconditions (RE): drone idle +
// radiotower present & not broken (else sendShop aborts / a null tower would fault).
void TriggerHostDelivery(void* gm, void* gmCls, void* drone, void* dCls) {
    const int32_t offActive = Off(dCls, L"Active");
    if (offActive >= 0 && ReadAt<uint8_t>(drone, offActive)) {
        UE_LOGI("[drone_probe] drive(host): drone already Active -- skip (not idle)"); return;
    }
    const int32_t offRt = Off(gmCls, L"radiotower");
    void* rt = offRt >= 0 ? ReadAt<void*>(gm, offRt) : nullptr;
    if (!rt || !R::IsLive(rt)) {
        UE_LOGI("[drone_probe] drive(host): radiotower null -- skip (sendShop reads it)"); return;
    }
    const int32_t offBroken = Off(R::ClassOf(rt), L"isBroken");
    if (offBroken >= 0 && ReadAt<uint8_t>(rt, offBroken)) {
        UE_LOGI("[drone_probe] drive(host): radiotower isBroken -- skip (sendShop aborts)"); return;
    }
    const int32_t offSell = Off(dCls, L"sellLocation");
    void* sell = offSell >= 0 ? ReadAt<void*>(drone, offSell) : nullptr;
    if (!sell || !R::IsLive(sell)) {
        UE_LOGI("[drone_probe] drive(host): drone.sellLocation null -- skip (beginFly reads it)"); return;
    }
    uint8_t order[kOrderStructSize];
    if (!BuildDefaultOrder(gm, gmCls, order)) return;
    const int32_t offLaptop = Off(gmCls, L"laptop");
    void* laptop = offLaptop >= 0 ? ReadAt<void*>(gm, offLaptop) : nullptr;
    if (!laptop || !R::IsLive(laptop)) { UE_LOGW("[drone_probe] drive(host): laptop null -- skip"); return; }
    void* fn = R::FindFunction(R::ClassOf(laptop), L"makeAnOrder");
    if (!fn) { UE_LOGW("[drone_probe] drive(host): makeAnOrder not found"); return; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return;
    f.SetRaw(L"NewItem", order, kOrderStructSize);
    f.Set<bool>(L"automatic", true);
    const bool ok = ue_wrap::Call(laptop, f);
    UE_LOGI("[drone_probe] DRIVE(host): makeAnOrder(default,automatic) ok=%d -- watch flyingType/"
            "Active/orders.Num++ now + cargo Init + DRAINED at arrival.", ok ? 1 : 0);
}

// CLIENT: commit one shop order (the only writer of saveSlot.orders), NO fly. Confirms [#7] (the
// client shop path works + the order stays client-local -> the OrderRequest edge is needed).
void PlaceClientOrder(void* gm, void* gmCls) {
    uint8_t order[kOrderStructSize];
    if (!BuildDefaultOrder(gm, gmCls, order)) return;
    const int32_t offLaptop = Off(gmCls, L"laptop");
    void* laptop = offLaptop >= 0 ? ReadAt<void*>(gm, offLaptop) : nullptr;
    if (!laptop || !R::IsLive(laptop)) { UE_LOGW("[drone_probe] drive(client): laptop null -- skip"); return; }
    void* fn = R::FindFunction(R::ClassOf(laptop), L"addOrderCart");
    if (!fn) { UE_LOGW("[drone_probe] drive(client): addOrderCart not found"); return; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return;
    f.SetRaw(L"NewItem", order, kOrderStructSize);
    const bool ok = ue_wrap::Call(laptop, f);
    UE_LOGI("[drone_probe] DRIVE(client): addOrderCart(default) ok=%d -- watch saveSlot.orders.Num++ "
            "(ORDER PLACED [#7] = shop reachable + order is CLIENT-LOCAL).", ok ? 1 : 0);
}

}  // namespace

void Install() {
    if (!ProbeEnabled() || g_installed) return;
    if (!R::FindClass(L"drone_C")) return;  // drone BP not loaded yet -- retry next tick

    // Drone verbs (POST -- observe AFTER they ran). A FIRED log = observable; SILENCE
    // during a real delivery = BP-internal (the doorOpen/flashlight trap) -> we must
    // poll the flags (state dump below) instead of hooking that verb.
    RegisterOn(L"drone_C", L"ReceiveTick",      "drone.ReceiveTick",     /*isTick*/true);
    RegisterOn(L"drone_C", L"triggerFly",       "drone.triggerFly",      false);
    RegisterOn(L"drone_C", L"beginFly",         "drone.beginFly",        false);
    RegisterOn(L"drone_C", L"dropSack",         "drone.dropSack",        false);
    RegisterOn(L"drone_C", L"soundAlarm",       "drone.soundAlarm",      false);
    RegisterOn(L"drone_C", L"checkOrders",      "drone.checkOrders",     false);
    RegisterOn(L"drone_C", L"compileOrder",     "drone.compileOrder",    false);
    RegisterOn(L"drone_C", L"player_use",       "drone.player_use",      false);
    RegisterOn(L"drone_C", L"sendShop",         "drone.sendShop",        false);
    RegisterOn(L"drone_C", L"ReceiveBeginPlay", "drone.ReceiveBeginPlay",false);
    // Console call path (the #1-risk question: is the client's call edge hookable?).
    RegisterOn(L"droneConsole_C", L"player_use",        "console.player_use",        false);
    RegisterOn(L"droneConsole_C", L"playerHandUse_LMB", "console.playerHandUse_LMB", false);
    RegisterOn(L"droneConsole_C", L"isButtonUsed",      "console.isButtonUsed",      false);
    // Cargo spawn -> does the box fire the Aprop_C Init the prop pipeline observes? ([#3])
    RegisterOn(L"orderPlace_C",                  L"spawnOrder",      "orderPlace.spawnOrder", false);
    RegisterOn(L"prop_container_orderbox_C",     L"Init",            "orderbox.Init",         false);
    RegisterOn(L"prop_container_giftbox_C",      L"Init",            "giftbox.Init",          false);
    RegisterOn(L"prop_inventoryContainer_drone_C", L"ReceiveBeginPlay", "itembox.BeginPlay",  false);
    // Auto-drive schedule + gift injection ([#8],[#9]).
    RegisterOn(L"daynightCycle_C", L"sendDriveBox",       "daynight.sendDriveBox",     false);
    RegisterOn(L"daynightCycle_C", L"Make Default Order", "daynight.MakeDefaultOrder", false);
    RegisterOn(L"daynightCycle_C", L"createNewTask",      "daynight.createNewTask",    false);

    g_installed = true;
    UE_LOGI("[drone_probe] installed %zu observers (drone/console/orderPlace/cargo/daynight). "
            "Call the drone console for a delivery -- a FIRED line = observable, silence = BP-internal.",
            g_watchedCount);
}

void Tick(bool connected, bool isHost) {
    if (!ProbeEnabled()) return;
    void* gm = ResolveGamemode();
    if (!gm) return;
    void* gmCls = R::ClassOf(gm);
    const int32_t offDrone = Off(gmCls, L"drone");
    if (offDrone < 0) return;

    void* drone = ReadAt<void*>(gm, offDrone);
    if (!drone || !R::IsLive(drone)) {
        if (!g_droneAbsenceLogged) {
            g_droneAbsenceLogged = true;
            UE_LOGI("[drone_probe] mainGamemode.drone is NULL/dead right now (gm=%p) -- "
                    "drone may be dormant/un-spawned; watching for it.", gm);
        }
        return;
    }
    g_droneAbsenceLogged = false;
    if (!g_dronePresenceLogged) {
        g_dronePresenceLogged = true;
        UE_LOGI("[drone_probe] DRONE PRESENT [#1]: mainGamemode.drone=%p cls='%ls' "
                "(if this ptr stays stable all session -> singleton placed actor, identity OK)",
                drone, R::ClassNameOf(drone).c_str());
    }

    void* dCls = R::ClassOf(drone);
    const int active   = Off(dCls, L"Active")     >= 0 ? (int)ReadAt<uint8_t>(drone, Off(dCls, L"Active"))     : -1;
    const int hasOrder = Off(dCls, L"hasOrder")   >= 0 ? (int)ReadAt<uint8_t>(drone, Off(dCls, L"hasOrder"))   : -1;
    const int hasSack  = Off(dCls, L"hasSack")    >= 0 ? (int)ReadAt<uint8_t>(drone, Off(dCls, L"hasSack"))    : -1;
    const int flying   = Off(dCls, L"flyingType") >= 0 ? (int)ReadAt<int32_t>(drone, Off(dCls, L"flyingType")) : -2;
    const int picked   = Off(dCls, L"pickedUp")   >= 0 ? (int)ReadAt<uint8_t>(drone, Off(dCls, L"pickedUp"))   : -1;

    if (active != g_lastActive || hasOrder != g_lastHasOrder || hasSack != g_lastHasSack ||
        flying != g_lastFlyingType || picked != g_lastPickedUp) {
        UE_LOGI("[drone_probe] STATE [#4]: Active=%d hasOrder=%d hasSack=%d flyingType=%d pickedUp=%d",
                active, hasOrder, hasSack, flying, picked);
        g_lastActive = active; g_lastHasOrder = hasOrder; g_lastHasSack = hasSack;
        g_lastFlyingType = flying; g_lastPickedUp = picked;
    }

    // radarObjects membership ([#10]) -- TArray<AActor*> = { void** Data; int32 Num; ... }.
    const int32_t offRadar = Off(gmCls, L"radarObjects");
    if (offRadar >= 0) {
        void** data = ReadAt<void**>(gm, offRadar);
        const int32_t num = ReadAt<int32_t>(gm, offRadar + 8);
        int present = 0;
        if (data && num > 0 && num < 8192) {
            for (int32_t i = 0; i < num; ++i) if (data[i] == drone) { present = 1; break; }
        }
        if (present != g_radarLast) {
            UE_LOGI("[drone_probe] RADAR [#10]: drone %s mainGamemode.radarObjects (Num=%d) -- "
                    "%s on the local radar panel",
                    present ? "IS IN" : "is NOT in", num, present ? "blips" : "no blip");
            g_radarLast = present;
        }
    }

    // Advance the throttle counter once per drone-present tick (NOT inside the save guard below --
    // a transiently-null saveSlot would otherwise stall the SAVE-dump cadence; audit 2026-06-08).
    ++g_tickCounter;

    // ---- order queue: the load-bearing economy signal ([#6][#7]) -------------
    // The static BP RE (bytecode-verified 2026-06-08) proved the ONLY persistent order write is
    // ui_laptop.makeAnOrder -> addOrderCart -> Array_Add(saveSlot.orders) on the LOCAL GameMode
    // (VOTV has no UE replication) -> a CLIENT's order is client-LOCAL and never reaches the host.
    // So poll saveSlot.orders.Num EVERY tick (two cheap derefs) and report the EDGE:
    //   INCREMENT on a CLIENT = [#7] the laptop shop works AND the order stayed local -> confirms
    //                           the OrderRequest client->host edge is required (+ dump order[0] so
    //                           we see what an order carries / that the TSubclassOf `object` reads).
    //   DECREMENT            = the queue DRAIN point the static RE lost (no Array_Remove was found
    //                          in any drone fn). (droneOrder@0x01F0 is never written per the RE --
    //                          dumped throttled below only as a runtime cross-check.)
    const int32_t offSave = Off(gmCls, L"saveSlot");
    void* save = offSave >= 0 ? ReadAt<void*>(gm, offSave) : nullptr;
    if (save && R::IsLive(save)) {
        void* sCls = R::ClassOf(save);
        const int32_t offOrders = Off(sCls, L"orders");  // TArray<Fstruct_storeOrder>
        if (offOrders >= 0) {
            const int ordersNum = ReadAt<int32_t>(save, offOrders + 8);
            if (g_lastOrdersNum == -2) {
                g_lastOrdersNum = ordersNum;  // prime on first observation -- no edge log
            } else if (ordersNum != g_lastOrdersNum) {
                if (ordersNum > g_lastOrdersNum) {
                    // Dump the FRONT order[0] (index 0 -> stride-independent, no struct-stride math).
                    void* ordersData = ReadAt<void*>(save, offOrders);
                    std::wstring objName = L"<no items>";
                    int itemsNum = -1; float etaTime = 0.f;
                    if (ordersData) {
                        void* order0   = ordersData;                  // Fstruct_storeOrder: items@0x00, time@0x10
                        void* itemsData = ReadAt<void*>(order0, 0x00);
                        itemsNum        = ReadAt<int32_t>(order0, 0x08);
                        etaTime         = ReadAt<float>(order0, 0x10);
                        if (itemsData && itemsNum > 0) {
                            void* obj0 = ReadAt<void*>(itemsData, 0x10);  // Fstruct_store.object @0x10 (TSubclassOf, index 0)
                            objName = (obj0 && R::IsLive(obj0)) ? R::ToString(R::NameOf(obj0))
                                                               : std::wstring(L"<null/unresolved>");
                        }
                    }
                    UE_LOGI("[drone_probe] ORDER PLACED [#7]: saveSlot.orders.Num %d->%d "
                            "(order[0]: items=%d obj0='%ls' time=%.1f). On a CLIENT this proves the shop "
                            "works + the order is CLIENT-LOCAL (host never sees it) -> OrderRequest needed; "
                            "obj0 readable confirms the TSubclassOf serializes for the wire.",
                            g_lastOrdersNum, ordersNum, itemsNum, objName.c_str(), etaTime);
                } else {
                    UE_LOGI("[drone_probe] ORDER QUEUE DRAINED: saveSlot.orders.Num %d->%d "
                            "(the drain point -- where the static RE lost the queue consume).",
                            g_lastOrdersNum, ordersNum);
                }
                g_lastOrdersNum = ordersNum;
            }
        }

        // throttled economy cross-check (Points / dailyDelivery / droneOrder) -- ~ every 300 ticks.
        if ((g_tickCounter % 300) == 0) {
            const int32_t offActive = Off(sCls, L"droneOrder");  // Fstruct_storeOrder { items[]@0; time@0x10 }
            const int32_t offPoints = Off(sCls, L"Points");
            const int32_t offDaily  = Off(sCls, L"dailyDelivery");
            const int activeItems = offActive >= 0 ? ReadAt<int32_t>(save, offActive + 8) : -1;
            const int points      = offPoints >= 0 ? ReadAt<int32_t>(save, offPoints)     : -1;
            const int daily       = offDaily  >= 0 ? (int)ReadAt<uint8_t>(save, offDaily) : -1;
            UE_LOGI("[drone_probe] SAVE [#6]: orders.Num=%d droneOrder.items=%d Points=%d dailyDelivery=%d",
                    g_lastOrdersNum, activeItems, points, daily);
        }
    }

    // ---- autonomous one-shot trigger (ini drone_probe_drive=1) -- "run the probe yourself" ----
    if (DriveEnabled() && connected && !g_driveFired) {
        // Settle ~10s after connected + drone-present (we only reach here with a live drone) so the
        // world is loaded + the link stable, then fire ONCE. HOST flies a delivery; CLIENT commits a
        // shop order. The state/orders/cargo/radar edges above capture the result.
        // ~14s after connect+drone-present. Deliberately LONGER than coop::order_sync's connect-time
        // watermark prime (~5s, when the client's saveSlot first resolves) so the auto-placed order
        // lands AFTER the prime -> order_sync forwards it (else the order is swallowed as pre-existing
        // and the v49 economy e2e can't be observed in the autonomous smoke).
        if (++g_settleTicks >= 1800) {
            g_driveFired = true;  // latch BEFORE the call: blocks any re-fire; each precondition bail
                                  // below logs its own reason (re-arm needs a restart -- fine, one-shot)
            UE_LOGI("[drone_probe] DRIVE: firing autonomous trigger (isHost=%d) after settle", isHost ? 1 : 0);
            if (isHost) TriggerHostDelivery(gm, gmCls, drone, dCls);
            else        PlaceClientOrder(gm, gmCls);
        }
    }
}

}  // namespace coop::dev::drone_probe
