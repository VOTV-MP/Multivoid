// coop/props/chip_scrap_sync.cpp -- see coop/props/chip_scrap_sync.h.
//
// 0.9.0n. actorChipPile_C::turnIntoScrap picks a scrap row by its `chipType` (a food, a prop by name, or a ball),
// mints it at the pile, K2_DestroyActor's the pile and plays shovelDig_Cue in 2D. mainPlayer_C's shovel notify `dig`
// calls it on the actorChipPile_C its trace hit; prop_rake_C calls it too.

#include "coop/props/chip_scrap_sync.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_instances.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>

namespace coop::chip_scrap_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace E  = ue_wrap::engine;
namespace EL = coop::element;

UE_FACT_FN(kRowTurnIntoScrap, "chip_scrap_sync", L"actorChipPile_C", L"turnIntoScrap", Declared);

constexpr const wchar_t* kPileClass = L"actorChipPile_C";
constexpr int kTag = 0x43485053;          // 'CHPS'
constexpr float kReachCm = 600.f;         // the sender's body to the pile (a shovel's trace is 2 m; a body lags)
constexpr float kPlaceCm = 150.f;         // the asked place to a pile with no element
constexpr uint64_t kGapMs = 250;          // four asks a second per sender
constexpr int kCap = 512;

std::atomic<N::Session*> g_session{nullptr};
bool g_watched = false;
uint64_t g_lastAt[N::kMaxPeers] = {};
void* g_piles[kCap];

bool IsClient() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    return s && s->role() == N::Role::Client;
}

bool IsPile(void* obj) { return obj && R::IsLive(obj) && R::NameEquals(R::NameOf(R::ClassOf(obj)), kPileClass); }

float Dist2(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

// CLIENT: the scoop is the host's to run.
sg::Verdict OnScrapPre(const sg::Call& call) {
    if (call.tag != kTag || !IsClient() || !call.object) return sg::Verdict::Run;
    N::Session* const s = g_session.load(std::memory_order_acquire);
    N::ChipScrapAskPayload p{};
    p.elementId = static_cast<uint32_t>(EL::Registry::Get().EidForActor(call.object));
    ue_wrap::FVector at{};
    E::TryGetActorLocation(call.object, at);
    p.x = at.X;
    p.y = at.Y;
    p.z = at.Z;
    const bool sent = s && s->connected() && s->SendReliableToSlot(0, N::ReliableKind::ChipScrapAsk, &p, sizeof(p));
    UE_LOGI("chip_scrap: this client scooped the trash pile eid=%u at (%.0f,%.0f,%.0f) -- asked of the host (sent=%d)",
            p.elementId, at.X, at.Y, at.Z, sent ? 1 : 0);
    return sg::Verdict::Cancel;
}

// HOST: the pile the ask names: its element when it is a live pile, else the pile nearest the place.
void* Resolve(const N::ChipScrapAskPayload& p) {
    if (p.elementId != 0) {
        void* const a = EL::LiveActorOfType(static_cast<EL::ElementId>(p.elementId), EL::ElementType::Prop);
        if (IsPile(a)) return a;
    }
    const ue_wrap::FVector at{p.x, p.y, p.z};
    const int n = ue_wrap::world_instances::Find(kPileClass, g_piles, kCap);
    void* best = nullptr;
    float bestD = kPlaceCm * kPlaceCm;
    for (int k = 0; k < n; ++k) {
        ue_wrap::FVector q{};
        if (!IsPile(g_piles[k]) || !E::TryGetActorLocation(g_piles[k], q)) continue;
        const float d = Dist2(q, at);
        if (d < bestD) {
            bestD = d;
            best = g_piles[k];
        }
    }
    return best;
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || g_watched) return;
    if ((g_watched = sg::WatchName(kRowTurnIntoScrap, kTag, &OnScrapPre, nullptr)))
        UE_LOGI("chip_scrap: a trash pile's scoop (turnIntoScrap) is watched");
}

void OnAsk(uint8_t slot, const N::ChipScrapAskPayload& p) {
    if (slot < 1 || slot >= N::kMaxPeers) return;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
        UE_LOGW("chip_scrap: slot %u's scoop refused: a place that is not a number", slot);
        return;
    }
    const uint64_t now = ::GetTickCount64();
    if (now - g_lastAt[slot] < kGapMs) {
        UE_LOGW("chip_scrap: slot %u's scoop refused: within %llu ms of its last", slot,
                static_cast<unsigned long long>(kGapMs));
        return;
    }
    g_lastAt[slot] = now;
    void* const pile = Resolve(p);
    if (!pile) {
        UE_LOGW("chip_scrap: slot %u's scoop of eid=%u refused: no trash pile here", slot, p.elementId);
        return;
    }
    coop::RemotePlayer* const rp = coop::players::Registry::Get().Puppet(slot);
    void* const body = rp ? rp->actor() : nullptr;
    ue_wrap::FVector bodyAt{}, pileAt{};
    if (!body || !E::TryGetActorLocation(body, bodyAt) || !E::TryGetActorLocation(pile, pileAt) ||
        Dist2(bodyAt, pileAt) > kReachCm * kReachCm) {
        UE_LOGW("chip_scrap: slot %u's scoop of eid=%u refused: its body is not within %.0f m of the pile", slot,
                p.elementId, kReachCm / 100.f);
        return;
    }
    ue_wrap::ParamFrame pf(R::FindDispatchFunction(R::ClassOf(pile), L"turnIntoScrap", nullptr));
    const bool ran = pf.valid() && ue_wrap::Call(pile, pf);
    UE_LOGI("chip_scrap: slot %u scooped the trash pile eid=%u -- the host's turnIntoScrap ran=%d", slot, p.elementId,
            ran ? 1 : 0);
}

void OnDisconnect() {
    for (uint64_t& t : g_lastAt) t = 0;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::chip_scrap_sync
