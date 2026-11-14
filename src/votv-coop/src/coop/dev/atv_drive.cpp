// coop/dev/atv_drive.cpp -- see coop/dev/atv_drive.h. The acts are what a player's hands do at an ATV: the walk is the bot
// director's, and the brake, throttle, reverse, turbo and dismount keys are the ATV's own input-action events, called as the engine
// calls them when the possessed ATV's input fires.

#include "coop/dev/atv_drive.h"

#include "harness/autotest/peer_signal.h"

#include "coop/dev/director/director.h"
#include "coop/interactables/atv_sync.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::atv_drive {
namespace {

namespace PS = harness::autotest::peer_signal;
namespace DR = coop::director;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
using ue_wrap::FVector;

constexpr int kWalkS = 300;
constexpr int kBurnS = 30;           // a drive holds the throttle until the driver's own fuel has fallen, within this

bool ReadFloat(void* o, const wchar_t* name, float& v) {
    const int32_t off = o ? R::FindPropertyOffset(R::ClassOf(o), name) : -1;
    if (off < 0) return false;
    std::memcpy(&v, static_cast<const uint8_t*>(o) + off, sizeof(v));
    return true;
}

// The director walks the local player to within `reachCm` of `at`; `carry` keeps what the hand holds (the walk set
// without ClearHand). `what` names the goal in the log and in `why`.
bool WalkTo(const FVector& at, float reachCm, bool carry, const char* what, std::string& why) {
    std::shared_ptr<DR::BackgroundWalk> walk;
    OnGt([&] { walk = DR::StartBackgroundWalk(at, reachCm, kWalkS, carry); });
    UE_LOGI("atv-drill: walking to the %s at (%.0f,%.0f,%.0f)", what, at.X, at.Y, at.Z);
    PS::WaitFor("the director's walk", kWalkS + 60, [&] { return walk->state.load() != 0; }, false);
    if (walk->state.load() != 1) {
        DR::CancelBackgroundWalk(*walk);
        why = std::string("the director's walk to the ") + what + " did not arrive";
        return false;
    }
    return true;
}

}  // namespace

void* Me() { return coop::players::Registry::Get().Local(); }

// The ATV this peer holds under `key`. Game thread.
void* Atv(const std::wstring& key) {
    coop::atv_sync::TableView tv;
    return coop::atv_sync::ReadTable(key, tv) ? tv.actor : nullptr;
}

// A function with no parameters, or one key parameter left zero, called as the engine calls it. Game thread.
bool CallByName(void* obj, const wchar_t* name) {
    void* fn = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), name) : nullptr;
    if (!fn) return false;
    const int32_t fs = R::FunctionFrameSize(fn);
    std::vector<uint8_t> frame(fs > 0 ? static_cast<size_t>(fs) : 0, 0u);
    return R::CallFunction(obj, fn, frame.empty() ? nullptr : frame.data());
}

Vitals ReadVitals(const std::wstring& key) {
    Vitals v;
    OnGt([&] {
        void* atv = Atv(key);
        if (!atv) return;
        ReadFloat(atv, L"fuel", v.fuel);
        ReadFloat(atv, L"health", v.health);
        const int32_t off = R::FindPropertyOffset(R::ClassOf(atv), L"modules");
        if (off >= 0) v.modules = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(atv) + off + 8);
        v.seated = coop::atv_sync::ActorSeatedBy(coop::players::Registry::Get().LocalPeerId()) == atv;
    });
    return v;
}

bool WalkToAtv(const std::wstring& key, std::string& why) {
    FVector at{};
    bool read = false;
    OnGt([&] {
        void* atv = Atv(key);
        read = atv && E::TryGetActorLocation(atv, at);
    });
    if (!read) {
        why = "the ATV's place does not read";
        return false;
    }
    return WalkTo(at, kReachCm, false, "ATV", why);
}

bool PressAtv(const std::wstring& key, const wchar_t* fn, std::string& why) {
    bool ok = false, seated = false;
    OnGt([&] {
        void* atv = Atv(key);
        seated = atv && coop::atv_sync::ActorSeatedBy(coop::players::Registry::Get().LocalPeerId()) == atv;
        ok = seated && CallByName(atv, fn);
    });
    if (!ok) why = seated ? "the ATV's key event did not dispatch" : "the player is not in the seat";
    return ok;
}

bool Leave(const std::wstring& key, std::string& why) {
    if (!PressAtv(key, L"InpActEvt_dismount_K2Node_InputActionEvent_0", why)) return false;
    if (!PS::WaitFor("the player leaves the seat", 10, [&] { return !ReadVitals(key).seated; }, false)) {
        why = "the dismount key left the player in the seat";
        return false;
    }
    return true;
}

bool Brake(const std::wstring& key, std::string& why) {
    return PressAtv(key, L"InpActEvt_atv_brake_K2Node_InputActionEvent_22", why);
}

bool TurboOff(const std::wstring& key, std::string& why) {
    return PressAtv(key, L"InpActEvt_atv_turbo_K2Node_InputActionEvent_13", why);
}

namespace {

// The seated player drives: the brake off (a parked ATV sits on its handbrake), the turbo up, then the throttle key `down`
// pressed and held until the driver's own fuel has fallen or the bound runs out.
bool HoldThrottle(const std::wstring& key, const wchar_t* down, std::string& why) {
    bool braked = false, turbo = false;
    float fuel0 = -1.f;
    OnGt([&] {
        void* atv = Atv(key);
        braked = atv && ue_wrap::atv::GetBrake(atv);
        turbo = atv && ue_wrap::atv::GetTurbo(atv);
        if (atv) ReadFloat(atv, L"fuel", fuel0);
    });
    if (braked && !Brake(key, why)) return false;
    // Step 5 leaves the turbo key down; at turbo speed the ATV struck something within 2 s and threw its driver out (f3-green).
    if (turbo && !TurboOff(key, why)) return false;
    if (!PressAtv(key, down, why)) return false;
    // Decided on whichever comes first: the driver's own fuel falls, or the driver is out of the seat (nothing burns then).
    bool thrown = false;
    const bool burned = PS::WaitFor("the driver's own fuel falls", kBurnS, [&] {
        const Vitals v = ReadVitals(key);
        thrown = !v.seated;
        return thrown || v.fuel < fuel0 - 0.5f;
    }, false) && !thrown;
    if (!burned) why = thrown ? "the ATV threw its driver out of the seat before any fuel burned"
                              : "the throttle was held and this copy's own fuel did not fall";
    return burned;
}

}  // namespace

// The throttle stays down for the step; DriveOff lets it go.
bool DriveOn(const std::wstring& key, std::string& why) {
    return HoldThrottle(key, L"InpActEvt_atv_forward_K2Node_InputActionEvent_4", why);
}

void DriveOff(const std::wstring& key) {
    std::string unused;
    PressAtv(key, L"InpActEvt_atv_forward_K2Node_InputActionEvent_5", unused);
}

// The ATV's reverse is its own `atv_back` action: the press sets input_back, which the tick reads as the opposite sign of the
// forward axis; DriveBackOff is the release.
bool DriveBack(const std::wstring& key, std::string& why) {
    return HoldThrottle(key, L"InpActEvt_atv_back_K2Node_InputActionEvent_6", why);
}

void DriveBackOff(const std::wstring& key) {
    std::string unused;
    PressAtv(key, L"InpActEvt_atv_back_K2Node_InputActionEvent_7", unused);
}

}  // namespace coop::dev::atv_drive
