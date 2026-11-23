// coop/world/sirenpost_sync.cpp -- see coop/world/sirenpost_sync.h.
//
// 0.9.0n. sirenpost_C::ReceiveTick returns while it WasRecentlyRendered(60) or the time dilation is above 1; else it
// traces down to stand on the ground, turns its `tangent` by a random ease, steps along it by up to 100 a second,
// traces ahead and, at a face it meets, turns its `normal` off it. Its begin play turns its tangent at random and ends
// it outside the story's and the infinite mode.

#include "coop/world/sirenpost_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_instances.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace coop::sirenpost_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace E  = ue_wrap::engine;

constexpr const wchar_t* kClass = L"sirenpost_C";
UE_FACT_FN(kRowTick, "sirenpost_sync", L"sirenpost_C", L"ReceiveTick", Declared);
constexpr int kTag = 0x53505354;   // 'SPST'
constexpr uint64_t kSendMs = 500;

std::atomic<N::Session*> g_session{nullptr};
bool g_watched = false;
uint64_t g_nextSend = 0;
ue_wrap::FVector g_sentAt{};
bool g_sentOnce = false;
ue_wrap::FRotator g_turn{};   // CLIENT: the normal's turn, which 126 brings before the place

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

void* Post() {
    void* one[1];
    return ue_wrap::world_instances::Find(kClass, one, 1) > 0 && one[0] && R::IsLive(one[0]) ? one[0] : nullptr;
}

// CLIENT: the post walks on the host alone.
sg::Verdict OnTickPre(const sg::Call& call) {
    return call.tag == kTag && IsClient() ? sg::Verdict::Cancel : sg::Verdict::Run;
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

// The post's turn, then its place; false when it stands nowhere.
bool SendPlace(N::Session& s, void* post, const ue_wrap::FVector& at, int slot) {
    void* const normal = Field(post, L"normal");
    const ue_wrap::FRotator turn = normal ? E::GetComponentWorldRotation(normal) : ue_wrap::FRotator{};
    Send(s, kCueSirenPostTurn, turn.Pitch, turn.Yaw, turn.Roll, slot);
    Send(s, kCueSirenPostAt, at.X, at.Y, at.Z, slot);
    return true;
}

void SendPost(N::Session& s) {
    void* const post = Post();
    ue_wrap::FVector at{};
    if (!post || !E::TryGetActorLocation(post, at)) return;
    const float dx = at.X - g_sentAt.X, dy = at.Y - g_sentAt.Y, dz = at.Z - g_sentAt.Z;
    if (g_sentOnce && dx * dx + dy * dy + dz * dz < 25.f) return;   // within 5 cm: unmoved
    SendPlace(s, post, at, -1);
    if (!g_sentOnce) UE_LOGI("sirenpost: the host's siren post stands at (%.0f,%.0f,%.0f) -- sent as it walks", at.X,
                             at.Y, at.Z);
    g_sentAt = at;
    g_sentOnce = true;
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!g_watched && (g_watched = sg::WatchName(kRowTick, kTag, &OnTickPre, nullptr)))
        UE_LOGI("sirenpost: the siren post's tick is watched");
    if (s->role() != N::Role::Host || !s->connected()) return;
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextSend) return;
    g_nextSend = now + kSendMs;
    SendPost(*s);
}

void SendJoin(int slot) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    void* const post = Post();
    ue_wrap::FVector at{};
    if (!s || s->role() != N::Role::Host || !post || !E::TryGetActorLocation(post, at)) return;
    SendPlace(*s, post, at, slot);
    UE_LOGI("sirenpost: slot %d's world is ready -- the post's place (%.0f,%.0f,%.0f) sent to it", slot, at.X, at.Y, at.Z);
}

bool OnCue(const N::EventCuePayload& p) {
    if (p.cueId != kCueSirenPostAt && p.cueId != kCueSirenPostTurn) return false;
    if (!IsClient()) return true;
    if (p.cueId == kCueSirenPostTurn) {
        g_turn = ue_wrap::FRotator{p.x, p.y, p.z};
        return true;
    }
    void* const post = Post();
    if (!post) return true;
    static bool s_said = false;
    E::SetActorLocation(post, ue_wrap::FVector{p.x, p.y, p.z});
    if (void* normal = Field(post, L"normal")) E::SetComponentWorldRotation(normal, g_turn);
    if (!s_said) {
        s_said = true;
        UE_LOGI("sirenpost: the host's siren post stands at (%.0f,%.0f,%.0f), and this world's too", p.x, p.y, p.z);
    }
    return true;
}

void OnDisconnect() {
    g_nextSend = 0;
    g_sentOnce = false;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::sirenpost_sync
