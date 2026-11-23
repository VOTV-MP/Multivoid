// coop/world/wanderer_sync.cpp -- see coop/world/wanderer_sync.h.
//
// 0.9.0n. log_C: its tick rolls `weight` and its `check` timer finds a pawn in its sphere; either runs vanish(true),
// whose timeline ends in Teleport (a random traced point) and vanish(false). teleBooth_C: its Timer unhides it, runs
// tp and, at 10 % and while not rendered, sets its place and turn. gost_C: its Teleport timer sets its place at a
// random traced point, and its SetVisibility timer hides it at 50 %. goat_C: its tick, past day 6 and while `cantp`,
// runs lookPlace, which at 10 % and while not rendered sets its `goatstand` at a random grass point and hides its
// `goat` (visibility and collision off).

#include "coop/world/wanderer_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_instances.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace coop::wanderer_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace E  = ue_wrap::engine;

UE_FACT_FN(kRowLogTick, "wanderer_sync", L"log_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowLogCheck, "wanderer_sync", L"log_C", L"check", Declared);
UE_FACT_FN(kRowLogTeleport, "wanderer_sync", L"log_C", L"Teleport", Declared);
UE_FACT_FN(kRowLogVanish, "wanderer_sync", L"log_C", L"vanish", Declared);
UE_FACT_FN(kRowBoothTimer, "wanderer_sync", L"teleBooth_C", L"Timer", Declared);
UE_FACT_FN(kRowGhostTeleport, "wanderer_sync", L"gost_C", L"Teleport", Declared);
UE_FACT_FN(kRowGhostHide, "wanderer_sync", L"gost_C", L"SetVisibility", Declared);
UE_FACT_FN(kRowGoatLook, "wanderer_sync", L"goat_C", L"lookPlace", Declared);

constexpr int kTagRefuse = 0x574E4452;  // 'WNDR'
constexpr int kTagVanish = 0x574E4456;  // 'WNDV'
constexpr uint64_t kCheckMs = 500;

// The moved part: the actor itself, or (the goat) its stand; `shown` is a part whose visibility is carried.
struct Wanderer {
    const wchar_t* cls;
    const wchar_t* moved;   // null: the actor
    const wchar_t* shown;   // null: none
    bool hidden;            // the actor's own hidden flag is carried
};
constexpr Wanderer kWanderers[] = {
    {L"log_C", nullptr, nullptr, false},
    {L"teleBooth_C", nullptr, nullptr, true},
    {L"gost_C", nullptr, nullptr, true},
    {L"goat_C", L"goatstand", L"goat", false},
};
constexpr int kCount = static_cast<int>(sizeof(kWanderers) / sizeof(kWanderers[0]));

struct State {
    ue_wrap::FVector at{};
    ue_wrap::FRotator turn{};
    int flags = 0;
};

std::atomic<N::Session*> g_session{nullptr};
bool g_watched = false;
uint64_t g_nextCheck = 0;
State g_sent[kCount];
bool g_sentOnce[kCount] = {};
int g_which = -1;          // CLIENT: the entity 127 named
int g_flags = 0;
ue_wrap::FRotator g_turn{};

bool IsClient() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    return s && s->role() == N::Role::Client;
}

void* Field(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return nullptr;
    void* v = nullptr;
    std::memcpy(&v, static_cast<const uint8_t*>(obj) + off, sizeof(v));
    return v && R::IsLive(v) ? v : nullptr;
}

int Bit(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return -1;
    return (static_cast<const uint8_t*>(obj)[off] & mask) ? 1 : 0;
}

void* One(int i) {
    void* one[1];
    return ue_wrap::world_instances::Find(kWanderers[i].cls, one, 1) > 0 && one[0] && R::IsLive(one[0]) ? one[0]
                                                                                                       : nullptr;
}

bool Read(int i, void* a, State& out) {
    const Wanderer& w = kWanderers[i];
    if (w.moved) {
        void* const part = Field(a, w.moved);
        if (!part || !E::TryGetComponentLocation(part, out.at)) return false;
        out.turn = E::GetComponentWorldRotation(part);
    } else {
        if (!E::TryGetActorLocation(a, out.at)) return false;
        E::TryGetActorRotation(a, out.turn);
    }
    out.flags = 0;
    if (w.hidden && Bit(a, L"bHidden") == 1) out.flags |= 1;
    if (w.shown && Bit(Field(a, w.shown), L"bVisible") == 1) out.flags |= 2;
    return true;
}

bool Same(const State& a, const State& b) {
    const float dx = a.at.X - b.at.X, dy = a.at.Y - b.at.Y, dz = a.at.Z - b.at.Z;
    return dx * dx + dy * dy + dz * dz < 25.f && std::fabs(a.turn.Yaw - b.turn.Yaw) < 1.f &&
           std::fabs(a.turn.Pitch - b.turn.Pitch) < 1.f && std::fabs(a.turn.Roll - b.turn.Roll) < 1.f &&
           a.flags == b.flags;
}

// To every client (`slot` -1) or to one.
void Send(N::Session& s, uint32_t cue, float x, float y, float z, int slot = -1) {
    N::EventCuePayload p{};
    p.cueId = cue;
    p.x = x;
    p.y = y;
    p.z = z;
    if (slot < 0) s.SendReliable(N::ReliableKind::EventCue, &p, sizeof(p));
    else s.SendReliableToSlot(slot, N::ReliableKind::EventCue, &p, sizeof(p));
}

void SendState(N::Session& s, int i, const State& now, int slot) {
    Send(s, kCueWhich, static_cast<float>(i), static_cast<float>(now.flags), 0.f, slot);
    Send(s, kCueTurn, now.turn.Pitch, now.turn.Yaw, now.turn.Roll, slot);
    Send(s, kCuePlace, now.at.X, now.at.Y, now.at.Z, slot);
}

// CLIENT: the rolls run on the host alone.
sg::Verdict OnRefusePre(const sg::Call& call) {
    return call.tag == kTagRefuse && IsClient() ? sg::Verdict::Cancel : sg::Verdict::Run;
}

// HOST: the log's vanish, sent as it starts.
sg::Verdict OnVanishPre(const sg::Call& call) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (call.tag != kTagVanish || !s || s->role() != N::Role::Host || !s->connected() || !call.locals)
        return sg::Verdict::Run;
    const int32_t off = R::FindParamOffset(call.function, L"vanish");
    const bool out = off >= 0 && call.locals[off] != 0;
    Send(*s, kCueVanish, out ? 1.f : 0.f, 0.f, 0.f);
    UE_LOGI("wanderer: the host's ghost log %s", out ? "vanishes" : "comes back");
    return sg::Verdict::Run;
}

void Watch() {
    if (g_watched) return;
    const ue_wrap::facts::Function* const refuse[] = {&kRowLogTick, &kRowLogCheck, &kRowLogTeleport, &kRowBoothTimer,
                                             &kRowGhostTeleport, &kRowGhostHide, &kRowGoatLook};
    bool all = true;
    for (const ue_wrap::facts::Function* row : refuse) all = sg::WatchName(*row, kTagRefuse, &OnRefusePre, nullptr) && all;
    all = sg::WatchName(kRowLogVanish, kTagVanish, &OnVanishPre, nullptr) && all;
    if (all) {
        g_watched = true;
        UE_LOGI("wanderer: the ghost log's, booth's, sheet ghost's and William's rolls are watched");
    }
}

void HostSend(N::Session& s) {
    for (int i = 0; i < kCount; ++i) {
        void* const a = One(i);
        State now;
        if (!a || !Read(i, a, now)) continue;
        if (g_sentOnce[i] && Same(now, g_sent[i])) continue;
        SendState(s, i, now, -1);
        if (!g_sentOnce[i] || !Same(now, g_sent[i]))
            UE_LOGI("wanderer: the host's %ls stands at (%.0f,%.0f,%.0f) flags=%d -- sent", kWanderers[i].cls, now.at.X,
                    now.at.Y, now.at.Z, now.flags);
        g_sent[i] = now;
        g_sentOnce[i] = true;
    }
}

bool SetPartWorld(void* part, const ue_wrap::FVector& at, const ue_wrap::FRotator& turn) {
    void* const f = R::FindDispatchFunction(R::ClassOf(part), L"K2_SetWorldLocationAndRotation", nullptr);
    ue_wrap::ParamFrame pf(f);
    if (!pf.valid()) return false;
    const uint8_t no = 0, yes = 1;
    pf.Set(L"NewLocation", at);
    pf.Set(L"NewRotation", turn);
    pf.Set(L"bSweep", no);
    pf.Set(L"bTeleport", yes);
    return ue_wrap::Call(part, pf);
}

void Apply(int i, const ue_wrap::FVector& at) {
    void* const a = One(i);
    if (!a) return;
    const Wanderer& w = kWanderers[i];
    if (w.moved) {
        if (void* part = Field(a, w.moved)) SetPartWorld(part, at, g_turn);
    } else {
        E::SetActorLocation(a, at);
        E::SetActorRotation(a, g_turn);
    }
    if (w.hidden) E::SetActorHiddenInGame(a, (g_flags & 1) != 0);
    if (w.shown) {
        void* const part = Field(a, w.shown);
        const bool shown = (g_flags & 2) != 0;
        if (part && Bit(part, L"bVisible") != (shown ? 1 : 0)) {
            E::SetComponentVisible(part, shown, false);
            E::SetComponentCollisionEnabled(part, shown ? 3 : 0);
        }
    }
    static bool s_said[kCount] = {};
    if (!s_said[i]) {
        s_said[i] = true;
        UE_LOGI("wanderer: the host's %ls stands at (%.0f,%.0f,%.0f) flags=%d, and this world's too", w.cls, at.X,
                at.Y, at.Z, g_flags);
    }
}

void PlayVanish(bool out) {
    void* const a = One(0);
    void* const f = a ? R::FindDispatchFunction(R::ClassOf(a), L"vanish", nullptr) : nullptr;
    ue_wrap::ParamFrame pf(f);
    const uint8_t v = out ? 1 : 0;
    if (pf.valid() && pf.Set(L"vanish", v)) ue_wrap::Call(a, pf);
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    Watch();
    if (s->role() != N::Role::Host || !s->connected()) return;
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextCheck) return;
    g_nextCheck = now + kCheckMs;
    HostSend(*s);
}

void SendJoin(int slot) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != N::Role::Host) return;
    int sent = 0;
    for (int i = 0; i < kCount; ++i) {
        void* const a = One(i);
        State now;
        if (!a || !Read(i, a, now)) continue;
        SendState(*s, i, now, slot);
        ++sent;
    }
    UE_LOGI("wanderer: slot %d's world is ready -- %d wanderer(s)' places sent to it", slot, sent);
}

bool OnCue(const N::EventCuePayload& p) {
    if (p.cueId < kCueWhich || p.cueId > kCueVanish) return false;
    if (!IsClient()) return true;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return true;
    switch (p.cueId) {
        case kCueWhich: {
            const int i = static_cast<int>(p.x);
            g_which = i >= 0 && i < kCount ? i : -1;
            g_flags = static_cast<int>(p.y);
            break;
        }
        case kCueTurn: g_turn = ue_wrap::FRotator{p.x, p.y, p.z}; break;
        case kCuePlace:
            if (g_which >= 0) Apply(g_which, ue_wrap::FVector{p.x, p.y, p.z});
            g_which = -1;
            break;
        default: PlayVanish(p.x != 0.f); break;
    }
    return true;
}

void OnDisconnect() {
    g_nextCheck = 0;
    for (bool& b : g_sentOnce) b = false;
    g_which = -1;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::wanderer_sync
