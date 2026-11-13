// coop/interactables/atv_gate.cpp -- see coop/interactables/atv_gate.h.

#include "coop/interactables/atv_gate.h"

#include "coop/interactables/atv_host_sync.h"
#include "coop/interactables/atv_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/phys_mods.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::atv_gate {
namespace {

namespace A  = ue_wrap::atv;
namespace H  = coop::atv_host_sync;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace op = coop::net::atv_host_op;

// ATV_C's own bodies; the witch car inherits them, so a Declared row matches it too.
UE_FACT_FN(kRowFuelUp, "atv_gate", L"ATV_C", L"fuelUp", Declared);
UE_FACT_FN(kRowToolboxFix, "atv_gate", L"ATV_C", L"toolboxFix", Declared);
UE_FACT_FN(kRowPlayerUsedOn, "atv_gate", L"ATV_C", L"playerUsedOn", Declared);
UE_FACT_FN(kRowUpgradeTake, "atv_gate", L"ATV_C", L"upgradeTake", Declared);
UE_FACT_FN(kRowHonkPress, "atv_gate", L"ATV_C", L"InpActEvt_atv_honk_K2Node_InputActionEvent_14", Declared);
UE_FACT_FN(kRowHonkRelease, "atv_gate", L"ATV_C", L"InpActEvt_atv_honk_K2Node_InputActionEvent_15", Declared);

enum Row { kFuelUp, kToolbox, kUsedOn, kTake, kHonkPress, kHonkRelease, kRowCount };
const ue_wrap::facts::Function* const kRows[kRowCount] = {&kRowFuelUp,    &kRowToolboxFix, &kRowPlayerUsedOn,
                                                          &kRowUpgradeTake, &kRowHonkPress, &kRowHonkRelease};
const char* const kRowTag[kRowCount] = {"fuelUp", "toolboxFix", "playerUsedOn", "upgradeTake", "honk press", "honk release"};
constexpr int kTagBase = 0x41475400;   // 'AGT', then the row
constexpr float kMax = 100.f;
constexpr size_t kMaxTakeoffs = 8;

std::atomic<bool> g_watched{false};
std::atomic<uint32_t> g_registered{0};   // bit i: row i's watch registered
bool g_settled = false;                  // game thread

// What a call's PRE read, for its POST: one slot a row (none of these bodies re-enters itself).
struct Pend {
    void* obj = nullptr;
    H::Vitals before;
    uint8_t value = 0;       // upgradeTake: the module's value, read at the PRE from the FName
    bool viaPour = false;    // fuelUp: called by gascanFuel
    void* heldBefore = nullptr;
    bool valid = false;
};
Pend g_pend[kRowCount];
uint32_t g_overlapRefused = 0;

struct Takeoff {
    uint32_t reqId;
    ue_wrap::CachedObjRef prop;
};
std::vector<Takeoff> g_takeoffs;

void* HeldActor() {
    void* pawn = coop::players::Registry::Get().Local();
    ue_wrap::engine::MainPlayerGrabState gs{};
    return (pawn && ue_wrap::engine::ReadMainPlayerGrabState(pawn, gs)) ? gs.holdingActor : nullptr;
}

bool CalledBy(const sg::Call& call, const wchar_t* fn) {
    return call.callerFunction && R::IsLive(call.callerFunction) && R::NameEquals(R::NameOf(call.callerFunction), fn);
}

enum class Who : uint8_t { None, Host, Client };

Who RoleNow() {
    coop::net::Session* s = H::LiveSession();
    return !s ? Who::None : s->role() == coop::net::Role::Host ? Who::Host : Who::Client;
}

// ---- PRE: what the call starts from; fuelUp's refusal on a client ----

sg::Verdict OnPre(const sg::Call& call) {
    const int row = call.tag - kTagBase;
    Pend& pd = g_pend[row];
    pd.valid = false;
    if (H::Applying() || row < 0 || row >= kRowCount) return sg::Verdict::Run;
    const Who role = RoleNow();
    if (role == Who::None) return sg::Verdict::Run;
    const bool client = role == Who::Client;
    if (row == kFuelUp) {
        pd.viaPour = CalledBy(call, L"gascanFuel");
        if (client && !pd.viaPour) {
            // The fuelbox's overlap with a can lying in the world: the can is a world prop and the host's copy its author.
            const uint32_t n = ++g_overlapRefused;
            if ((n & (n - 1)) == 0)
                UE_LOGI("atv_gate: fuelUp on %p not from gascanFuel (the fuelbox overlap) refused on this client (#%u)",
                        call.object, n);
            return sg::Verdict::Cancel;
        }
    }
    if (row == kUsedOn && client && call.locals) {
        static R::ParamOffset sPlayer{L"player"};
        const int32_t off = sPlayer.Of(call.function);
        if (off >= 0 && !coop::players::Registry::Get().IsLocal(*reinterpret_cast<void* const*>(call.locals + off)))
            return sg::Verdict::Run;
    }
    if (row == kTake) {
        static R::ParamOffset sItem{L"item"};
        const int32_t off = sItem.Of(call.function);
        if (off < 0 || !call.locals) return sg::Verdict::Run;
        R::FName name{};
        std::memcpy(&name, call.locals + off, sizeof(name));
        // The module's value is read from the name here: at the POST the part is gone from the arrays.
        if (!A::ModuleByName(call.object, name, pd.value)) return sg::Verdict::Run;
        pd.heldBefore = HeldActor();
    }
    if (!H::ReadVitals(call.object, pd.before)) return sg::Verdict::Run;
    pd.obj = call.object;
    pd.valid = true;
    return sg::Verdict::Run;
}

// ---- POST: the host publishes what changed; a client asks for what its copy's verb did ----

bool Has(const H::Vitals& v, uint8_t m) { return std::find(v.modules, v.modules + v.count, m) != v.modules + v.count; }

void OnPost(const sg::Call& call) {
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount) return;
    Pend& pd = g_pend[row];
    if (!pd.valid || pd.obj != call.object) return;
    pd.valid = false;
    if (H::Applying()) return;
    const std::wstring key = A::GetKeyString(call.object);
    if (!coop::atv_sync::AnnounceableAtvKey(key)) return;
    const Who role = RoleNow();
    if (role == Who::Host) {
        H::PublishIfChanged(key, call.object, pd.before);
        return;
    }
    H::Vitals now;
    if (role != Who::Client || !H::ReadVitals(call.object, now)) return;
    switch (row) {
    case kFuelUp: {
        // The pour's gain is the fuel the copy's own body took from the can; a zero gain asks nothing.
        const float gain = std::fmin(now.fuel - pd.before.fuel, kMax);
        if (pd.viaPour && gain > 0.f) {
            if (H::Ask(key, op::kRefuel, gain, 0))
                UE_LOGI("atv_gate: this player poured %.1f into '%ls' -- the host adds it", gain, key.c_str());
        }
        break;
    }
    case kToolbox:
        // Once per hold: the first call that took the health to 100; the frames after it change nothing.
        if (std::floor(pd.before.health) < kMax && std::floor(now.health) >= kMax && H::Ask(key, op::kRepair, 0.f, 0))
            UE_LOGI("atv_gate: this player repaired '%ls' -- the host repairs it", key.c_str());
        break;
    case kUsedOn:
        if (now.count == pd.before.count + 1) {
            for (int i = 0; i < now.count; ++i) {
                if (Has(pd.before, now.modules[i])) continue;
                if (H::Ask(key, op::kFit, 0.f, now.modules[i]))
                    UE_LOGI("atv_gate: this player fitted module %u on '%ls' -- the host fits it",
                            static_cast<unsigned>(now.modules[i]), key.c_str());
                break;
            }
        }
        break;
    case kTake:
        if (now.count + 1 == pd.before.count && !Has(now, pd.value)) {
            const uint32_t id = H::Ask(key, op::kTakeoff, 0.f, pd.value);
            if (!id) break;
            UE_LOGI("atv_gate: this player took module %u off '%ls' -- the host takes it off",
                    static_cast<unsigned>(pd.value), key.c_str());
            // The prop the verb handed over, remembered for a refusal (a take-off the host will not make leaves it a ghost).
            void* held = HeldActor();
            void* cls = ue_wrap::phys_mods::ClassForByte(pd.value);
            if (held && held != pd.heldBefore && cls && R::ClassOf(held) == cls) {
                if (g_takeoffs.size() >= kMaxTakeoffs) g_takeoffs.erase(g_takeoffs.begin());
                g_takeoffs.push_back({id, {}});
                g_takeoffs.back().prop.Set(held);
            }
        }
        break;
    default:
        break;
    }
}

// The honk key's press and release on the copy a player drives. The host's own goes to every client; a client's, as the
// ATV's syncer, goes to the host.
void OnHonkPost(const sg::Call& call) {
    if (H::Applying()) return;
    const bool down = call.tag - kTagBase == kHonkPress;
    const std::wstring key = A::GetKeyString(call.object);
    if (!coop::atv_sync::AnnounceableAtvKey(key)) return;
    const Who role = RoleNow();
    if (role == Who::Host) {
        H::SendHorn(key, down, -1);
        return;
    }
    coop::atv_sync::TableView t;
    if (role == Who::Client && coop::atv_sync::ReadTable(key, t) &&
        t.syncerSlot == coop::players::Registry::Get().LocalPeerId())
        H::AskHorn(key, down);
}

}  // namespace

void Install(coop::net::Session* /*session*/) {
    if (g_watched.exchange(true, std::memory_order_acq_rel)) return;
    int watched = 0;
    for (int i = 0; i < kRowCount; ++i) {
        const bool honk = i == kHonkPress || i == kHonkRelease;
        if (sg::WatchName(*kRows[i], kTagBase + i, honk ? nullptr : &OnPre, honk ? &OnHonkPost : &OnPost)) {
            g_registered.fetch_or(1u << i, std::memory_order_acq_rel);
            ++watched;
        } else {
            UE_LOGE("atv_gate: the watch on %ls::%ls did not register -- a player's %s on an ATV is not carried",
                    kRows[i]->Class(), kRows[i]->Name(), kRowTag[i]);
        }
    }
    UE_LOGI("atv_gate: %d/%d ATV watches registered by class and name", watched, kRowCount);
}

void SettleWatches() {
    const uint32_t registered = g_registered.load(std::memory_order_acquire);
    if (g_settled || !registered) return;
    for (int i = 0; i < kRowCount; ++i)
        if ((registered & (1u << i)) && !sg::NameWatchSettled(*kRows[i], kTagBase + i)) return;
    g_settled = true;
    for (int i = 0; i < kRowCount; ++i)
        if ((registered & (1u << i)) && !sg::NameWatchLive(*kRows[i], kTagBase + i))
            UE_LOGW("atv_gate: the watch on %ls::%ls settled dead -- a player's %s on an ATV is not carried",
                    kRows[i]->Class(), kRows[i]->Name(), kRowTag[i]);
}

void OnAnswer(uint32_t reqId, uint8_t reason) {
    const auto it = std::find_if(g_takeoffs.begin(), g_takeoffs.end(), [&](const Takeoff& t) { return t.reqId == reqId; });
    if (it == g_takeoffs.end()) return;
    void* prop = it->prop.Get();
    g_takeoffs.erase(it);
    if (reason == coop::net::atv_host_reason::kOk || !prop) return;
    if (prop == HeldActor() && ue_wrap::engine::DestroyActor(prop))
        UE_LOGW("atv_gate: the take-off was refused by the host (%u) -- the module it handed this player removed",
                static_cast<unsigned>(reason));
    else
        UE_LOGW("atv_gate: the take-off was refused by the host (%u), and the module it handed over is no longer in "
                "hand -- left", static_cast<unsigned>(reason));
}

void OnDisconnect() {
    g_overlapRefused = 0;
    g_takeoffs.clear();
    for (Pend& p : g_pend) p = Pend{};
}

}  // namespace coop::atv_gate
