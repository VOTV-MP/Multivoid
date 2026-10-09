// ue_wrap/devices/serverbox.cpp -- see ue_wrap/devices/serverbox.h.

#include "ue_wrap/devices/serverbox.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/economy.h"          // SaveSlotPtr: the repair record's saveSlot
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"  // GetComponentLocation
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/engine/world_identity.h"

#include <chrono>
#include <cstring>

namespace ue_wrap::serverbox {
namespace {

namespace R = reflection;
namespace P = profile;

using field_io::TArrayView;
using field_io::ReadFStringAt;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Every group latches after this many passes that had the class in hand and still came up short:
// a member missing from a loaded class does not appear later, so the retry is only worth the
// window where the class itself is still loading, and a pass that keeps warning is a warning once
// a second forever.
constexpr int kMaxPostClassAttempts = 5;

int32_t g_offName   = -1;
void*   g_fnProcess = nullptr;  // pocessFloppy
void*   g_fnEject   = nullptr;  // ejectFloppy
bool     g_resolved = false;
bool     g_verbsLatchedOff = false;
int      g_verbAttempts = 0;
uint64_t g_nextTryMs = 0;

// The box list resolves on its own: the break-and-fix lane wants the boxes and nothing else, so a
// slot field this wrapper cannot find must not cost it the list.
int32_t  g_offServers = -1;  // mainGamemode_C.servers
uint64_t g_nextServersTryMs = 0;

bool EnsureServersResolved() {
    if (g_offServers >= 0) return true;
    const uint64_t now = NowMs();
    if (now < g_nextServersTryMs) return false;
    g_nextServersTryMs = now + 1000;
    void* gmCls = R::FindClass(P::name::GamemodeClass);
    if (!gmCls) return false;
    g_offServers = R::FindPropertyOffset(gmCls, L"servers");
    if (g_offServers < 0) return false;
    UE_LOGI("serverbox: server list resolved (servers=0x%X)", g_offServers);
    return true;
}

// ---- break state: the gamemode's three totals, the box's flag, and its re-skin ----------------

int32_t g_offBroken   = -1;  // mainGamemode_C.brokenServers
int32_t g_offEffCalc  = -1;  // serverEfficiency_calc
int32_t g_offEffDownl = -1;  // serverEfficiency_downl
int32_t g_offIsBroken = -1;  // serverBox_C.IsBroken, byte offset
uint8_t g_maskIsBroken = 0;  // ...and its FBoolProperty real bit
void*   g_fnCheck = nullptr; // serverBox_C::check()
bool     g_breakResolved = false;
bool     g_breakLatchedOff = false;
int      g_breakAttempts = 0;
uint64_t g_nextBreakTryMs = 0;

bool BreakGroupComplete() {
    return g_offBroken >= 0 && g_offEffCalc >= 0 && g_offEffDownl >= 0 && g_offIsBroken >= 0 &&
           g_maskIsBroken != 0 && g_fnCheck != nullptr;
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved) return true;
    if (g_verbsLatchedOff) return false;
    const uint64_t now = NowMs();
    if (now < g_nextTryMs) return false;
    g_nextTryMs = now + 1000;

    void* cls = R::FindClass(L"serverBox_C");
    if (!cls) return false;  // world not loaded yet

    g_offName   = R::FindPropertyOffset(cls, L"name");
    g_fnProcess = R::FindFunction(cls, L"pocessFloppy");
    g_fnEject   = R::FindFunction(cls, L"ejectFloppy");

    // No offset fallbacks: an unresolved member here means the class is not the one this wrapper
    // was written against, and a guessed offset would write into whatever now lives there.
    if (g_offName < 0 || !g_fnProcess || !g_fnEject) {
        if (++g_verbAttempts >= kMaxPostClassAttempts) {
            g_verbsLatchedOff = true;
            UE_LOGW("serverbox: resolution incomplete after %d passes (name=%d pocessFloppy=%p "
                    "ejectFloppy=%p) -- the box's verbs stay off; game version mismatch?",
                    g_verbAttempts, g_offName, g_fnProcess, g_fnEject);
        }
        return false;
    }
    g_resolved = true;
    UE_LOGI("serverbox: resolved (name=0x%X insert=%p eject=%p)", g_offName, g_fnProcess,
            g_fnEject);
    return true;
}

size_t ReadServers(std::vector<void*>& out) {
    if (!EnsureServersResolved()) return 0;
    void* gm = world_singleton::Gamemode();
    if (!gm) return 0;
    const auto* arr = reinterpret_cast<const TArrayView*>(
        reinterpret_cast<const uint8_t*>(gm) + g_offServers);
    if (!arr->data || arr->num <= 0) return 0;
    void* const* elems = reinterpret_cast<void* const*>(arr->data);
    const size_t before = out.size();
    for (int32_t i = 0; i < arr->num; ++i) out.push_back(elems[i]);
    return out.size() - before;
}

std::wstring ReadName(void* box) {
    if (!box || !g_resolved) return std::wstring();
    return ReadFStringAt(box, g_offName);
}

bool CallProcessFloppy(void* box, void* discActor) {
    if (!box || !discActor || !g_resolved) return false;
    ParamFrame f(g_fnProcess);
    if (!f.valid()) return false;
    if (!f.Set(L"Object", discActor)) return false;
    return Call(box, f);
}

bool CallEjectFloppy(void* box) {
    if (!box || !g_resolved) return false;
    ParamFrame f(g_fnEject);
    if (!f.valid()) return false;
    return Call(box, f);
}

bool EnsureBreakResolved() {
    if (g_breakResolved) return true;
    if (g_breakLatchedOff) return false;
    const uint64_t now = NowMs();
    if (now < g_nextBreakTryMs) return false;
    g_nextBreakTryMs = now + 2000;

    void* gmCls = R::FindClass(P::name::GamemodeClass);
    void* sbCls = R::FindClass(L"serverBox_C");
    if (!gmCls || !sbCls) return false;  // world not loaded yet

    if (g_offBroken   < 0) g_offBroken   = R::FindPropertyOffset(gmCls, L"brokenServers");
    if (g_offEffCalc  < 0) g_offEffCalc  = R::FindPropertyOffset(gmCls, L"serverEfficiency_calc");
    if (g_offEffDownl < 0) g_offEffDownl = R::FindPropertyOffset(gmCls, L"serverEfficiency_downl");
    if (g_offIsBroken < 0) R::FindBoolProperty(sbCls, L"IsBroken", g_offIsBroken, g_maskIsBroken);
    if (!g_fnCheck) g_fnCheck = R::FindFunction(sbCls, L"check");

    if (BreakGroupComplete()) {
        g_breakResolved = true;
        UE_LOGI("serverbox: break state resolved (broken=0x%X eff=0x%X/0x%X IsBroken=0x%X "
                "mask=0x%02X check=yes)", g_offBroken, g_offEffCalc, g_offEffDownl, g_offIsBroken,
                g_maskIsBroken);
        return true;
    }
    if (++g_breakAttempts >= kMaxPostClassAttempts) {
        g_breakLatchedOff = true;
        UE_LOGW("serverbox: break state INCOMPLETE after %d passes (broken=0x%X IsBroken=0x%X "
                "check=%s) -- latched off; game version mismatch?", g_breakAttempts, g_offBroken,
                g_offIsBroken, g_fnCheck ? "yes" : "no");
    }
    return false;
}

bool ReadIsBroken(void* box) {
    if (!box || !g_breakResolved) return false;
    const uint8_t b = *(reinterpret_cast<const uint8_t*>(box) + g_offIsBroken);
    return (b & g_maskIsBroken) != 0;
}

bool ApplyBreak(void* box, bool broken) {
    if (!box || !g_breakResolved) return false;
    uint8_t* p = reinterpret_cast<uint8_t*>(box) + g_offIsBroken;
    if (broken) *p |= g_maskIsBroken;
    else        *p &= static_cast<uint8_t>(~g_maskIsBroken);
    ParamFrame f(g_fnCheck);
    if (!f.valid()) return false;
    return Call(box, f);
}

bool ReadAggregates(Aggregates& out) {
    if (!g_breakResolved) return false;
    void* gm = world_singleton::Gamemode();
    if (!gm) return false;
    const auto* base = reinterpret_cast<const uint8_t*>(gm);
    out.brokenServers      = *reinterpret_cast<const int32_t*>(base + g_offBroken);
    out.efficiencyCalc     = *reinterpret_cast<const float*>  (base + g_offEffCalc);
    out.efficiencyDownload = *reinterpret_cast<const float*>  (base + g_offEffDownl);
    return true;
}

bool WriteAggregates(const Aggregates& in) {
    if (!g_breakResolved) return false;
    void* gm = world_singleton::Gamemode();
    if (!gm) return false;
    auto* base = reinterpret_cast<uint8_t*>(gm);
    *reinterpret_cast<int32_t*>(base + g_offBroken)   = in.brokenServers;
    *reinterpret_cast<float*>  (base + g_offEffCalc)  = in.efficiencyCalc;
    *reinterpret_cast<float*>  (base + g_offEffDownl) = in.efficiencyDownload;
    return true;
}

// ---- repair ------------------------------------------------------------------------------------------

namespace {
int32_t  g_offDamaged = -1;        // serverBox_C.damaged, byte offset
uint8_t  g_maskDamaged = 0;        // ...and its real bit
int32_t  g_offMinigame = -1;       // serverBox_C.minigame (int)
void*    g_fnFix = nullptr;        // serverBox_C::fix()
void*    g_fnBreakServer = nullptr;  // serverBox_C::breakServer()
int32_t  g_offGmWidget = -1;       // mainGamemode_C.serverMinigame
bool     g_repairResolved = false;
bool     g_repairLatchedOff = false;
int      g_repairAttempts = 0;
uint64_t g_nextRepairTryMs = 0;

void* RepairWidget() {
    void* gm = world_singleton::Gamemode();
    if (!gm || g_offGmWidget < 0) return nullptr;
    void* w = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_offGmWidget);
    return (w && R::IsLive(w)) ? w : nullptr;
}
}  // namespace

bool EnsureRepairResolved() {
    if (g_repairResolved) return true;
    if (g_repairLatchedOff) return false;
    const uint64_t now = NowMs();
    if (now < g_nextRepairTryMs) return false;
    g_nextRepairTryMs = now + 2000;
    void* gmCls = R::FindClass(P::name::GamemodeClass);
    void* sbCls = R::FindClass(L"serverBox_C");
    if (!gmCls || !sbCls) return false;  // world not loaded yet
    if (g_offDamaged < 0) R::FindBoolProperty(sbCls, L"damaged", g_offDamaged, g_maskDamaged);
    if (g_offMinigame < 0) g_offMinigame = R::FindPropertyOffset(sbCls, L"minigame");
    if (!g_fnFix) g_fnFix = R::FindFunction(sbCls, L"fix");
    if (!g_fnBreakServer) g_fnBreakServer = R::FindFunction(sbCls, L"breakServer");
    if (g_offGmWidget < 0) g_offGmWidget = R::FindPropertyOffset(gmCls, L"serverMinigame");
    if (g_offDamaged >= 0 && g_maskDamaged != 0 && g_offMinigame >= 0 && g_fnFix && g_fnBreakServer &&
        g_offGmWidget >= 0) {
        g_repairResolved = true;
        UE_LOGI("serverbox: repair group resolved (damaged=0x%X mask=0x%02X minigame=0x%X widget=0x%X)", g_offDamaged,
                g_maskDamaged, g_offMinigame, g_offGmWidget);
        return true;
    }
    if (++g_repairAttempts >= kMaxPostClassAttempts) {
        g_repairLatchedOff = true;
        UE_LOGW("serverbox: repair group INCOMPLETE after %d passes (damaged=0x%X minigame=0x%X fix=%s break=%s "
                "widget=0x%X) -- latched off", g_repairAttempts, g_offDamaged, g_offMinigame, g_fnFix ? "yes" : "no",
                g_fnBreakServer ? "yes" : "no", g_offGmWidget);
    }
    return false;
}

bool ReadRepairState(void* box, RepairState& out) {
    if (!box || !g_repairResolved) return false;
    const auto* base = reinterpret_cast<const uint8_t*>(box);
    out.damaged = (base[g_offDamaged] & g_maskDamaged) != 0;
    out.minigame = *reinterpret_cast<const int32_t*>(base + g_offMinigame);
    return true;
}

bool WriteRepairState(void* box, const RepairState& in) {
    if (!box || !g_repairResolved) return false;
    auto* base = reinterpret_cast<uint8_t*>(box);
    if (in.damaged) base[g_offDamaged] |= g_maskDamaged;
    else            base[g_offDamaged] &= static_cast<uint8_t>(~g_maskDamaged);
    *reinterpret_cast<int32_t*>(base + g_offMinigame) = in.minigame;
    return true;
}

bool CallFix(void* box) {
    if (!box || !g_repairResolved) return false;
    ParamFrame f(g_fnFix);
    return f.valid() && Call(box, f);
}

bool CallBreakServer(void* box) {
    if (!box || !g_repairResolved) return false;
    ParamFrame f(g_fnBreakServer);
    return f.valid() && Call(box, f);
}

bool IsRepairWidget(void* obj) { return obj && obj == RepairWidget(); }

bool CallRepairEnd(void* box, bool correct) {
    void* w = box ? RepairWidget() : nullptr;
    if (!w) return false;
    void* wCls = R::ClassOf(w);
    const int32_t offServer = R::FindPropertyOffset(wCls, L"server");
    void* fn = R::FindDispatchFunctionCached(wCls, L"end");
    if (offServer < 0 || !fn) return false;
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(w) + offServer) = box;
    ParamFrame f(fn);
    return f.valid() && f.Set<bool>(L"correct", correct) && Call(w, f);
}

namespace {
void*    g_rewardCls = nullptr;     // the widget class the two reward offsets were read from
int32_t  g_offIsLol = -1;           // ui_serverMinigame_C.isLol, its byte and bit
uint8_t  g_maskIsLol = 0;
int32_t  g_offTime = -1;            // ui_serverMinigame_C.time (float, the solve time)
void*    g_bestCls = nullptr;       // the saveSlot class servertimeBest was read from
int32_t  g_offBest = -1;            // saveSlot.servertimeBest (float)

float* RepairBestField() {
    void* save = economy::SaveSlotPtr();
    if (!save) return nullptr;
    void* cls = R::ClassOf(save);
    if (cls != g_bestCls) {
        g_bestCls = cls;
        g_offBest = R::FindPropertyOffset(cls, L"servertimeBest");
    }
    return g_offBest < 0 ? nullptr : reinterpret_cast<float*>(static_cast<uint8_t*>(save) + g_offBest);
}
}  // namespace

bool ReadRepairReward(void* widget, bool& lol, float& time) {
    if (!widget) return false;
    void* cls = R::ClassOf(widget);
    if (cls != g_rewardCls) {
        g_rewardCls = cls;
        g_maskIsLol = 0;
        if (!R::FindBoolProperty(cls, L"isLol", g_offIsLol, g_maskIsLol)) g_offIsLol = -1;
        g_offTime = R::FindPropertyOffset(cls, L"time");
    }
    if (g_offIsLol < 0 || g_offTime < 0) return false;
    const auto* base = static_cast<const uint8_t*>(widget);
    lol = (base[g_offIsLol] & g_maskIsLol) != 0;
    std::memcpy(&time, base + g_offTime, sizeof(time));
    return true;
}

bool ReadRepairBest(float& out) {
    const float* f = RepairBestField();
    if (!f) return false;
    out = *f;
    return true;
}

bool WriteRepairBest(float value) {
    float* f = RepairBestField();
    if (!f) return false;
    *f = value;
    return true;
}

namespace {
int32_t  g_offUpgrades = -1;        // serverBox_C.upgrades (int)
bool     g_upgradesMissing = false; // the class loaded without the member: never retried
CachedObjRef g_spawnerCls;          // initialServerUpgradeSpawn_C
CachedObjRef g_upgPropCls;          // prop_serverUpg_C, found again in each world that loads it
int32_t  g_offTakeUpgrade = -1;     // serverBox_C.takeUpgrade (UBoxComponent*)
int32_t  g_lookByte = -1;           // serverBox_C.lookatUpgrades, its byte and bit
uint8_t  g_lookMask = 0;
bool     g_spawnerMissed = false;   // the last lookup missed, in world generation g_spawnerMissGen
uint32_t g_spawnerMissGen = 0;
}  // namespace

bool ReadUpgrades(void* box, int32_t& out) {
    if (!box || g_upgradesMissing) return false;
    if (g_offUpgrades < 0) {
        void* cls = R::FindClass(L"serverBox_C");
        if (!cls) return false;
        g_offUpgrades = R::FindPropertyOffset(cls, L"upgrades");
        if (g_offUpgrades < 0) {
            g_upgradesMissing = true;
            UE_LOGW("serverbox: serverBox_C carries no 'upgrades' -- the upgrade reads stay off");
            return false;
        }
    }
    out = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(box) + g_offUpgrades);
    return true;
}

bool WriteUpgrades(void* box, int32_t level) {
    int32_t cur = 0;
    if (!ReadUpgrades(box, cur)) return false;  // resolves the member
    // Looked up on the box's own class at each call, through the lookup memoised by slot and serial: a
    // placed box's class is the level's, and never kept by pointer across worlds.
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(box), L"updUpgrades");
    if (!fn) return false;
    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(box) + g_offUpgrades) = level;
    ParamFrame f(fn);
    return f.valid() && Call(box, f);
}

int32_t IndexOf(void* box) {
    if (!box) return -1;
    std::vector<void*> servers;
    ReadServers(servers);
    for (size_t i = 0; i < servers.size(); ++i)
        if (servers[i] == box) return static_cast<int32_t>(i);
    return -1;
}

bool IsUpgradeClass(void* cls) {
    if (!cls) return false;
    if (!g_upgPropCls.Alive()) {
        void* found = object_index::ClassByName(L"prop_serverUpg_C");
        if (!found) return false;
        g_upgPropCls.Set(found);
    }
    void* base = g_upgPropCls.Raw();
    return R::IsDescendantOfAny(cls, &base, 1, 8);
}

void ForEachUpgrade(UpgradeFn fn, void* ctx) {
    struct Visit { UpgradeFn fn; void* ctx; } v{fn, ctx};
    object_index::ForEachClass([](void* c, void* cls, void*) {
        if (!IsUpgradeClass(cls)) return;
        object_index::ForEachInstance(cls, [](void* c2, void* obj, int32_t index) {
            // An index member may still be loading, under construction or dying; the slot's flags say so.
            if (!obj || (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable))) return;
            if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            auto* vv = static_cast<Visit*>(c2);
            vv->fn(vv->ctx, obj);
        }, c);
    }, &v);
}

void* SpawnUpgradeProp(const FVector& at) {
    void* cls = object_index::ClassByName(L"prop_serverUpg_1_C");
    return cls ? engine::SpawnActor(cls, at) : nullptr;
}

void* TakeOutComponent(void* box) {
    if (!box) return nullptr;
    if (g_offTakeUpgrade < 0) g_offTakeUpgrade = R::FindPropertyOffset(R::ClassOf(box), L"takeUpgrade");
    if (g_offTakeUpgrade < 0) return nullptr;
    void* comp = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(box) + g_offTakeUpgrade);
    return (comp && R::IsLive(comp)) ? comp : nullptr;
}

bool ReadLooksAtTakeOut(void* box, bool& out) {
    if (!box) return false;
    if (g_lookByte < 0 && !R::FindBoolProperty(R::ClassOf(box), L"lookatUpgrades", g_lookByte, g_lookMask))
        return false;
    out = (*(reinterpret_cast<const uint8_t*>(box) + g_lookByte) & g_lookMask) != 0;
    return true;
}

bool CallInstall(void* box, void* player, void* held, const R::FName& heldName) {
    void* fn = box ? R::FindDispatchFunctionCached(R::ClassOf(box), kInstallVerb) : nullptr;
    void* comp = TakeOutComponent(box);  // a component of the box for the hit, which the install never reads
    if (!fn || !comp || !player || !held) return false;
    const FVector at = engine::GetComponentLocation(comp);
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && hit_result::Write(f, L"hit", box, comp, at) &&
           f.Set<void*>(L"lookAtComponent", comp) && f.Set<void*>(L"holdObject", held) &&
           f.Set<R::FName>(L"holdPropName", heldName) && Call(box, f);
}

bool CallTakeOut(void* box, void* player) {
    void* fn = box ? R::FindDispatchFunctionCached(R::ClassOf(box), kTakeOutVerb) : nullptr;
    void* comp = TakeOutComponent(box);
    if (!fn || !comp || !player) return false;
    const FVector at = engine::GetComponentLocation(comp);
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && hit_result::Write(f, L"hit", box, comp, at) &&
           f.Set<uint8_t>(L"action", 4) && f.Set<void*>(L"lookAtComponent", comp) && Call(box, f);
}

int32_t CountUpgradeSpawners() {
    if (!g_spawnerCls.Alive()) {
        // A class lookup that misses walks the object array, so after a miss the next lookup waits
        // for a new world: the class loads with a world or not at all.
        const uint32_t gen = world_identity::Generation();
        if (g_spawnerMissed && gen == g_spawnerMissGen) return 0;
        void* cls = R::FindClass(L"initialServerUpgradeSpawn_C");
        if (!cls) {
            g_spawnerMissed = true;
            g_spawnerMissGen = gen;
            return 0;
        }
        g_spawnerMissed = false;
        g_spawnerCls.Set(cls);
    }
    int32_t n = 0;
    object_index::ForEachInstance(g_spawnerCls.Raw(), [](void* ctx, void* obj, int32_t index) {
        // An index member may still be loading, under construction or dying; the slot's flags say so.
        if (!obj || (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable))) return;
        if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) ++*static_cast<int32_t*>(ctx);
    }, &n);
    return n;
}

}  // namespace ue_wrap::serverbox
