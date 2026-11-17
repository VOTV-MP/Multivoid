// coop/world/cave_rock_sync.cpp -- see coop/world/cave_rock_sync.h.

#include "coop/world/cave_rock_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
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

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::cave_rock_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

constexpr const wchar_t* kSpawnerClass = L"antibreatherSPawner_C";
constexpr float kNearCm = 5000.f;   // the game's own distance past which its block shows the rock
constexpr uint64_t kReadMs = 500;
UE_FACT_FN(kRowBlock, "cave_rock_sync", L"antibreatherSPawner_C", L"block", Declared);
constexpr int kTag = 0x43415600;   // 'CAV'

std::atomic<N::Session*> g_session{nullptr};
bool g_registered = false;
int g_applying = 0;
std::unordered_map<std::wstring, bool> g_sent;   // spawner name -> closed, as last sent
uint64_t g_nextRead = 0;

uint64_t Now() { return ::GetTickCount64(); }

std::vector<void*> Spawners() {
    std::vector<void*> out;
    for (void* o : R::FindObjectsByClass(kSpawnerClass))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) out.push_back(o);
    return out;
}

void* Rock(void* spawner) {
    static int32_t sOff = -2;
    if (sOff == -2) sOff = R::FindPropertyOffset(R::ClassOf(spawner), L"blockRock");
    if (sOff < 0) return nullptr;
    void* const c = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(spawner) + sOff);
    return c && R::IsLive(c) ? c : nullptr;
}

// Closed: the rock shown (its bVisible). False when it does not read.
bool Closed(void* spawner, bool& out) {
    void* const rock = Rock(spawner);
    int32_t off = -1;
    uint8_t mask = 0;
    if (!rock || !R::FindBoolProperty(R::ClassOf(rock), L"bVisible", off, mask)) return false;
    out = (static_cast<const uint8_t*>(rock)[off] & mask) != 0;
    return true;
}

// The game's own two writes of its block: the rock shown or hidden, its collision QueryAndPhysics or none.
bool SetClosed(void* spawner, bool closed) {
    void* const rock = Rock(spawner);
    if (!rock || !ue_wrap::engine::SetComponentVisible(rock, closed, true)) return false;
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(rock), L"SetCollisionEnabled"));
    return f.valid() && f.Set<uint8_t>(L"NewType", closed ? 3 : 0) && ue_wrap::Call(rock, f);
}

std::wstring NameOf(void* o) {
    const std::wstring n = R::ToString(R::NameOf(o));
    return n.size() > sizeof(N::WireKey::data) ? std::wstring() : n;
}

void* ByName(const std::wstring& name) {
    for (void* s : Spawners())
        if (NameOf(s) == name) return s;
    return nullptr;
}

void Send(N::Session& s, const std::wstring& name, bool closed, int slot) {
    N::CaveRockPayload p{};
    N::WireKeyFromString(name, p.name);
    p.closed = closed ? 1 : 0;
    if (slot >= 0) s.SendReliableToSlot(slot, N::ReliableKind::CaveRockState, &p, sizeof(p));
    else s.SendReliable(N::ReliableKind::CaveRockState, &p, sizeof(p));
}

// Whether a client's player is within the game's 50 m of the spawner.
bool ClientNear(void* spawner) {
    ue_wrap::FVector at{};
    if (!ue_wrap::engine::TryGetActorLocation(spawner, at)) return false;
    for (uint8_t slot = 1; slot < N::kMaxPeers; ++slot) {
        coop::RemotePlayer* const rp = coop::players::Registry::Get().Puppet(slot);
        ue_wrap::FVector pl{};
        if (!rp || !rp->TryGetLocation(pl)) continue;
        const float dx = pl.X - at.X, dy = pl.Y - at.Y, dz = pl.Z - at.Z;
        if (dx * dx + dy * dy + dz * dz <= kNearCm * kNearCm) return true;
    }
    return false;
}

void* g_blockSpawner = nullptr;
bool g_blockWasClosed = false;

sg::Verdict OnPre(const sg::Call& call) {
    if (call.tag != kTag || g_applying) return sg::Verdict::Run;
    if (coop::spawn_authority::IsActiveClientSession()) return sg::Verdict::Cancel;   // the host's rock is the cave's
    g_blockSpawner = call.object;
    g_blockWasClosed = true;
    Closed(call.object, g_blockWasClosed);
    return sg::Verdict::Run;
}

void OnPost(const sg::Call& call) {
    if (call.tag != kTag || call.object != g_blockSpawner) return;
    g_blockSpawner = nullptr;
    N::Session* const s = g_session.load(std::memory_order_acquire);
    bool closed = false;
    if (!s || s->role() != N::Role::Host || g_blockWasClosed || !Closed(call.object, closed) || !closed) return;
    if (!ClientNear(call.object)) return;
    // The game shut it because the host's player went far; a client's player is still near, so it stays open.
    ++g_applying;
    SetClosed(call.object, false);
    --g_applying;
    UE_LOGI("cave_rock: the cave stays open on the host -- a client's player is within 50 m");
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!g_registered) {
        static uint64_t nextTry = 0;
        if (Now() >= nextTry) {
            nextTry = Now() + 1000;
            if ((g_registered = sg::WatchName(kRowBlock, kTag, &OnPre, &OnPost)))
                UE_LOGI("cave_rock: the Antibreather cave's block is watched");
        }
    }
    if (s->role() != N::Role::Host || Now() < g_nextRead) return;
    g_nextRead = Now() + kReadMs;
    for (void* sp : Spawners()) {
        const std::wstring name = NameOf(sp);
        bool closed = false;
        if (name.empty() || !Closed(sp, closed)) continue;
        const auto it = g_sent.find(name);
        if (it != g_sent.end() && it->second == closed) continue;
        Send(*s, name, closed, -1);
        g_sent[name] = closed;
        UE_LOGI("cave_rock: HOST the cave '%ls' is %s -- sent", name.c_str(), closed ? "shut" : "open");
    }
}

void OnState(const N::CaveRockPayload& p) {
    const std::wstring name = N::StringFromWireKey(p.name);
    void* const sp = ByName(name);
    bool closed = false;
    if (!sp || (Closed(sp, closed) && closed == (p.closed != 0))) return;
    ++g_applying;
    const bool ok = SetClosed(sp, p.closed != 0);
    --g_applying;
    UE_LOGI("cave_rock: CLIENT the cave '%ls' is %s, as the host's (ok=%d)", name.c_str(), p.closed ? "shut" : "open",
            ok ? 1 : 0);
}

void SendJoin(int slot) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != N::Role::Host) return;
    for (void* sp : Spawners()) {
        bool closed = false;
        const std::wstring name = NameOf(sp);
        if (!name.empty() && Closed(sp, closed)) Send(*s, name, closed, slot);
    }
}

void OnDisconnect() {
    g_sent.clear();
    g_nextRead = 0;
    g_blockSpawner = nullptr;
}

}  // namespace coop::cave_rock_sync
