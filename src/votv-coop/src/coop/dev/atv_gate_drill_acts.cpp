// coop/dev/atv_gate_drill_acts.cpp -- the acts of the ATV gate drill; see coop/dev/atv_gate_drill.h. Each act is what a
// player's hands do at an ATV, issued at the seam the game's own input reaches:
//   - a held item is used with the fire key (mainPlayer's InpActEvt_fire press and release), the camera turned on the ATV
//     until the player's own interaction trace takes it, so the game's handler of the press runs against that trace;
//   - the module's take-off is the upgrades screen the radial opens (actionName "upgrades", the verb the seat uses for
//     "sit") and the click on a module's button, the function the engine calls when the button's OnClicked fires;
//   - the seat is atv_seat's, the game's own seat verb; the lights, brake, turbo, horn and dismount keys are the ATV's own
//     input-action events, called as the engine calls them when the possessed ATV's input fires.
// No act calls gascanFuel, toolboxFix, playerUsedOn or upgradeTake itself.

#include "atv_gate_drill_detail.h"

#include "atv_seat.h"  // co-located private header

#include "harness/autotest/peer_signal.h"

#include "coop/dev/atv_gate_probe.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/input_stub.h"
#include "coop/dev/director/routes.h"
#include "coop/dev/drill_hand.h"
#include "coop/interactables/atv_sync.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::atv_gate_drill::detail {
namespace {

namespace PS = harness::autotest::peer_signal;
namespace PR = coop::dev::atv_gate_probe;
namespace DR = coop::director;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
using ue_wrap::FRotator;
using ue_wrap::FVector;

constexpr float kReachCm = 220.f;      // the walk ends this near the ATV's root: its body is wider than that, so the player stands beside it
constexpr int   kWalkS = 300;
constexpr int   kAnswerS = 6;          // the game answers a fire press within this, or it did not
constexpr int   kToolboxHoldS = 30;    // the toolbox fixes while the key is held, for the ATV's own fix time
constexpr int   kEffectS = 3;          // a state the answer changed shows within this
constexpr int   kSlotS = 15;           // the upgrades screen's module slot exists within this of the verb
constexpr float kLowFuel = 20.f;
constexpr float kLowHealth = 50.f;
constexpr float kCanFuel = 100.f;      // a gas can's hold record loads empty without it
constexpr int   kToolboxFixes = 10;    // the toolbox class default; its hold record carries none
constexpr const wchar_t* kCanClass = L"prop_gascan_C";
constexpr const wchar_t* kToolboxClass = L"prop_toolbox_C";
constexpr const wchar_t* kModuleRow = L"atvup_lights";
constexpr const wchar_t* kModuleClass = L"prop_atvUpgrade_bigLights_C";
constexpr const wchar_t* kSlotClass = L"uicomp_objectUpgradeSlot_C";
constexpr const wchar_t* kSlotClick =
    L"BndEvt__ui_objectUpgradeSlot_Button_60_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature";
constexpr const wchar_t* kUpgradesClass = L"ui_objectUpgrades_C";

// The fire action's press and release, the pair a left click makes.
DR::InputStub g_firePress{L"InpActEvt_fire_K2Node_InputActionEvent_58"};
DR::InputStub g_fireRelease{L"InpActEvt_fire_K2Node_InputActionEvent_59"};

void* Me() { return coop::players::Registry::Get().Local(); }

// The ATV this peer holds under `key`. Game thread.
void* Atv(const std::wstring& key) {
    coop::atv_sync::TableView tv;
    return coop::atv_sync::ReadTable(key, tv) ? tv.actor : nullptr;
}

bool ReadFloat(void* o, const wchar_t* name, float& v) {
    const int32_t off = o ? R::FindPropertyOffset(R::ClassOf(o), name) : -1;
    if (off < 0) return false;
    std::memcpy(&v, static_cast<const uint8_t*>(o) + off, sizeof(v));
    return true;
}

bool WriteFloat(void* o, const wchar_t* name, float v) {
    const int32_t off = o ? R::FindPropertyOffset(R::ClassOf(o), name) : -1;
    if (off < 0) return false;
    std::memcpy(static_cast<uint8_t*>(o) + off, &v, sizeof(v));
    return true;
}

bool WriteInt(void* o, const wchar_t* name, int32_t v) {
    const int32_t off = o ? R::FindPropertyOffset(R::ClassOf(o), name) : -1;
    if (off < 0) return false;
    std::memcpy(static_cast<uint8_t*>(o) + off, &v, sizeof(v));
    return true;
}

// A function with no parameters, or one key parameter left zero, called as the engine calls it. Game thread.
bool CallByName(void* obj, const wchar_t* name) {
    void* fn = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), name) : nullptr;
    if (!fn) return false;
    const int32_t fs = R::FunctionFrameSize(fn);
    std::vector<uint8_t> frame(fs > 0 ? static_cast<size_t>(fs) : 0, 0u);
    return R::CallFunction(obj, fn, frame.empty() ? nullptr : frame.data());
}

// ---- the hand ----

bool HandIs(const wchar_t* cls) {
    bool in = false;
    OnGt([&] {
        void* h = coop::hand_item::LocalHandActor();
        in = h && R::ClassNameOf(h) == cls;
    });
    return in;
}

bool HandEmpty() {
    bool empty = false;
    OnGt([&] { empty = coop::hand_item::LocalHandActor() == nullptr; });
    return empty;
}

// The item in the local hand as a pick leaves it (drill_hand), waited for. Null `cls` empties the hand.
bool TakeHand(const wchar_t* row, const wchar_t* cls, const float* fuel = nullptr) {
    bool took = false;
    OnGt([&] { took = coop::dev::drill_hand::TakeIntoHandNow(row, cls, fuel); });
    if (!took) return false;
    return PS::WaitFor("the hand's item", 20, [&] { return cls ? HandIs(cls) : HandEmpty(); }, false);
}

// ---- the aim and the fire key ----

// The camera turned at the ATV through a fan of headings around it, each held until the player's own interaction trace
// answers, until the trace strikes the ATV: what the trace takes is what the game's handler of the fire key acts on.
bool AimAtAtv(const std::wstring& key, std::string& why) {
    static constexpr float kYaw[] = {0.f, -5.f, 5.f, -10.f, 10.f, -16.f, 16.f};
    static constexpr float kPitch[] = {0.f, 8.f, -8.f, 16.f, -16.f, 24.f};
    std::wstring other;
    for (float dp : kPitch) {
        for (float dy : kYaw) {
            bool aimed = false;
            OnGt([&] {
                void* p = Me();
                void* c = p ? E::GetController(p) : nullptr;
                void* atv = Atv(key);
                FVector at{};
                if (!c || !atv || !E::TryGetActorLocation(atv, at)) return;
                at.Z += 40.f;
                FRotator r = DR::LookAt(E::GetCameraLocation(), at);
                r.Pitch += dp;
                r.Yaw += dy;
                E::SetControlRotation(c, r);
                aimed = true;
            });
            if (!aimed) {
                why = "no controller or no ATV to aim at";
                return false;
            }
            if (PS::WaitFor("the trace takes the ATV", 1, [&] {
                    bool hit = false;
                    OnGt([&] {
                        void* p = Me();
                        void* a = p ? E::ReadMainPlayerHitActor(p) : nullptr;
                        hit = a && a == Atv(key);
                        if (a && !hit && other.empty()) other = R::ClassNameOf(a);
                    });
                    return hit;
                }, false))
                return true;
        }
    }
    std::string took = other.empty() ? "nothing else" : "";
    for (const wchar_t c : other) took.push_back(c < 128 ? static_cast<char>(c) : '?');
    why = "no aim put the player's trace on the ATV (it took " + took + ")";
    return false;
}

// Has any of `fns` been seen since `base`, by either seam.
bool Rose(const PR::Counts& base, std::initializer_list<PR::Fn> fns) {
    PR::Counts now;
    OnGt([&] { now = PR::Snapshot(); });
    for (PR::Fn f : fns)
        if (now.gate[f] + now.pe[f] > base.gate[f] + base.pe[f]) return true;
    return false;
}

PR::Counts Counts() {
    PR::Counts c;
    OnGt([&] { c = PR::Snapshot(); });
    return c;
}

// The fire key down, held until the game answers one of `fns` (or `boundS`), then let go. False when the key did not
// dispatch; `answered` says whether the game answered.
bool HoldFire(std::initializer_list<PR::Fn> fns, int boundS, bool& answered) {
    const PR::Counts base = Counts();
    bool pressed = false;
    OnGt([&] { pressed = DR::PressInputEvent(Me(), g_firePress); });
    if (!pressed) return false;
    answered = PS::WaitFor("the game answers the fire key", boundS, [&] { return Rose(base, fns); }, false);
    OnGt([&] { DR::PressInputEvent(Me(), g_fireRelease); });
    return true;
}

// The ATV's fuel and health set low on this peer's copy, the precondition of a refuel and a repair.
bool Lower(const std::wstring& key, std::string& why) {
    bool ok = false;
    OnGt([&] {
        void* atv = Atv(key);
        ok = atv && WriteFloat(atv, L"fuel", kLowFuel) && WriteFloat(atv, L"health", kLowHealth);
    });
    if (!ok) why = "the ATV's fuel and health could not be set";
    return ok;
}

}  // namespace

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

std::wstring ChooseAtv() {
    std::wstring key;
    OnGt([&] {
        void* me = Me();
        FVector mine{};
        if (!me || !E::TryGetActorLocation(me, mine)) return;
        float bestLen = 0.f;
        for (void* o : R::FindObjectsByClass(L"ATV_C")) {
            FVector pos{};
            float len = 0.f;
            if (!o || !R::IsLive(o) || R::ClassNameOf(o) != L"ATV_C" || !ue_wrap::atv::IsAtv(o)) continue;
            const std::wstring k = ue_wrap::atv::GetKeyString(o);
            if (!coop::atv_sync::AnnounceableAtvKey(k) || !E::TryGetActorLocation(o, pos)) continue;
            if (!DR::RouteInLegs(me, mine, pos, kReachCm, &len)) continue;
            if (key.empty() || len < bestLen) {
                key = k;
                bestLen = len;
            }
        }
    });
    return key;
}

bool CopyResolves(const std::wstring& key) {
    return PS::WaitFor("the ATV copy under the host's key", 120, [&] {
        void* atv = nullptr;
        OnGt([&] { atv = Atv(key); });
        return atv != nullptr;
    });
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
    std::shared_ptr<DR::BackgroundWalk> walk;
    OnGt([&] { walk = DR::StartBackgroundWalk(at, kReachCm, kWalkS); });
    UE_LOGI("[ATVG-DRILL] walking to the ATV at (%.0f,%.0f,%.0f)", at.X, at.Y, at.Z);
    PS::WaitFor("the director's walk to the ATV", kWalkS + 60, [&] { return walk->state.load() != 0; }, false);
    if (walk->state.load() != 1) {
        DR::CancelBackgroundWalk(*walk);
        why = "the director's walk to the ATV did not arrive";
        return false;
    }
    return true;
}

// ---- the acts on the held item ----

bool PourCan(const std::wstring& key, std::string& why) {
    if (!Lower(key, why)) return false;
    if (!TakeHand(L"gascan", kCanClass, &kCanFuel)) {
        why = "the gas can did not reach the hand";
        return false;
    }
    if (!AimAtAtv(key, why)) return false;
    bool answered = false;
    if (!HoldFire({PR::kGascanFuel, PR::kFuelUp, PR::kPlayerUsedOn}, kAnswerS, answered)) {
        why = "the fire key did not dispatch";
        return false;
    }
    PS::WaitFor("the fuel rises", kEffectS, [&] { return ReadVitals(key).fuel > kLowFuel + 0.5f; }, false);
    UE_LOGI("[ATVG-DRILL] can: the fire key was held on the ATV; an ATV function answered=%d", answered ? 1 : 0);
    return true;
}

bool UseToolbox(const std::wstring& key, std::string& why) {
    if (!Lower(key, why)) return false;
    if (!TakeHand(L"toolbox", kToolboxClass)) {
        why = "the toolbox did not reach the hand";
        return false;
    }
    bool fixes = false;
    OnGt([&] { fixes = WriteInt(coop::hand_item::LocalHandActor(), L"fixes", kToolboxFixes); });
    if (!fixes) {
        why = "the toolbox's fixes could not be set";
        return false;
    }
    if (!AimAtAtv(key, why)) return false;
    bool answered = false;
    if (!HoldFire({PR::kToolboxFix}, kToolboxHoldS, answered)) {
        why = "the fire key did not dispatch";
        return false;
    }
    PS::WaitFor("the health rises", kEffectS, [&] { return ReadVitals(key).health > kLowHealth + 0.5f; }, false);
    UE_LOGI("[ATVG-DRILL] toolbox: the fire key was held on the ATV; toolboxFix answered=%d", answered ? 1 : 0);
    return true;
}

bool FitModule(const std::wstring& key, std::string& why) {
    if (!TakeHand(kModuleRow, kModuleClass)) {
        why = "the module did not reach the hand";
        return false;
    }
    if (!AimAtAtv(key, why)) return false;
    const int before = ReadVitals(key).modules;
    bool answered = false;
    if (!HoldFire({PR::kPlayerUsedOn}, kAnswerS, answered)) {
        why = "the fire key did not dispatch";
        return false;
    }
    PS::WaitFor("the module count rises", kEffectS, [&] { return ReadVitals(key).modules > before; }, false);
    UE_LOGI("[ATVG-DRILL] fit: the fire key was held on the ATV; playerUsedOn answered=%d", answered ? 1 : 0);
    return true;
}

// ---- the take-off ----

namespace {

// The live module slots of the upgrades screen: `total` of them, `named` of those naming `atv` as their actor, and the
// first one that does (null when none). `firstActor` is the class of the first slot's actor, whoever's. Game thread.
void* FindSlot(void* atv, int& total, int& named, std::wstring& firstActor) {
    void* found = nullptr;
    total = named = 0;
    for (void* w : R::FindObjectsByClass(kSlotClass)) {
        if (!w || !R::IsLive(w) || R::NameStartsWith(R::NameOf(w), L"Default__")) continue;
        ++total;
        const int32_t off = R::FindPropertyOffset(R::ClassOf(w), L"actor");
        void* const actor = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(w) + off) : nullptr;
        if (total == 1) firstActor = actor && R::IsLive(actor) ? R::ClassNameOf(actor) : L"none";
        if (atv && actor == atv) {
            ++named;
            if (!found) found = w;
        }
    }
    return found;
}

// The upgrades screen (the game keeps one) closed through its own quit, which gives the input back. Game thread.
void CloseScreen() {
    for (void* w : R::FindObjectsByClass(kUpgradesClass))
        if (w && R::IsLive(w) && !R::NameStartsWith(R::NameOf(w), L"Default__")) CallByName(w, L"quit");
}

int32_t ArrayNum(void* o, const wchar_t* name) {
    const int32_t off = o ? R::FindPropertyOffset(R::ClassOf(o), name) : -1;
    return off < 0 ? -1 : *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(o) + off + 8);
}

// What stood where the slot should be, for the `why` of a take-off that could not be issued: the lists the screen builds
// its slots from (the ATV's upgradesNames and modules), the screens that exist and name the ATV, and the slots. Game thread.
std::string DescribeScreen(void* atv) {
    int total = 0, named = 0, screens = 0, screensNamingAtv = 0;
    std::wstring first;
    FindSlot(atv, total, named, first);
    for (void* w : R::FindObjectsByClass(kUpgradesClass)) {
        if (!w || !R::IsLive(w) || R::NameStartsWith(R::NameOf(w), L"Default__")) continue;
        ++screens;
        const int32_t off = R::FindPropertyOffset(R::ClassOf(w), L"actor");
        if (off >= 0 && atv && *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(w) + off) == atv) ++screensNamingAtv;
    }
    std::string firstActor;
    for (const wchar_t c : first) firstActor.push_back(c < 128 ? static_cast<char>(c) : '?');
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "upgradesNames=%d modules=%d; screens=%d, naming the ATV=%d; slots=%d, naming the ATV=%d, first slot's actor=%s",
                  ArrayNum(atv, L"upgradesNames"), ArrayNum(atv, L"modules"), screens, screensNamingAtv, total, named,
                  firstActor.empty() ? "-" : firstActor.c_str());
    return buf;
}

}  // namespace

// The screen is opened by the verb and its slots are built by the screen's own list, which the game fills in the
// verb's call (objectViewer assign then genList), so a slot is waited for as a state, bounded, before the act is
// given up. A screen that shows none says what it holds in `why`: whether it needed a frame the drill cannot give is
// read from those counts.
bool TakeModuleOff(const std::wstring& key, std::string& why) {
    if (!TakeHand(L"None", nullptr)) {
        why = "the hand could not be emptied";
        return false;
    }
    bool opened = false;
    OnGt([&] {
        void* atv = Atv(key);
        void* me = Me();
        opened = atv && me && atv_seat::CallActionName(me, atv, L"upgrades");
    });
    if (!opened) {
        why = "the ATV's upgrades verb did not run";
        return false;
    }
    void* slot = nullptr;
    PS::WaitFor("the upgrades screen builds a module slot", kSlotS, [&] {
        OnGt([&] {
            int total = 0, named = 0;
            std::wstring first;
            slot = FindSlot(Atv(key), total, named, first);
        });
        return slot != nullptr;
    }, false);
    if (!slot) {
        std::string what;
        OnGt([&] {
            what = DescribeScreen(Atv(key));
            CloseScreen();
        });
        why = "the upgrades screen built no module slot of the ATV in " + std::to_string(kSlotS) + " s (" + what + ")";
        return false;
    }
    const PR::Counts base = Counts();
    bool clicked = false;
    OnGt([&] { clicked = R::IsLive(slot) && CallByName(slot, kSlotClick); });
    if (!clicked) {
        OnGt([&] { CloseScreen(); });
        why = "the module button's click did not dispatch";
        return false;
    }
    const bool answered = PS::WaitFor("the game answers the click", kAnswerS, [&] { return Rose(base, {PR::kUpgradeTake}); }, false);
    OnGt([&] { CloseScreen(); });
    UE_LOGI("[ATVG-DRILL] take: the module's button was clicked on the upgrades screen; upgradeTake answered=%d",
            answered ? 1 : 0);
    return true;
}

// ---- the seat and its keys ----

bool Sit(const std::wstring& key, std::string& why) {
    if (!TakeHand(L"None", nullptr)) {
        why = "the hand could not be emptied for the seat";
        return false;
    }
    atv_seat::SitTry t;
    PS::WaitFor("the seat verb finishes", 40, [&] {
        bool done = false;
        OnGt([&] {
            void* atv = Atv(key);
            void* me = Me();
            done = atv && me && atv_seat::TrySit(me, atv, t);
        });
        return done;
    }, false);
    if (!ReadVitals(key).seated) {
        why = "the seat verb did not seat the player (see the [ATVP] lines)";
        return false;
    }
    return true;
}

namespace {

// One of the ATV's own input-action events, called as the engine calls it on the possessed ATV; the player must be seated.
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

}  // namespace

bool Leave(const std::wstring& key, std::string& why) {
    if (!PressAtv(key, L"InpActEvt_dismount_K2Node_InputActionEvent_0", why)) return false;
    if (!PS::WaitFor("the player leaves the seat", 10, [&] { return !ReadVitals(key).seated; }, false)) {
        why = "the dismount key left the player in the seat";
        return false;
    }
    return true;
}

bool Lights(const std::wstring& key, std::string& why) {
    return PressAtv(key, L"InpActEvt_atv_lights_K2Node_InputActionEvent_3", why);
}

bool Brake(const std::wstring& key, std::string& why) {
    return PressAtv(key, L"InpActEvt_atv_brake_K2Node_InputActionEvent_22", why);
}

bool Turbo(const std::wstring& key, std::string& why) {
    return PressAtv(key, L"InpActEvt_atv_turbo_K2Node_InputActionEvent_12", why) &&
           PressAtv(key, L"InpActEvt_atv_turbo_K2Node_InputActionEvent_13", why);
}

bool Honk(const std::wstring& key, std::string& why) {
    return PressAtv(key, L"InpActEvt_atv_honk_K2Node_InputActionEvent_14", why) &&
           PressAtv(key, L"InpActEvt_atv_honk_K2Node_InputActionEvent_15", why);
}

}  // namespace coop::dev::atv_gate_drill::detail
