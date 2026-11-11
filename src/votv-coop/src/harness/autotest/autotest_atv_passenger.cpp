// harness/autotest/autotest_atv_passenger.cpp -- TEST-ONLY: a player in the ATV's passenger spot, on two peers.
//
// Over the world's ATV at midday, with a kerfur beside it: the host drives and the client takes the spot behind it (the
// passenger lane's own ask, as the use key makes it); the host drives 2 m/s for 2 s; the client's copy is cut 1 m on, as
// the corrector cuts; the host gets off and the passenger stays; the client gets off by its own unsit. Then the client
// drives, its kerfur rides, and the host takes the spot over the kerfur; the client drives on; the host's copy is cut;
// the client gets off and the host keeps the spot; the host gets off. After each step each peer logs
// `atv-passenger: STATE <step>` (the table, its own seat, the other peer's body against the passenger and driver places,
// the riding kerfur) and judges it in `atv-passenger: CHECK` lines; `atv-passenger: VERDICT` closes each peer's run. A
// driver who gets off shoots the passenger from beside the ATV. VOTVCOOP_ATV_PASSENGER_HOLD=1 stops after the client
// boards and leaves both riders on for a watcher, the host taking `forward <seconds>` from the folder's `cmd` file; a
// third peer with VOTVCOOP_ATV_PASSENGER_WATCH=1 logs how far each rider's body stands off its seat. MODE=view is the
// host's sweep of passenger seat places. Gated by VOTVCOOP_RUN_ATV_PASSENGER; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/dev/set_clock.h"
#include "coop/dev/spawn_npc.h"
#include "coop/interactables/atv_passenger.h"
#include "coop/interactables/atv_sync.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
#include "harness/screenshot.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/devices/atv_passenger_seat.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <fstream>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace A = ue_wrap::atv;
namespace P = ue_wrap::profile;
namespace PS = ue_wrap::atv_passenger_seat;

std::string g_dir;
void* g_atv = nullptr;   // game thread only
std::wstring g_key;
int g_checks = 0, g_failed = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

const char* Who() { return IsClientRole() ? "client" : "host"; }

void Write(const std::string& name, const std::string& text) {
    std::ofstream(g_dir + "\\" + name, std::ios::binary | std::ios::trunc) << text;
}

bool Await(const std::string& name, std::string* text = nullptr, int seconds = 240) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty()) {
            if (text) *text = s;
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("atv-passenger: VERDICT ABORT -- no '%hs' from the other peer", name.c_str());
    return false;
}

template <class T>
bool Field(void* obj, const wchar_t* name, T& out) {
    const int32_t off = obj && R::IsLive(obj) ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(obj) + off, sizeof(T));
    return true;
}

void* Me() { return coop::players::Registry::Get().Local(); }

void PlaceMe(const ue_wrap::FVector& at, float yaw);

// The other drill peer's body: the host's (slot 0) on the client, the first client's (slot 1) on the host.
void* OtherBody() {
    coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(IsClientRole() ? 0 : 1);
    return p && p->valid() ? p->GetActor() : nullptr;
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

int AnimOnAtv(void* body) {
    void* comp = body ? ue_wrap::puppet::GetSkeletalMeshComponent(body) : nullptr;
    if (!comp || !R::IsLive(comp)) return -1;
    void* anim = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(comp) +
                                                 P::off::USkeletalMesh_AnimScriptInstance);
    bool on = false;
    return anim && Field(anim, L"isOnAtv", on) ? (on ? 1 : 0) : -1;
}

void* FirstKerfur() {
    for (void* o : R::FindObjectsByClass(L"kerfurOmega_C"))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) return o;
    return nullptr;
}

// What one peer sees after a step.
struct Seen {
    uint8_t occupant = 0xFF, passenger = 0xFF;
    bool sittingOnAtv = false, attached = false, rider = false;
    float bodyFromPassenger = -1.f, bodyFromDriver = -1.f;
    int bodyIsOnAtv = -1;
    float eyeOverAtv = 0.f;
    ue_wrap::FVector atvAt{};
};

Seen Look(const std::string& step) {
    Seen v;
    OnGameThread([&] {
        if (!g_atv || !R::IsLive(g_atv)) return;
        coop::atv_sync::TableView t;
        if (coop::atv_sync::ReadTable(g_key, t)) { v.occupant = t.occupantSlot; v.passenger = t.passengerSlot; }
        void* sittingOn = nullptr;
        E::ReadMainPlayerSittingOn(Me(), sittingOn);
        v.sittingOnAtv = sittingOn == g_atv;
        void* parent = nullptr;
        v.attached = E::GetAttachParentActor(Me(), parent) && parent == g_atv;
        v.rider = A::RidingKerfur(g_atv) != nullptr;
        E::TryGetActorLocation(g_atv, v.atvAt);
        v.eyeOverAtv = E::GetCameraLocation().Z - v.atvAt.Z;
        if (void* body = OtherBody()) {
            ue_wrap::FVector at{}, pass{}, drv{};
            ue_wrap::FRotator r{};
            if (E::TryGetActorLocation(body, at)) {
                if (PS::BodyTransform(g_atv, pass, r)) v.bodyFromPassenger = Dist(at, pass);
                if (A::GetSeatTransform(g_atv, drv, r)) v.bodyFromDriver = Dist(at, drv);
            }
            v.bodyIsOnAtv = AnimOnAtv(body);
        }
    });
    UE_LOGI("atv-passenger: STATE %hs %hs occupant=%u passenger=%u mineSitting=%d mineAttached=%d riding=%d "
            "kerfurRider=%d bodyFromPassenger=%.0f bodyFromDriver=%.0f bodyIsOnAtv=%d eyeOverAtv=%.0f atv=(%.0f,%.0f,%.0f)",
            step.c_str(), Who(), v.occupant, v.passenger, v.sittingOnAtv ? 1 : 0, v.attached ? 1 : 0,
            coop::atv_passenger::Riding() ? 1 : 0, v.rider ? 1 : 0, v.bodyFromPassenger, v.bodyFromDriver,
            v.bodyIsOnAtv, v.eyeOverAtv, v.atvAt.X, v.atvAt.Y, v.atvAt.Z);
    return v;
}

void Check(const std::string& step, bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_failed;
    UE_LOGI("atv-passenger: CHECK %hs %hs %hs -- %hs", step.c_str(), Who(), ok ? "PASS" : "FAIL", what);
}

// The other peer's body sits in the passenger spot on this peer.
void CheckBodyRides(const std::string& step, const Seen& v) {
    Check(step, v.bodyFromPassenger >= 0.f && v.bodyFromPassenger < 30.f && v.bodyIsOnAtv == 1,
          "the other player's body sits in the passenger spot, in the ATV seat pose");
}

// This peer 4 m from the ATV on its right, its camera on the passenger spot, then a screenshot.
void Shot(const wchar_t* label) {
    bool seated = false;
    OnGameThread([&] {
        void* on = nullptr;
        seated = E::ReadMainPlayerSittingOn(Me(), on) && on;
    });
    if (seated) {   // a rider is not moved: its own view
        harness::screenshot::Capture(label);
        return;
    }
    OnGameThread([] {
        ue_wrap::FVector at{};
        ue_wrap::FRotator rot{};
        if (!A::GetRootTransform(g_atv, at, rot)) return;
        const float yaw = (rot.Yaw + 70.f) * 0.01745329252f;
        PlaceMe({at.X + std::cos(yaw) * 400.f, at.Y + std::sin(yaw) * 400.f, at.Z + 120.f}, 0.f);
    });
    ::Sleep(1000);
    OnGameThread([] {
        ue_wrap::FVector at{};
        ue_wrap::FRotator r{};
        if (!PS::BodyTransform(g_atv, at, r)) return;
        const ue_wrap::FVector from = E::GetCameraLocation();
        const float dx = at.X - from.X, dy = at.Y - from.Y, dz = at.Z - from.Z;
        const float yaw = std::atan2(dy, dx) * 57.2957795f;
        const float pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 57.2957795f;
        if (void* c = E::GetController(Me())) E::SetControlRotation(c, ue_wrap::FRotator{pitch, yaw, 0.f});
    });
    ::Sleep(800);
    OnGameThread([label] {
        ue_wrap::FVector cam = E::GetCameraLocation(), atv{}, seat{}, body{}, sit{};
        ue_wrap::FRotator r{};
        E::TryGetActorLocation(g_atv, atv);
        PS::BodyTransform(g_atv, seat, r);
        if (void* b = OtherBody()) E::TryGetActorLocation(b, body);
        void* arrow = nullptr;
        Field(g_atv, L"sitarrow", arrow);
        if (arrow) E::TryGetComponentLocation(arrow, sit);
        UE_LOGI("atv-passenger: SHOT %ls cam-atv=(%.0f,%.0f,%.0f) seat-atv=(%.0f,%.0f,%.0f) body-atv=(%.0f,%.0f,%.0f) "
                "sitarrow-atv=(%.0f,%.0f,%.0f) atvYaw=%.0f", label, cam.X - atv.X, cam.Y - atv.Y, cam.Z - atv.Z,
                seat.X - atv.X, seat.Y - atv.Y, seat.Z - atv.Z, body.X - atv.X, body.Y - atv.Y, body.Z - atv.Z,
                sit.X - atv.X, sit.Y - atv.Y, sit.Z - atv.Z, r.Yaw);
    });
    harness::screenshot::Capture(label);
}

// ---- the actions ----

// The player to `at` facing `yaw` through its own teleportWObackrooms, the move the ATV's own dismount makes: a plain
// teleport of a player is a clip the game answers with a trip to the backrooms.
void PlaceMe(const ue_wrap::FVector& at, float yaw) {
    void* f = R::FindFunction(R::ClassOf(Me()), L"teleportWObackrooms");
    ue_wrap::ParamFrame pf(f);
    ue_wrap::FTransform t{};
    const float half = yaw * 0.5f * 0.01745329252f;
    t.RotZ = std::sin(half);
    t.RotW = std::cos(half);
    t.TX = at.X; t.TY = at.Y; t.TZ = at.Z;
    t.SX = t.SY = t.SZ = 1.f;
    const bool ok = f && pf.valid() && pf.Set<ue_wrap::FTransform>(L"newTransform", t) &&
                    pf.Set<bool>(L"useRotation", true) && ue_wrap::Call(Me(), pf);
    if (!ok) E::TeleportTo(Me(), at, ue_wrap::FRotator{0.f, yaw, 0.f});
}

void Settle() {
    OnGameThread([] {
        bool turbo = false;
        Field(g_atv, L"turbo", turbo);
        auto input = [](const wchar_t* fn) {
            void* f = R::FindFunction(R::ClassOf(g_atv), fn);
            ue_wrap::ParamFrame pf(f);
            if (f && pf.valid()) ue_wrap::Call(g_atv, pf);
        };
        if (turbo) input(L"InpActEvt_atv_turbo_K2Node_InputActionEvent_13");
        if (!A::GetBrake(g_atv)) input(L"InpActEvt_atv_brake_K2Node_InputActionEvent_22");
    });
}

void Drive() {
    for (int i = 0; i < 6; ++i) {
        bool driven = false;
        OnGameThread([&] {
            if (!(A::IsDriven(g_atv) && A::GetOccupantPlayer(g_atv) == Me())) {
                std::wstring verb = L"sit";
                R::FString fs{verb.data(), static_cast<int32_t>(verb.size()) + 1, static_cast<int32_t>(verb.size()) + 1};
                void* f = R::FindFunction(R::ClassOf(g_atv), L"actionName");
                ue_wrap::ParamFrame pf(f);
                if (f && pf.valid() && pf.Set<void*>(L"player", Me()) && pf.Set<R::FString>(L"name", fs))
                    ue_wrap::Call(g_atv, pf);
            }
            driven = A::IsDriven(g_atv) && A::GetOccupantPlayer(g_atv) == Me();
        });
        UE_LOGI("atv-passenger: %hs drive try %d -> driven=%d", Who(), i + 1, driven ? 1 : 0);
        if (driven) return;
        ::Sleep(2000);
    }
}

void DriverOff() {
    OnGameThread([] { UE_LOGI("atv-passenger: %hs got off the handlebars (ok=%d)", Who(), A::Dismount(g_atv) ? 1 : 0); });
}

// Beside the ATV on its right, then the passenger lane's ask, as the use key on the ATV makes it.
void Board() {
    OnGameThread([] {
        ue_wrap::FVector at{};
        ue_wrap::FRotator rot{};
        if (!A::GetRootTransform(g_atv, at, rot)) return;
        const float yaw = (rot.Yaw + 90.f) * 0.01745329252f;
        // 120 cm over the ATV's root, so the body drops onto the ground beside it.
        PlaceMe({at.X + std::cos(yaw) * 200.f, at.Y + std::sin(yaw) * 200.f, at.Z + 120.f}, rot.Yaw - 90.f);
    });
    ::Sleep(2000);
    bool took = false;
    OnGameThread([&] { took = coop::atv_passenger::OnUseAtOccupied(g_atv); });
    UE_LOGI("atv-passenger: %hs asked for the passenger spot (taken by the lane=%d)", Who(), took ? 1 : 0);
}

void Unsit() {
    OnGameThread([] { UE_LOGI("atv-passenger: %hs unsit (ok=%d)", Who(), PS::Unsit(Me()) ? 1 : 0); });
}

// The driver's copy pushed forward at 2 m/s for 2 s, as a ride moves it.
void Push() {
    for (int i = 0; i < 20; ++i) {
        OnGameThread([] {
            ue_wrap::FRotator rot{};
            if (!E::TryGetActorRotation(g_atv, rot)) return;
            const float yaw = rot.Yaw * 0.01745329252f;
            E::SetActorRootPhysicsVelocity(g_atv, {std::cos(yaw) * 200.f, std::sin(yaw) * 200.f, 0.f}, {});
        });
        ::Sleep(100);
    }
}

// The passenger's own copy cut 1 m on, as the corrector cuts a mirror that drifted.
void Cut() {
    OnGameThread([] {
        ue_wrap::FVector at{};
        ue_wrap::FRotator rot{};
        if (!A::GetRootTransform(g_atv, at, rot)) return;
        const float yaw = rot.Yaw * 0.01745329252f;
        const bool ok = A::TeleportRig(g_atv, {at.X + std::cos(yaw) * 100.f, at.Y + std::sin(yaw) * 100.f, at.Z}, rot);
        UE_LOGI("atv-passenger: %hs cut its copy 1 m on (ok=%d)", Who(), ok ? 1 : 0);
    });
}

// The ATV's own keys, by their input events: the brake toggles, forward presses (4) and lets go (5).
void AtvKey(const wchar_t* fn) {
    OnGameThread([fn] {
        void* f = R::FindFunction(R::ClassOf(g_atv), fn);
        ue_wrap::ParamFrame pf(f);
        if (f && pf.valid()) ue_wrap::Call(g_atv, pf);
    });
}

void SetBrake(bool on) {
    bool now = false;
    OnGameThread([&] { now = A::GetBrake(g_atv); });
    if (now != on) AtvKey(L"InpActEvt_atv_brake_K2Node_InputActionEvent_22");
}

// The host's hold loop: `forward <seconds>` drives the ATV forward with its own keys that long, the brake off, then
// lets go and sets the brake; `quit` ends the loop. Each line is taken once and the file removed.
void HoldCommands() {
    const std::string path = g_dir + "\\cmd";
    for (;;) {
        std::string line;
        {
            std::ifstream f(path, std::ios::binary);
            if (f) std::getline(f, line);
        }
        if (line.empty()) { ::Sleep(200); continue; }
        std::remove(path.c_str());
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        UE_LOGI("atv-passenger: HOLD command '%hs'", line.c_str());
        if (line == "quit") return;
        if (line.rfind("forward", 0) == 0) {
            const float secs = line.size() > 8 ? static_cast<float>(std::atof(line.c_str() + 8)) : 3.f;
            SetBrake(false);
            AtvKey(L"InpActEvt_atv_forward_K2Node_InputActionEvent_4");
            ::Sleep(static_cast<DWORD>((secs > 0.f && secs < 60.f ? secs : 3.f) * 1000.f));
            AtvKey(L"InpActEvt_atv_forward_K2Node_InputActionEvent_5");
            ::Sleep(1500);
            SetBrake(true);
            Look("hold-forward");
        }
    }
}

void HostDrives() { Settle(); Drive(); }
void ClientBoards() { Board(); ::Sleep(4000); Shot(L"passenger-own-view"); }
void HostPushes() { Push(); }
void HostOff() { DriverOff(); ::Sleep(2000); Shot(L"passenger-client-seen"); }
void ClientUnsits() { Unsit(); }
void ClientDrives() { Settle(); Drive(); ::Sleep(15000); }   // the kerfur's walk to the ATV
void HostBoards() { Board(); }
void ClientPushes() { Push(); }
void HostUnsits() { Unsit(); }
void ClientOffAndLook() { DriverOff(); ::Sleep(2000); Shot(L"passenger-host-seen"); }

// One step: the actor does it, both look and judge, the watcher shoots when named.
using Judge = void (*)(const std::string& step, const Seen& v, bool actor);
void Step(bool hostActs, const char* name, void (*act)(), int waitMs, Judge judge, const wchar_t* shot = nullptr) {
    const std::string done = std::string(name) + (hostActs ? "_host" : "_client");
    const bool mine = hostActs != IsClientRole();
    if (mine) {
        act();
        ::Sleep(waitMs);
        judge(name, Look(name), true);
        Write(done, "x");
        Await(std::string(name) + "_seen");
    } else {
        if (!Await(done)) return;
        ::Sleep(1500);
        judge(name, Look(name), false);
        if (shot) Shot(shot);
        Write(std::string(name) + "_seen", "x");
    }
}

void J1(const std::string& st, const Seen& v, bool actor) {
    Check(st, v.occupant == 0 && v.passenger == 1, "the table: the host drives, the client rides");
    if (actor) Check(st, v.sittingOnAtv && v.attached && coop::atv_passenger::Riding(), "the client sits on the ATV");
    if (actor) Check(st, std::fabs(v.eyeOverAtv - PS::kEyeAboveRootCm) < 10.f, "the client's eye is at head height");
    else CheckBodyRides(st, v);
}
void J2(const std::string& st, const Seen& v, bool) {
    Check(st, v.occupant == 0 && v.passenger == 1, "the table holds through the drive");
    if (IsClientRole()) Check(st, v.sittingOnAtv && v.attached, "the client is still seated after the drive");
    else CheckBodyRides(st, v);
}
void J3(const std::string& st, const Seen& v, bool actor) {
    Check(st, v.occupant == 0xFF && v.passenger == 1, "the driver is off; the passenger keeps the spot");
    if (!actor) Check(st, v.sittingOnAtv, "the client is still seated");
    else CheckBodyRides(st, v);
}
// The player stands within 2 m of the ATV's height: set down beside it, not fallen through the ground.
void CheckStands(const std::string& st) {
    ue_wrap::FVector me{}, atv{};
    OnGameThread([&] { E::TryGetActorLocation(Me(), me); E::TryGetActorLocation(g_atv, atv); });
    char what[96];
    std::snprintf(what, sizeof(what), "set down beside the ATV (%.0f cm away, %.0f cm above it)", Dist(me, atv),
                  me.Z - atv.Z);
    Check(st, Dist(me, atv) < 400.f && std::fabs(me.Z - atv.Z) < 200.f, what);
}

void J4(const std::string& st, const Seen& v, bool actor) {
    Check(st, v.passenger == 0xFF, "the spot is free once the client got off");
    if (actor) CheckStands(st);
    if (actor) Check(st, !v.sittingOnAtv && !v.attached && !coop::atv_passenger::Riding(), "the client stands again");
    else Check(st, v.bodyIsOnAtv == 0, "the client's body stands on the host");
}
void J5(const std::string& st, const Seen& v, bool) {
    Check(st, v.occupant == 1 && v.passenger == 0xFF, "the table: the client drives");
}
void J6(const std::string& st, const Seen& v, bool actor) {
    Check(st, v.occupant == 1 && v.passenger == 0, "the table: the client drives, the host rides");
    Check(st, !v.rider, "no kerfur rides once the host has the spot");
    if (actor) Check(st, v.sittingOnAtv && v.attached, "the host sits on the ATV");
    if (actor) Check(st, std::fabs(v.eyeOverAtv - PS::kEyeAboveRootCm) < 10.f, "the host's eye is at head height");
    else CheckBodyRides(st, v);
}
void J7(const std::string& st, const Seen& v, bool) {
    Check(st, v.occupant == 1 && v.passenger == 0, "the table holds through the client's drive");
    if (!IsClientRole()) Check(st, v.sittingOnAtv && v.attached, "the host is still seated after the drive");
    else CheckBodyRides(st, v);
}
void J8(const std::string& st, const Seen& v, bool actor) {
    Check(st, v.occupant == 0xFF && v.passenger == 0xFF, "the ATV is empty once the host got off");
    if (actor) CheckStands(st);
    if (actor) Check(st, !v.sittingOnAtv && !coop::atv_passenger::Riding(), "the host stands again");
}
void J9b(const std::string& st, const Seen& v, bool actor) {
    Check(st, v.occupant == 0xFF && v.passenger == 0, "the driver is off; the host keeps the spot");
    if (actor) CheckBodyRides(st, v);
}
void J0(const std::string& st, const Seen& v, bool) {
    Check(st, v.occupant == 0 && v.passenger == 0xFF, "the table: the host drives");
}

void RunSteps() {
    Look("0-start");
    Step(true, "1-host-drives", &HostDrives, 3000, &J0);
    Step(false, "2-client-boards", &ClientBoards, 4000, &J1, L"passenger-client-rides");
    if (coop::config::ReadEnv("VOTVCOOP_ATV_PASSENGER_HOLD") == "1") {
        // Held for a watcher: the host at the handlebars and the client behind it, left as they are. The host then
        // takes `forward <seconds>` lines from the folder's `cmd` file and drives on them.
        UE_LOGI("atv-passenger: HOLD %hs -- the riders stay on the ATV for a watcher", Who());
        if (!IsClientRole()) HoldCommands();
        return;
    }
    Step(true, "3-host-drives-on", &HostPushes, 3000, &J2);
    Step(false, "3b-client-cut", &Cut, 3000, &J2);
    Step(true, "4-host-off", &HostOff, 3000, &J3, L"passenger-driver-off");
    Step(false, "5-client-unsits", &ClientUnsits, 3000, &J4);
    Step(false, "6-client-drives", &ClientDrives, 2000, &J5);
    Step(true, "7-host-boards", &HostBoards, 5000, &J6, L"passenger-host-rides");
    Step(false, "8-client-drives-on", &ClientPushes, 3000, &J7);
    Step(true, "8b-host-cut", &Cut, 3000, &J7);
    Step(false, "9-client-off", &ClientOffAndLook, 1000, &J9b);
    Step(true, "10-host-unsits", &HostUnsits, 3000, &J8);
    UE_LOGI("atv-passenger: VERDICT %hs %hs -- %d of %d checks failed", Who(), g_failed == 0 ? "PASS" : "FAIL",
            g_failed, g_checks);
    UE_LOGI("atv-passenger: DONE");
}

void FirstAtv(void* ctx, void* actor, int32_t) {
    void** out = static_cast<void**>(ctx);
    if (!*out) *out = actor;
}

void RunHost() {
    ::Sleep(20000);
    if (!Await("client_ready")) return;
    OnGameThread([] {
        if (!A::EnsureResolved()) return;
        A::ForEachAtv(&FirstAtv, &g_atv);
        if (g_atv) g_key = A::GetKeyString(g_atv);
    });
    if (!g_atv || g_key.empty()) {
        UE_LOGW("atv-passenger: VERDICT ABORT -- no ATV in the host's world");
        return;
    }
    const std::string key(g_key.begin(), g_key.end());
    // The host beside the ATV by its own seat and dismount, then a kerfur 2 m further.
    // Midday, so the shots show the riders.
    OnGameThread([] { coop::dev::set_clock::ApplyTimeFraction(0.5f); });
    Settle();
    Drive();
    DriverOff();
    ::Sleep(2000);
    OnGameThread([] {
        ue_wrap::FVector me{}, atv{};
        if (!E::TryGetActorLocation(Me(), me) || !E::TryGetActorLocation(g_atv, atv)) return;
        float dx = me.X - atv.X, dy = me.Y - atv.Y;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > 1.f) { dx /= d; dy /= d; } else { dx = 1.f; dy = 0.f; }
        void* k = FirstKerfur() ? FirstKerfur()
                                : coop::dev::spawn_npc::SpawnAt(L"kerfurOmega_C", {me.X + dx * 200.f, me.Y + dy * 200.f,
                                                                                   me.Z + 20.f});
        UE_LOGI("atv-passenger: HOST kerfur %p beside the ATV", k);
    });
    ::Sleep(6000);
    Write("atv_key", key);
    if (!Await("client_found")) return;
    RunSteps();
}

struct Find {
    std::wstring key;
    void* actor = nullptr;
};

void ByKey(void* ctx, void* actor, int32_t) {
    Find* f = static_cast<Find*>(ctx);
    if (!f->actor && A::GetKeyString(actor) == f->key) f->actor = actor;
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(10000);
    Write("client_ready", "x");
    std::string key;
    if (!Await("atv_key", &key)) return;
    Find find{std::wstring(key.begin(), key.end())};
    for (int i = 0; i < 300 && !find.actor; ++i) {
        OnGameThread([&] {
            if (A::EnsureResolved()) A::ForEachAtv(&ByKey, &find);
        });
        if (!find.actor) ::Sleep(100);
    }
    g_atv = find.actor;
    g_key = find.key;
    if (!g_atv) {
        UE_LOGW("atv-passenger: VERDICT ABORT -- the client never had the ATV keyed '%hs'", key.c_str());
        return;
    }
    ::Sleep(2000);
    Write("client_found", "x");
    RunSteps();
}

// The view sweep (VOTVCOOP_ATV_PASSENGER_MODE=view, the host alone, midday): the host takes the passenger seat at each
// of a row of places and shoots its own view; `atv-passenger: VIEW` logs each place and the eye in the ATV's frame.
void RunViewSweep() {
    ::Sleep(25000);
    OnGameThread([] {
        coop::dev::set_clock::ApplyTimeFraction(0.5f);
        if (A::EnsureResolved()) A::ForEachAtv(&FirstAtv, &g_atv);
    });
    if (!g_atv) { UE_LOGW("atv-passenger: VIEW no ATV"); return; }
    Settle();
    const float places[][2] = {{-60.f, 160.f}, {-60.f, 170.f}, {-80.f, 160.f}};
    for (const auto& pl : places) {
        OnGameThread([] {
            ue_wrap::FVector at{};
            ue_wrap::FRotator rot{};
            if (!A::GetRootTransform(g_atv, at, rot)) return;
            const float yaw = (rot.Yaw + 90.f) * 0.01745329252f;
            E::TeleportTo(Me(), {at.X + std::cos(yaw) * 200.f, at.Y + std::sin(yaw) * 200.f, at.Z + 60.f},
                          ue_wrap::FRotator{0.f, rot.Yaw - 90.f, 0.f});
        });
        ::Sleep(1500);
        void* box = nullptr;
        bool sat = false;
        OnGameThread([&] {
            PS::SetSeatOffsetsForDrill(pl[0], pl[1]);
            box = PS::SpawnSitBox(g_atv);
            if (box) PS::Sit(Me(), g_atv, box);
            void* on = nullptr;
            sat = E::ReadMainPlayerSittingOn(Me(), on) && on == g_atv;
            if (sat) E::SetActorRootCollisionEnabled(Me(), 1);
        });
        ::Sleep(4000);
        OnGameThread([&] {
            ue_wrap::FVector atv{}, cam = E::GetCameraLocation();
            ue_wrap::FRotator rot{};
            E::TryGetActorLocation(g_atv, atv);
            E::TryGetActorRotation(g_atv, rot);
            const float c = std::cos(-rot.Yaw * 0.01745329252f), sn = std::sin(-rot.Yaw * 0.01745329252f);
            const float dx = cam.X - atv.X, dy = cam.Y - atv.Y;
            UE_LOGI("atv-passenger: VIEW ahead=%.0f up=%.0f sat=%d eye=(%.0f,%.0f,%.0f)", pl[0], pl[1], sat ? 1 : 0,
                    dx * c - dy * sn, dx * sn + dy * c, cam.Z - atv.Z);
        });
        wchar_t label[64];
        std::swprintf(label, 64, L"view-ahead%d-up%d", static_cast<int>(pl[0]), static_cast<int>(pl[1]));
        harness::screenshot::Capture(label);
        OnGameThread([&] {
            PS::Unsit(Me());
            E::SetActorRootCollisionEnabled(Me(), 3);
            if (box) E::DestroyActor(box);
        });
        ::Sleep(1500);
    }
    UE_LOGI("atv-passenger: DONE");
}

// A third peer's watch (VOTVCOOP_ATV_PASSENGER_WATCH=1): the ATV the host names, sampled every 50 ms; each second
// with the ATV moving logs `atv-passenger: WATCH` with the most the driver's body (slot 0) and the passenger's (slot 1)
// stood off their places in that second.
void RunWatcher() {
    bool ready = false;
    for (int i = 0; i < 960 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    std::string key;
    if (!Await("atv_key", &key, 600)) return;
    UE_LOGI("atv-passenger: WATCH started on '%hs'", key.c_str());
    ue_wrap::FVector last{};
    bool haveLast = false;
    for (;;) {
        Find find{std::wstring(key.begin(), key.end())};   // the actor each second: a join may replace it
        OnGameThread([&] { if (A::EnsureResolved()) A::ForEachAtv(&ByKey, &find); });
        if (find.actor != g_atv) haveLast = false;
        g_atv = find.actor;
        float worstDrv = 0.f, worstPass = 0.f, moved = 0.f;
        for (int i = 0; i < 20; ++i) {
            OnGameThread([&] {
                if (!g_atv || !R::IsLive(g_atv)) return;
                ue_wrap::FVector at{}, seat{}, body{};
                ue_wrap::FRotator r{};
                E::TryGetActorLocation(g_atv, at);
                if (haveLast) moved += Dist(at, last);
                last = at;
                haveLast = true;
                auto& reg = coop::players::Registry::Get();
                coop::RemotePlayer* d = reg.Puppet(0);
                coop::RemotePlayer* p = reg.Puppet(1);
                if (d && d->valid() && E::TryGetActorLocation(d->GetActor(), body) && A::GetSeatTransform(g_atv, seat, r))
                    worstDrv = (std::max)(worstDrv, Dist(body, seat));
                if (p && p->valid() && E::TryGetActorLocation(p->GetActor(), body) && PS::BodyTransform(g_atv, seat, r))
                    worstPass = (std::max)(worstPass, Dist(body, seat));
            });
            ::Sleep(50);
        }
        if (moved > 20.f)
            UE_LOGI("atv-passenger: WATCH moved=%.0fcm/s driverOff=%.0f passengerOff=%.0f", moved, worstDrv, worstPass);
    }
}

}  // namespace

DWORD WINAPI AtvPassengerThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("atv-passenger: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (coop::config::ReadEnv("VOTVCOOP_ATV_PASSENGER_MODE") == "view") { if (!IsClientRole()) RunViewSweep(); }
    else if (coop::config::ReadEnv("VOTVCOOP_ATV_PASSENGER_WATCH") == "1") RunWatcher();
    else if (IsClientRole()) RunClient();
    else                     RunHost();
    return 0;
}

}  // namespace harness::autotest
