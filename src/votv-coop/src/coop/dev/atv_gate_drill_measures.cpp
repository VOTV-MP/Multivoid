// coop/dev/atv_gate_drill_measures.cpp -- what the ATV gate drill measures beyond a player's acts on the ATV: a drained
// tank for a refuel's gain, the fuel of a copy nobody drives, the horn's replay on a copy nobody sits in, and a can
// dropped over the fuelbox. See coop/dev/atv_gate_drill.h; the player's own acts are atv_gate_drill_acts.cpp.

#include "atv_gate_drill_detail.h"

#include "harness/autotest/peer_signal.h"

#include "coop/dev/atv_gate_probe.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/input_stub.h"
#include "coop/player/hand_item.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_syncer.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <cmath>
#include <string>

namespace coop::dev::atv_gate_drill::detail {

namespace PS = harness::autotest::peer_signal;
namespace PR = coop::dev::atv_gate_probe;
namespace DR = coop::director;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
using ue_wrap::FRotator;
using ue_wrap::FVector;

// ---- what the increment measures beyond the player's acts ----

namespace {

constexpr int   kBurnS = 10;              // the window a copy's fuel is read over while the other peer sits in the seat
constexpr int   kDropS = 3;               // the can leaves the hand within this of the drop key
constexpr float kFuelboxReachCm = 100.f;  // the walk to the fuelbox ends this near it
constexpr float kAimToleranceDeg = 6.f;

// The drop key's press and release, the pair a tap makes.
DR::InputStub g_dropPress{L"InpActEvt_drop_K2Node_InputActionEvent_0"};
DR::InputStub g_dropRelease{L"InpActEvt_drop_K2Node_InputActionEvent_1"};

std::string g_note;   // the drill thread's; see TakeNote

// The AudioComponents attached to the ATV's fuelbox, where the horn's two SpawnSoundAttached calls put their sounds; -1
// when the ATV has no fuelbox. Game thread.
int HornAudio(void* atv) {
    void* const box = ObjProp(atv, L"fuelbox");
    if (!box) return -1;
    int n = 0;
    for (void* c : R::FindObjectsByClass(L"AudioComponent"))
        if (c && R::IsLive(c) && !R::NameStartsWith(R::NameOf(c), L"Default__") && ObjProp(c, L"AttachParent") == box) ++n;
    return n;
}

struct Horn {
    int alt = -1, fly = -1, hasFly = -1, audio = -1;
};

Horn ReadHorn(const std::wstring& key) {
    Horn h;
    const Vitals v = ReadVitals(key);
    h.alt = v.alt;
    h.fly = v.fly;
    h.hasFly = v.hasFly;
    OnGt([&] { h.audio = HornAudio(Atv(key)); });
    return h;
}

// One horn event on this peer's own copy while nobody sits, the horn's fields read around it.
bool HonkEvent(const std::wstring& key, const wchar_t* event, const char* label, std::string& why) {
    bool nobody = false;
    OnGt([&] {
        void* atv = Atv(key);
        nobody = atv && !ue_wrap::atv::IsDriven(atv);
    });
    if (!nobody) {
        why = "the ATV is driven or gone: the horn replay needs nobody in the seat";
        return false;
    }
    const Horn a = ReadHorn(key);
    bool ok = false;
    OnGt([&] { ok = CallByName(Atv(key), event); });
    if (!ok) {
        why = "the horn event did not dispatch";
        return false;
    }
    const Horn b = ReadHorn(key);
    UE_LOGI("[ATVG-DRILL] MEASURE honkreplay_%s detail role=%s input_alt=%d>%d fly=%d>%d hasFly=%d hornaudio=%d>%d", label,
            RoleName(), a.alt, b.alt, a.fly, b.fly, b.hasFly, a.audio, b.audio);
    return true;
}

// The camera's forward lies within kAimToleranceDeg of the direction to `to`.
bool CameraOn(const FVector& to) {
    constexpr float kDegToRad = 0.01745329252f;
    const FVector cam = E::GetCameraLocation();
    const float dx = to.X - cam.X, dy = to.Y - cam.Y, dz = to.Z - cam.Z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(len > 1.f)) return true;
    const FRotator r = E::GetCameraRotation();
    const float p = r.Pitch * kDegToRad, y = r.Yaw * kDegToRad;
    const float dot = (std::cos(p) * std::cos(y) * dx + std::cos(p) * std::sin(y) * dy + std::sin(p) * dz) / len;
    return dot >= std::cos(kAimToleranceDeg * kDegToRad);
}

// The can as one peer holds it: its fuel (`amount`), whether this peer authors it, and how far it is from the fuelbox;
// with `c0`, what the ATV's refuel functions did since. Game thread.
void LogCan(const char* who, void* can, float before, const FVector* box, const PR::Counts* c0) {
    float after = -1.f, away = -1.f;
    bool authors = false;
    if (can && R::IsLive(can)) {
        ReadFloat(can, L"amount", after);
        authors = coop::prop_syncer::AuthorsActorHere(can);
        FVector at{};
        if (box && E::TryGetActorLocation(can, at))
            away = std::sqrt((at.X - box->X) * (at.X - box->X) + (at.Y - box->Y) * (at.Y - box->Y) +
                             (at.Z - box->Z) * (at.Z - box->Z));
    } else {
        can = nullptr;
    }
    const PR::Counts now = PR::Snapshot();
    const PR::Counts zero;
    const PR::Counts& was = c0 ? *c0 : zero;
    UE_LOGI("[ATVG-DRILL] MEASURE overlap detail role=%s can=%s canfuel=%.1f>%.1f authors_can=%d can_to_fuelbox_cm=%.0f "
            "gascanFuel=%u fuelUp_by_gascanFuel=%u fuelUp_by_other_caller=%u", who, can ? "found" : "none",
            static_cast<double>(before), static_cast<double>(after), authors ? 1 : 0, static_cast<double>(away),
            c0 ? now.gate[PR::kGascanFuel] - was.gate[PR::kGascanFuel] : 0u,
            c0 ? now.fuelUpFromGascanFuel - was.fuelUpFromGascanFuel : 0u,
            c0 ? now.fuelUpOtherCaller - was.fuelUpOtherCaller : 0u);
}

}  // namespace

bool Drain(const std::wstring& key, std::string& why) {
    float before = -1.f, after = -1.f;
    bool ok = false;
    OnGt([&] {
        void* atv = Atv(key);
        ok = atv && ReadFloat(atv, L"fuel", before) && WriteFloat(atv, L"fuel", kLowFuel) && ReadFloat(atv, L"fuel", after);
    });
    if (!ok) why = "the ATV's fuel could not be set";
    UE_LOGI("[ATVG-DRILL] drain role=%s: the ATV's fuel on this copy %.1f -> %.1f", RoleName(), static_cast<double>(before),
            static_cast<double>(after));
    return ok;
}

bool IdleBurn(const std::wstring& key, std::string& why) {
    bool braked = false;
    OnGt([&] {
        void* atv = Atv(key);
        braked = atv && ue_wrap::atv::GetBrake(atv);
    });
    // The brake on first, so the held throttle moves nothing: the window measures the fuel and not a drive.
    if (!braked && !PressAtv(key, L"InpActEvt_atv_brake_K2Node_InputActionEvent_22", why)) return false;
    if (!PressAtv(key, L"InpActEvt_atv_forward_K2Node_InputActionEvent_4", why)) return false;
    PS::WaitFor("the idle-burn window", kBurnS, [] { return false; }, false);
    return PressAtv(key, L"InpActEvt_atv_forward_K2Node_InputActionEvent_5", why);
}

bool HonkReplayPress(const std::wstring& key, std::string& why) {
    return HonkEvent(key, L"InpActEvt_atv_honk_K2Node_InputActionEvent_14", "press", why);
}

bool HonkReplayRelease(const std::wstring& key, std::string& why) {
    return HonkEvent(key, L"InpActEvt_atv_honk_K2Node_InputActionEvent_15", "release", why);
}

// The can walks to the fuelbox in the hand (the walk set that keeps the hand), the camera turns on the fuelbox, and the
// drop key is tapped: the game's own release of the held prop. Where the can lands is the game's; the distance from the
// fuelbox is logged with what the ATV's functions did.
bool DropCanInFuelbox(const std::wstring& key, std::string& why) {
    if (!TakeHand(L"gascan", kCanClass, &kCanFuel)) {
        why = "the gas can did not reach the hand";
        return false;
    }
    FVector box{};
    bool read = false;
    OnGt([&] {
        void* comp = ObjProp(Atv(key), L"fuelbox");
        read = comp && E::TryGetComponentLocation(comp, box);
    });
    if (!read) {
        why = "the ATV's fuelbox component does not read";
        return false;
    }
    if (!WalkTo(box, kFuelboxReachCm, true, "fuelbox", why)) return false;
    bool aimed = false;
    OnGt([&] {
        void* p = Me();
        void* c = p ? E::GetController(p) : nullptr;
        if (!c) return;
        E::SetControlRotation(c, DR::LookAt(E::GetCameraLocation(), box));
        aimed = true;
    });
    if (!aimed) {
        why = "no controller to aim at the fuelbox";
        return false;
    }
    PS::WaitFor("the camera turns on the fuelbox", 2, [&] {
        bool on = false;
        OnGt([&] { on = CameraOn(box); });
        return on;
    }, false);
    if (!HandIs(kCanClass)) {
        why = "the can left the hand during the walk";
        return false;
    }
    float before = -1.f;
    OnGt([&] { ReadFloat(coop::hand_item::LocalHandActor(), L"amount", before); });
    const PR::Counts base = ProbeCounts();
    bool tapped = false;
    OnGt([&] { tapped = DR::PressInputEvent(Me(), g_dropPress) && DR::PressInputEvent(Me(), g_dropRelease); });
    if (!tapped) {
        why = "the drop key did not dispatch";
        return false;
    }
    if (!PS::WaitFor("the can leaves the hand", kDropS, [] { return HandEmpty(); }, false)) {
        why = "the drop key left the can in the hand";
        return false;
    }
    void* can = nullptr;
    std::wstring canKey;
    OnGt([&] {
        can = E::ReadMainPlayerDroppedItem(Me());
        if (can && R::IsLive(can)) canKey = ue_wrap::prop::GetInteractableKeyString(can);
        else can = nullptr;
    });
    g_note = canKey.empty() || canKey == L"None" ? std::string() : "canKey=" + std::string(canKey.begin(), canKey.end());
    PS::WaitFor("the fuelbox's overlap refuels", kAnswerS, [&] { return Rose(base, {PR::kFuelUp}); }, false);
    OnGt([&] { LogCan(RoleName(), can, before, &box, &base); });
    return true;
}

std::string TakeNote() {
    std::string n;
    n.swap(g_note);
    return n;
}

void ObserveNote(const char* step, const std::string& note) {
    if (std::string(step) != "overlap") return;
    static const std::string kPrefix = "canKey=";
    const bool named = note.compare(0, kPrefix.size(), kPrefix) == 0;
    const std::wstring canKey(note.begin() + (named ? kPrefix.size() : note.size()), note.end());
    void* can = nullptr;
    if (!canKey.empty())
        PS::WaitFor("the can's copy under its key", 5, [&] {
            OnGt([&] { can = coop::prop_key_index::Find(canKey); });
            return can != nullptr;
        }, false);
    OnGt([&] { LogCan(RoleName(), can, -1.f, nullptr, nullptr); });
}

}  // namespace coop::dev::atv_gate_drill::detail
