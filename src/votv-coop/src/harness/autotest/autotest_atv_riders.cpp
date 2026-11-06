// harness/autotest/autotest_atv_riders.cpp -- TEST-ONLY: who rides an ATV, as each of two peers sees it.
//
// The host gets off the world's ATV beside it and puts a kerfur next to it. The client sits in the ATV (its own `sit`
// verb), moves it 8 m with the ATV's own teleportVehicle and gets off; then the host sits and gets off. After each step
// each peer logs `atv-riders: STATE <step>`: the seat and the ATV's own `sittingKerfuro`, the kerfur this peer holds
// (attached to the ATV, its distance from it, its command), and the other peer's body (its distance from the driver's
// seat, 100 cm ahead of the ATV's `Camera` and 25 cm above `sitarrow`, and its AnimBP's `isOnAtv`); the seated steps also take a screenshot of the ATV. Reads only
// reflection, so the same drill runs on a build without the riders lane. Gated by VOTVCOOP_RUN_ATV_RIDERS; the folder
// is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/dev/spawn_npc.h"
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
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace A = ue_wrap::atv;
namespace P = ue_wrap::profile;

std::string g_dir;
void* g_atv = nullptr;   // game thread only

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

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
    UE_LOGW("atv-riders: VERDICT ABORT -- no '%hs' from the other peer", name.c_str());
    return false;
}

template <class T>
bool Field(void* obj, const wchar_t* name, T& out) {
    const int32_t off = obj && R::IsLive(obj) ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(obj) + off, sizeof(T));
    return true;
}

void* Obj(void* obj, const wchar_t* name) {
    void* o = nullptr;
    Field(obj, name, o);
    return o && R::IsLive(o) ? o : nullptr;
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void* Me() { return coop::players::Registry::Get().Local(); }

// The other peer's body on this peer.
void* OtherBody() {
    auto& reg = coop::players::Registry::Get();
    for (uint8_t slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        if (slot == reg.LocalPeerId()) continue;
        coop::RemotePlayer* p = reg.Puppet(slot);
        if (p && p->valid() && p->GetActor()) return p->GetActor();
    }
    return nullptr;
}

void* FirstKerfur() {
    for (void* o : R::FindObjectsByClass(L"kerfurOmega_C"))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) return o;
    return nullptr;
}

// The puppet's AnimBP's isOnAtv: 1, 0, or -1 when it does not read.
int AnimOnAtv(void* body) {
    void* comp = body ? ue_wrap::puppet::GetSkeletalMeshComponent(body) : nullptr;
    if (!comp || !R::IsLive(comp)) return -1;
    void* anim = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(comp) +
                                                 P::off::USkeletalMesh_AnimScriptInstance);
    bool on = false;
    return anim && Field(anim, L"isOnAtv", on) ? (on ? 1 : 0) : -1;
}

void LogState(const std::string& step) {
    OnGameThread([&] {
        if (!g_atv || !R::IsLive(g_atv)) {
            UE_LOGW("atv-riders: STATE %hs -- no ATV", step.c_str());
            return;
        }
        ue_wrap::FVector atvAt{}, seatAt{};
        E::TryGetActorLocation(g_atv, atvAt);
        // The driver's seat: under the driver's camera, at the height a riding kerfur's pivot takes.
        void* eye = Obj(g_atv, L"Camera");
        void* sit = Obj(g_atv, L"sitarrow");
        ue_wrap::FVector eyeAt{};
        const bool haveSeat = eye && sit && E::TryGetComponentLocation(eye, eyeAt) && E::TryGetComponentLocation(sit, seatAt);
        // 100 cm ahead of the camera along the ATV's facing: behind the handlebars.
        ue_wrap::FRotator sitRot{};
        if (sit) E::TryGetComponentWorldRotation(sit, sitRot);
        const float yawR = sitRot.Yaw * 0.01745329252f, pitchR = sitRot.Pitch * 0.01745329252f;
        seatAt.X = eyeAt.X + std::cos(pitchR) * std::cos(yawR) * 100.f;
        seatAt.Y = eyeAt.Y + std::cos(pitchR) * std::sin(yawR) * 100.f;
        seatAt.Z += std::sin(pitchR) * 100.f + 25.f;   // on the seat
        void* rider = Obj(g_atv, L"sittingKerfuro");
        void* kerfur = FirstKerfur();
        void* parent = nullptr;
        int attached = -1;
        float kDist = -1.f;
        uint8_t kState = 0xFF;
        if (kerfur) {
            attached = E::GetAttachParentActor(kerfur, parent) && parent == g_atv ? 1 : 0;
            ue_wrap::FVector kAt{};
            if (E::TryGetActorLocation(kerfur, kAt)) kDist = Dist(kAt, atvAt);
            Field(kerfur, L"State", kState);
        }
        void* body = OtherBody();
        float bDist = -1.f;
        ue_wrap::FVector bAt{};
        if (body && haveSeat && E::TryGetActorLocation(body, bAt)) bDist = Dist(bAt, seatAt);
        UE_LOGI("atv-riders: STATE %hs driven=%d mine=%d rider=%d kerfur=%d kerfurOnAtv=%d kerfurFromAtv=%.0f "
                "kerfurState=%u body=%d bodyFromSeat=%.0f bodyIsOnAtv=%d", step.c_str(), A::IsDriven(g_atv) ? 1 : 0,
                A::GetOccupantPlayer(g_atv) == Me() ? 1 : 0, rider ? 1 : 0, kerfur ? 1 : 0, attached, kDist,
                static_cast<unsigned>(kState), body ? 1 : 0, bDist, AnimOnAtv(body));
        // The seat's parts, each as its offset from the ATV's root and its world rotation, and the body's.
        ue_wrap::FRotator atvRot{};
        E::TryGetActorRotation(g_atv, atvRot);
        std::string parts;
        for (const wchar_t* name : {L"playerHit", L"sitarrow", L"Camera"}) {
            void* c = Obj(g_atv, name);
            ue_wrap::FVector l{};
            ue_wrap::FRotator r{};
            if (!c || !E::TryGetComponentLocation(c, l) || !E::TryGetComponentWorldRotation(c, r)) continue;
            char buf[160];
            std::snprintf(buf, sizeof(buf), " %ls=(%.0f,%.0f,%.0f|p%.0f y%.0f r%.0f)", name, l.X - atvAt.X, l.Y - atvAt.Y,
                          l.Z - atvAt.Z, r.Pitch, r.Yaw, r.Roll);
            parts += buf;
        }
        ue_wrap::FRotator bRot{};
        if (body) E::TryGetActorRotation(body, bRot);
        UE_LOGI("atv-riders: PARTS %hs atv=(p%.0f y%.0f r%.0f)%hs body=(%.0f,%.0f,%.0f|p%.0f y%.0f r%.0f)", step.c_str(),
                atvRot.Pitch, atvRot.Yaw, atvRot.Roll, parts.c_str(), bAt.X - atvAt.X, bAt.Y - atvAt.Y, bAt.Z - atvAt.Z,
                bRot.Pitch, bRot.Yaw, bRot.Roll);
    });
}

// This peer 4 m back from the ATV on its own side, its camera on the driver's seat, then a screenshot.
void Shot(const wchar_t* label) {
    OnGameThread([] {
        ue_wrap::FVector atv{}, me{}, at{};
        void* seat = Obj(g_atv, L"playerHit");
        if (!seat || !E::TryGetComponentLocation(seat, at) || !E::TryGetActorLocation(g_atv, atv) ||
            !E::TryGetActorLocation(Me(), me))
            return;
        float sx = me.X - atv.X, sy = me.Y - atv.Y;
        const float d = std::sqrt(sx * sx + sy * sy);
        if (d > 1.f) { sx /= d; sy /= d; } else { sx = 1.f; sy = 0.f; }
        E::TeleportTo(Me(), {atv.X + sx * 400.f, atv.Y + sy * 400.f, (std::max)(me.Z, atv.Z + 60.f)}, ue_wrap::FRotator{});
    });
    ::Sleep(1000);
    OnGameThread([] {
        ue_wrap::FVector at{};
        void* seat = Obj(g_atv, L"playerHit");
        if (!seat || !E::TryGetComponentLocation(seat, at)) return;
        const ue_wrap::FVector from = E::GetCameraLocation();
        const float dx = at.X - from.X, dy = at.Y - from.Y, dz = at.Z - from.Z;
        const float yaw = std::atan2(dy, dx) * 57.2957795f;
        const float pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 57.2957795f;
        if (void* c = E::GetController(Me())) E::SetControlRotation(c, ue_wrap::FRotator{pitch, yaw, 0.f});
    });
    ::Sleep(800);
    harness::screenshot::Capture(label);
}

// ---- the actions ----

void Sit() {
    for (int i = 0; i < 6; ++i) {
        bool driven = false;
        OnGameThread([&] {
            if (A::IsDriven(g_atv) && A::GetOccupantPlayer(g_atv) == Me()) {
                driven = true;
                return;
            }
            std::wstring verb = L"sit";
            R::FString fs{verb.data(), static_cast<int32_t>(verb.size()) + 1, static_cast<int32_t>(verb.size()) + 1};
            void* f = R::FindFunction(R::ClassOf(g_atv), L"actionName");
            ue_wrap::ParamFrame pf(f);
            if (f && pf.valid() && pf.Set<void*>(L"player", Me()) && pf.Set<R::FString>(L"name", fs)) ue_wrap::Call(g_atv, pf);
            driven = A::IsDriven(g_atv) && A::GetOccupantPlayer(g_atv) == Me();
        });
        UE_LOGI("atv-riders: %hs sit try %d -> driven=%d", IsClientRole() ? "CLIENT" : "HOST", i + 1, driven ? 1 : 0);
        if (driven) return;
        ::Sleep(2000);
    }
}

void Off() {
    OnGameThread([] {
        const bool ok = A::Dismount(g_atv);
        UE_LOGI("atv-riders: %hs got off (ok=%d)", IsClientRole() ? "CLIENT" : "HOST", ok ? 1 : 0);
    });
}

// The brake on and the turbo off, through the ATV's own keys, so a seat on it does not roll away.
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

void ClientSits() {
    Settle();
    Sit();
    ::Sleep(15000);   // the kerfur's walk to the ATV
}

void ClientMoves() {
    OnGameThread([] {
        ue_wrap::FVector at{};
        ue_wrap::FRotator rot{};
        const bool ok = A::GetRootTransform(g_atv, at, rot) &&
                        A::TeleportRig(g_atv, {at.X + 800.f, at.Y, at.Z + 30.f}, ue_wrap::FRotator{0.f, rot.Yaw, 0.f});
        UE_LOGI("atv-riders: CLIENT moved its ATV 8 m (ok=%d)", ok ? 1 : 0);
    });
}

void HostSits() {
    Settle();
    Sit();
    ::Sleep(15000);
}

// One step: the actor does it, then both log; the other waits for the actor's mark and logs after it. A shot, when
// named, is the watcher's.
void Step(bool hostActs, const char* name, void (*act)(), int waitMs, const wchar_t* shot = nullptr) {
    const std::string done = std::string(name) + (hostActs ? "_host" : "_client");
    const bool mine = hostActs != IsClientRole();
    if (mine) {
        act();
        ::Sleep(waitMs);
        LogState(name);
        Write(done, "x");
        Await(std::string(name) + "_seen");
    } else {
        if (!Await(done)) return;
        ::Sleep(1500);
        LogState(name);
        if (shot) Shot(shot);
        Write(std::string(name) + "_seen", "x");
    }
}

void RunSteps() {
    LogState("0-start");
    Step(false, "1-client-seated", &ClientSits, 2000, L"riders-client-seated");
    Step(false, "2-client-moved", &ClientMoves, 4000, L"riders-client-moved");
    Step(false, "3-client-off", &Off, 4000);
    Step(true, "4-host-seated", &HostSits, 2000, L"riders-host-seated");
    Step(true, "5-host-off", &Off, 4000);
    UE_LOGI("atv-riders: DONE");
}

void FirstAtv(void* ctx, void* actor, int32_t) {
    void** out = static_cast<void**>(ctx);
    if (!*out) *out = actor;
}

void RunHost() {
    ::Sleep(20000);
    if (!Await("client_ready")) return;
    std::string key;
    OnGameThread([&] {
        if (!A::EnsureResolved()) return;
        A::ForEachAtv(&FirstAtv, &g_atv);
        if (g_atv) {
            const std::wstring k = A::GetKeyString(g_atv);
            key.assign(k.begin(), k.end());
        }
    });
    if (!g_atv || key.empty()) {
        UE_LOGW("atv-riders: VERDICT ABORT -- no ATV in the host's world");
        return;
    }
    // The host gets beside the ATV by its own seat and dismount, then a kerfur stands 2 m further from it.
    Settle();
    Sit();
    Off();
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
        UE_LOGI("atv-riders: HOST kerfur %p beside the ATV", k);
    });
    ::Sleep(6000);
    UE_LOGI("atv-riders: HOST ATV key=%hs", key.c_str());
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
    if (!g_atv) {
        UE_LOGW("atv-riders: VERDICT ABORT -- the client never had the ATV keyed '%hs'", key.c_str());
        return;
    }
    ::Sleep(2000);
    Write("client_found", "x");
    RunSteps();
}

}  // namespace

DWORD WINAPI AtvRidersThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("atv-riders: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (IsClientRole()) RunClient();
    else                RunHost();
    return 0;
}

}  // namespace harness::autotest
