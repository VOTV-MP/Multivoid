// coop/props/shrimp_eater_authority.cpp -- see coop/props/shrimp_eater_authority.h.
//
// shrimpEaterMaster_C.eat (0.9.0n): the shrimp is lib.closestActor(gamemode.shrimps, lastloc); a prop_food_C at or
// above 60 C, at or below 0 C, or under water is skipped, and so is one with player 0 within 10000 of lastloc, one
// wasRecentlyRendered(5), or a day too early. The host's check here reads the same spot and the same window for every
// client's player, from the same nearest shrimp.

#include "coop/props/shrimp_eater_authority.h"

#include "coop/net/session.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/slot_count.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <unordered_map>

namespace coop::shrimp_eater_authority {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowEat, "shrimp_eater_authority", L"shrimpEaterMaster_C", L"eat", Declared);
constexpr int kTag = 0x53484500;   // 'SHE'

constexpr float kNearCm = 10000.f;      // the game's own 100 m round the last spot
constexpr uint64_t kSeenMs = 5000;      // the game's wasRecentlyRendered(5)
constexpr float kViewCm = 30000.f;      // how far a client's player is taken to see a shrimp pack
constexpr float kViewCos = 0.5f;        // within 60 degrees of its aim
constexpr uint64_t kReadEveryMs = 500;

std::atomic<N::Session*> g_session{nullptr};
bool g_registered = false;
uint64_t g_nextRead = 0;
std::unordered_map<void*, uint64_t> g_seenAt;   // HOST: a shrimp, and when a client's player last had it in view

uint64_t Now() { return ::GetTickCount64(); }

bool IsHost() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == N::Role::Host;
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    return std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z));
}

// The gamemode's `shrimps`, the shrimp packs the eater picks from.
struct Arr { void** data; int32_t num; int32_t max; };
const Arr* Shrimps() {
    void* const gm = ue_wrap::world_singleton::Gamemode();
    const int32_t off = gm ? R::FindPropertyOffset(R::ClassOf(gm), L"shrimps") : -1;
    return off < 0 ? nullptr : reinterpret_cast<const Arr*>(static_cast<const uint8_t*>(gm) + off);
}

bool ReadVec(void* obj, const wchar_t* field, ue_wrap::FVector& out) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), field) : -1;
    if (off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(obj) + off, sizeof(out));
    return true;
}

// HOST, twice a second: each shrimp a client's player has in view now.
void ReadViews() {
    const Arr* a = Shrimps();
    if (!a || !a->data) return;
    const uint64_t now = Now();
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        coop::RemotePlayer& rp = coop::puppet_drive::Puppet(slot);
        ue_wrap::FVector eye{};
        if (!rp.valid() || !rp.TryGetEyePosition(eye)) continue;
        const ue_wrap::FVector aim = rp.GetLatestAimDirection();
        for (int32_t i = 0; i < a->num; ++i) {
            void* const shrimp = a->data[i];
            ue_wrap::FVector at{};
            if (!shrimp || !R::IsLive(shrimp) || !ue_wrap::engine::TryGetActorLocation(shrimp, at)) continue;
            const float d = Dist(at, eye);
            if (d > kViewCm || d < 1.f) continue;
            const float dot = (aim.X * (at.X - eye.X) + aim.Y * (at.Y - eye.Y) + aim.Z * (at.Z - eye.Z)) / d;
            if (dot > kViewCos) g_seenAt[shrimp] = now;
        }
    }
}

// HOST: the shrimp this eat would take, and why a client's player keeps it, or null.
const char* KeptByClient(void* master, void*& shrimp) {
    shrimp = nullptr;
    const Arr* a = Shrimps();
    ue_wrap::FVector last{};
    if (!a || !a->data || !ReadVec(master, L"lastloc", last)) return nullptr;
    float best = 1e30f;
    for (int32_t i = 0; i < a->num; ++i) {
        ue_wrap::FVector at{};
        void* const o = a->data[i];
        if (!o || !R::IsLive(o) || !ue_wrap::engine::TryGetActorLocation(o, at)) continue;
        const float d = Dist(at, last);
        if (d < best) { best = d; shrimp = o; }
    }
    if (!shrimp) return nullptr;
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        coop::RemotePlayer& rp = coop::puppet_drive::Puppet(slot);
        ue_wrap::FVector eye{};
        if (rp.valid() && rp.TryGetEyePosition(eye) && Dist(eye, last) < kNearCm) return "a client's player is near";
    }
    const auto it = g_seenAt.find(shrimp);
    if (it != g_seenAt.end() && Now() - it->second < kSeenMs) return "a client's player saw it";
    return nullptr;
}

sg::Verdict OnEatPre(const sg::Call& call) {
    if (call.tag != kTag) return sg::Verdict::Run;
    if (coop::spawn_authority::IsActiveClientSession()) return sg::Verdict::Cancel;   // the host's Arirals eat
    if (!IsHost()) return sg::Verdict::Run;
    void* shrimp = nullptr;
    const char* why = KeptByClient(call.object, shrimp);
    if (!why) return sg::Verdict::Run;
    static void* s_lastShrimp = nullptr;
    static const char* s_lastWhy = nullptr;
    if (shrimp != s_lastShrimp || why != s_lastWhy)   // said once a shrimp and reason, not at every 15 s eat
        UE_LOGI("shrimp_eater: the Arirals leave shrimp %p -- %s", shrimp, why);
    s_lastShrimp = shrimp;
    s_lastWhy = why;
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    const uint64_t now = Now();
    if (!g_registered) {
        static uint64_t nextTry = 0;
        if (now >= nextTry) {
            nextTry = now + 1000;
            if ((g_registered = sg::WatchName(kRowEat, kTag, &OnEatPre, nullptr)))
                UE_LOGI("shrimp_eater: the Arirals' eat is watched");
        }
    }
    if (s->role() != N::Role::Host || now < g_nextRead) return;
    g_nextRead = now + kReadEveryMs;
    for (auto it = g_seenAt.begin(); it != g_seenAt.end();)
        it = now - it->second >= kSeenMs ? g_seenAt.erase(it) : std::next(it);
    ReadViews();
}

void OnDisconnect() { g_seenAt.clear(); }

}  // namespace coop::shrimp_eater_authority
