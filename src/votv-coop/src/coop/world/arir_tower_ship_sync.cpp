// coop/world/arir_tower_ship_sync.cpp -- see coop/world/arir_tower_ship_sync.h.
//
// arirShip_tower_C's begin play loop (0.9.0n), every 5 s: `visible` -> appear(false); else timeZ.Z >= 20 and
// (timeZ.X <= 3 or timeZ.X >= 21) and randomBoolWithWeight(0.1) and box.IsOverlappingActor(player 0's pawn) and not
// sphere_0.WasRecentlyRendered -> appear(true). The host's roll for a client reads the same, with that client's puppet
// in the box and its streamed aim standing for the render test.

#include "coop/world/arir_tower_ship_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/slot_count.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/daynightcycle.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>

namespace coop::arir_tower_ship_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

constexpr const wchar_t* kShipClass = L"arirShip_tower_C";
UE_FACT_FN(kRowAppear, "arir_tower_ship_sync", L"arirShip_tower_C", L"appear", Declared);
constexpr int kTag = 0x41525454;   // 'ARTT'
constexpr uint64_t kRollEveryMs = 5000;
constexpr float kViewCos = 0.5f;   // a client's player looking within 60 degrees of the ship is taken to have seen it

std::atomic<N::Session*> g_session{nullptr};
bool g_registered = false;
int g_applying = 0;
uint64_t g_nextRoll = 0;
int g_sent = -1;   // HOST: what the clients were last told, -1 nothing
std::mt19937 g_rng{std::random_device{}()};

uint64_t Now() { return ::GetTickCount64(); }

bool Visible(void* ship) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!ship || !R::FindBoolProperty(R::ClassOf(ship), L"visible", off, mask)) return false;
    return (static_cast<const uint8_t*>(ship)[off] & mask) != 0;
}

void* Field(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return nullptr;
    void* v = nullptr;
    std::memcpy(&v, static_cast<const uint8_t*>(obj) + off, sizeof(v));
    return v && R::IsLive(v) ? v : nullptr;
}

void* Ship() {
    for (void* o : R::FindObjectsByClass(kShipClass))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) return o;
    return nullptr;
}

bool Appear(void* ship, bool dir) {
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(ship), L"appear"));
    ++g_applying;
    const bool ran = f.valid() && f.Set<bool>(L"dir", dir) && ue_wrap::Call(ship, f);
    --g_applying;
    return ran;
}

bool Send(N::Session& s, void* ship, bool shown, int slot) {
    ue_wrap::FVector at{};
    if (!ue_wrap::engine::TryGetActorLocation(ship, at)) return false;
    N::EventCuePayload p{};
    p.cueId = kCueTowerShip;
    p.x = shown ? 1.f : 0.f;
    p.y = at.Z;   // which ship: there is one, and its height names it
    return slot < 0 ? s.SendReliable(N::ReliableKind::EventCue, &p, sizeof(p))
                    : s.SendReliableToSlot(slot, N::ReliableKind::EventCue, &p, sizeof(p));
}

sg::Verdict OnAppearPre(const sg::Call& call) {
    if (call.tag != kTag || g_applying) return sg::Verdict::Run;
    return coop::spawn_authority::IsActiveClientSession() ? sg::Verdict::Cancel : sg::Verdict::Run;
}

void OnAppearPost(const sg::Call& call) {
    if (call.tag != kTag) return;
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != N::Role::Host) return;
    const bool shown = Visible(call.object);
    if (g_sent == (shown ? 1 : 0)) return;
    if (!Send(*s, call.object, shown, -1)) return;
    g_sent = shown ? 1 : 0;
    UE_LOGI("arir_tower_ship: the tower's Ariral ship %s -- EventCue sent", shown ? "appears" : "goes");
}

// HOST: the game's roll, for each client's player in the box.
void RollForClients() {
    void* const ship = Ship();
    if (!ship || Visible(ship)) return;
    int32_t hour = 0, minute = 0, day = 0;
    if (!ue_wrap::daynightcycle::ReadTimeZ(hour, minute, day) || day < 20 || (hour > 3 && hour < 21)) return;
    void* const box = Field(ship, L"Box");
    ue_wrap::FVector at{};
    if (!box || !ue_wrap::engine::TryGetActorLocation(ship, at)) return;
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        coop::RemotePlayer& rp = coop::puppet_drive::Puppet(slot);
        void* const body = rp.valid() ? rp.GetActor() : nullptr;
        if (!body || !ue_wrap::engine::IsOverlappingActor(box, body)) continue;
        if (std::uniform_real_distribution<float>(0.f, 1.f)(g_rng) >= 0.1f) continue;   // the game's own odds
        ue_wrap::FVector eye{};
        if (rp.TryGetEyePosition(eye)) {
            const ue_wrap::FVector aim = rp.GetLatestAimDirection();
            const float dx = at.X - eye.X, dy = at.Y - eye.Y, dz = at.Z - eye.Z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d > 1.f && (aim.X * dx + aim.Y * dy + aim.Z * dz) / d > kViewCos) continue;   // it saw the ship's place
        }
        UE_LOGI("arir_tower_ship: slot %d's player is at the tower's top -- the ship appears (ran=%d)", slot,
                Appear(ship, true) ? 1 : 0);
        return;
    }
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
            if ((g_registered = sg::WatchName(kRowAppear, kTag, &OnAppearPre, &OnAppearPost)))
                UE_LOGI("arir_tower_ship: the tower's Ariral ship's appear is watched");
        }
    }
    if (s->role() != N::Role::Host || !s->connected() || now < g_nextRoll) return;
    g_nextRoll = now + kRollEveryMs;
    RollForClients();
}

void SendJoin(int slot) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    void* const ship = Ship();
    if (!s || s->role() != N::Role::Host || !ship || !Visible(ship)) return;
    Send(*s, ship, true, slot);
}

bool OnCue(const N::EventCuePayload& p) {
    if (p.cueId != kCueTowerShip) return false;
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != N::Role::Client) return true;
    void* const ship = Ship();
    const bool shown = p.x > 0.5f;
    if (!ship) {
        UE_LOGW("arir_tower_ship: the host's tower ship %s, and this world has none -- not shown",
                shown ? "appears" : "goes");
        return true;
    }
    if (Visible(ship) == shown) return true;
    UE_LOGI("arir_tower_ship: the host's tower ship %s here too (ran=%d)", shown ? "appears" : "goes",
            Appear(ship, shown) ? 1 : 0);
    return true;
}

void OnDisconnect() {
    g_nextRoll = 0;
    g_sent = -1;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::arir_tower_ship_sync
