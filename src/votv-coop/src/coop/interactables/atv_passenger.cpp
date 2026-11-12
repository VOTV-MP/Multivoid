// coop/interactables/atv_passenger.cpp -- see coop/interactables/atv_passenger.h.

#include "coop/interactables/atv_passenger.h"

#include "coop/interactables/atv_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_sound.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv_passenger_seat.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::atv_passenger {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace PS = ue_wrap::atv_passenger_seat;
using coop::atv_sync::PassengerAsk;

std::atomic<coop::net::Session*> g_session{nullptr};

// Want: the use key asked for the spot; the ask goes out on the next Tick. Asked: a client's ask is in flight.
enum class Phase : uint8_t { Idle, Want, Asked, Seated };
Phase g_phase = Phase::Idle;
void* g_atv = nullptr;
int32_t g_atvIdx = -1;
void* g_box = nullptr;
int32_t g_boxIdx = -1;
uint64_t g_askedMs = 0;
int g_capsuleWas = -1;
uint64_t g_eyeUntilMs = 0;   // the eye is set to its height until then
uint64_t g_eyeNextMs = 0;
constexpr uint64_t kEyeSettleMs = 3000, kEyeStepMs = 300;   // the player's capsule collision before the seat took its physics (-1: untouched)
constexpr uint64_t kAnswerWaitMs = 5000;   // an ask with no answer this long is given up

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void* Me() { return coop::players::Registry::Get().Local(); }

bool AtvLive() { return g_atv && R::IsLiveByIndex(g_atv, g_atvIdx); }

// The host's table for `atv` as this peer holds it.
bool TableOf(void* atv, coop::atv_sync::TableView& out) {
    struct Find { void* atv; coop::atv_sync::TableView* out; bool found; } f{atv, &out, false};
    coop::atv_sync::ForEachIndexed([](void* ctx, const std::wstring& key, void* actor) {
        auto* x = static_cast<Find*>(ctx);
        if (!x->found && actor == x->atv) x->found = coop::atv_sync::ReadTable(key, *x->out);
    }, &f);
    return f.found;
}

void RemoveBox() {
    if (g_box && R::IsLiveByIndex(g_box, g_boxIdx)) E::DestroyActor(g_box);
    g_box = nullptr;
    g_boxIdx = -1;
}

void Reset() {
    RemoveBox();
    g_phase = Phase::Idle;
    g_atv = nullptr;
    g_atvIdx = -1;
    g_askedMs = 0;
}

// AActor::SetActorEnableCollision / GetActorEnableCollision, the whole actor's collision.
bool ActorCollision(void* actor, int set = -1) {
    void* cls = R::ClassOf(actor);
    if (set >= 0) {
        void* fn = R::FindDispatchFunctionCached(cls, L"SetActorEnableCollision");
        ue_wrap::ParamFrame pf(fn);
        if (fn && pf.valid() && pf.Set<bool>(L"bNewActorEnableCollision", set != 0)) ue_wrap::Call(actor, pf);
    }
    void* get = R::FindDispatchFunctionCached(cls, L"GetActorEnableCollision");
    ue_wrap::ParamFrame g(get);
    return get && g.valid() && ue_wrap::Call(actor, g) && g.Get<bool>(L"ReturnValue");
}

// The kerfurs within kKerfurAtSeatCm of the seat. The game's sit tests the seat's reach with a capsule swept from the
// player to it, ignoring only the ATV; a kerfur at the ATV (climbing on to ride the free spot, or standing where it got
// off; on a client, the host's kerfur's copy) stands in that sweep and the sit is refused as "unreachable".
constexpr float kKerfurAtSeatCm = 300.f;
std::vector<void*> KerfursAtSeat(void* box) {
    struct Ctx { ue_wrap::FVector at; std::vector<void*> out; } c{};
    if (!E::TryGetActorLocation(box, c.at)) return {};
    if (void* cls = ue_wrap::object_index::ClassByName(L"kerfurOmega_C"))
        ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* o, int32_t) {
            auto* k = static_cast<Ctx*>(ctx);
            ue_wrap::FVector at{};
            if (!R::IsLive(o) || R::NameStartsWith(R::NameOf(o), L"Default__") || !E::TryGetActorLocation(o, at))
                return;
            const float dx = at.X - k->at.X, dy = at.Y - k->at.Y, dz = at.Z - k->at.Z;
            if (dx * dx + dy * dy + dz * dz < kKerfurAtSeatCm * kKerfurAtSeatCm) k->out.push_back(o);
        }, &c);
    return c.out;
}

// The game's own sit on the box, with the kerfurs at the seat out of its reach test for the length of the call.
void SitPastKerfurs(void* me, void* box) {
    std::vector<void*> cleared;
    for (void* k : KerfursAtSeat(box))
        if (ActorCollision(k)) {
            ActorCollision(k, 0);
            cleared.push_back(k);
        }
    PS::Sit(me, g_atv, box);
    for (void* k : cleared)
        if (R::IsLive(k)) ActorCollision(k, 1);
}

// The seat the host granted: the sit box, the game's own sit, then the player's capsule to query only. A sit the game refuses
// (its own hint says why) gives the spot back.
void Mount() {
    void* me = Me();
    void* sittingOn = nullptr;
    g_box = AtvLive() && me ? PS::SpawnSitBox(g_atv) : nullptr;
    g_boxIdx = g_box ? R::InternalIndexOf(g_box) : -1;
    if (g_box) SitPastKerfurs(me, g_box);
    if (!g_box || !E::ReadMainPlayerSittingOn(me, sittingOn) || sittingOn != g_atv) {
        UE_LOGI("atv_passenger: the game's sit on ATV %p did not take (box=%p) -- the spot goes back", g_atv, g_box);
        if (AtvLive()) coop::atv_sync::AskPassenger(g_atv, false);
        Reset();
        return;
    }
    // Query only: the sit welds the capsule to the ATV, and a capsule with physics there is knocked down by the ATV's
    // own corrections. Its movement still sweeps by query, so the game's unsit sets it down on the ground.
    g_capsuleWas = E::GetActorRootCollisionEnabled(me);
    if (g_capsuleWas == 2 || g_capsuleWas == 3) E::SetActorRootCollisionEnabled(me, 1);
    else g_capsuleWas = -1;
    g_phase = Phase::Seated;
    g_eyeUntilMs = NowMs() + kEyeSettleMs;
    g_eyeNextMs = 0;
    UE_LOGI("atv_passenger: seated as the passenger on ATV %p", g_atv);
}

// The seat ended on this machine (the player's own unsit, a ragdoll, the ATV gone): collision back, the box gone,
// and the leave to the host unless the host's table already let the spot go.
void RestoreCapsule() {
    if (g_capsuleWas >= 0)
        if (void* me = Me()) E::SetActorRootCollisionEnabled(me, static_cast<uint8_t>(g_capsuleWas));
    g_capsuleWas = -1;
}

void Dismounted(const char* why, bool tellHost) {
    RestoreCapsule();
    if (tellHost && AtvLive()) coop::atv_sync::AskPassenger(g_atv, false);
    UE_LOGI("atv_passenger: off ATV %p -- %s", g_atv, why);
    Reset();
}

void Refused(const char* why) {
    UE_LOGI("atv_passenger: the passenger spot on ATV %p refused -- %s", g_atv, why);
    if (void* me = Me()) coop::prop_sound::PlayDenyClick(me);
    Reset();
}

// The eye to its height over the ATV's root, by moving the seated player: the sit's crouch takes or not by timing, and
// the eye lands 25 or 70 cm over the capsule's centre. Measured, not assumed; the camera's lag settles over the steps.
void SettleEye(void* me) {
    const uint64_t now = NowMs();
    if (now > g_eyeUntilMs || now < g_eyeNextMs) return;
    g_eyeNextMs = now + kEyeStepMs;
    ue_wrap::FVector atv{}, at{};
    if (!E::TryGetActorLocation(g_atv, atv) || !E::TryGetActorLocation(me, at)) return;
    const float eyeOverMe = E::GetCameraLocation().Z - at.Z;
    const float wantZ = atv.Z + PS::kEyeAboveRootCm - eyeOverMe;
    if (std::fabs(wantZ - at.Z) < 2.f) return;
    E::SetActorLocation(me, ue_wrap::FVector{at.X, at.Y, wantZ});
    if (now + kEyeStepMs > g_eyeUntilMs)
        UE_LOGI("atv_passenger: the eye stands %.0f cm over the capsule; the seat is set to put it %.0f cm over the ATV",
                eyeOverMe, PS::kEyeAboveRootCm);
}

void TickSeated() {
    void* me = Me();
    void* sittingOn = nullptr;
    if (!AtvLive()) {
        if (me) PS::Unsit(me);
        return Dismounted("the ATV is gone", false);
    }
    if (!me || (E::ReadMainPlayerSittingOn(me, sittingOn) && sittingOn != g_atv))
        return Dismounted("the player got off", true);
    SettleEye(me);
    coop::atv_sync::TableView t;
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    if (TableOf(g_atv, t) && t.passengerSlot != self) {
        PS::Unsit(me);
        Dismounted("the host's table no longer gives this player the spot", false);
    }
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

bool OnUseAtOccupied(void* atv) {
    if (g_phase != Phase::Idle) return true;
    coop::atv_sync::TableView t;
    if (!atv || !TableOf(atv, t) || t.passengerSlot != 0xFF) return false;
    g_phase = Phase::Want;
    g_atv = atv;
    g_atvIdx = R::InternalIndexOf(atv);
    return true;
}

bool Riding() { return g_phase != Phase::Idle; }

void OnAnswer(void* atv, bool admitted, uint8_t reason) {
    if (g_phase != Phase::Asked || atv != g_atv) {
        // An answer for an ask this machine gave up: a grant is handed back.
        if (admitted && atv) coop::atv_sync::AskPassenger(atv, false);
        return;
    }
    if (!admitted) {
        char why[32];
        std::snprintf(why, sizeof(why), "reason %u", static_cast<unsigned>(reason));
        return Refused(why);
    }
    Mount();
}

void Tick() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    switch (g_phase) {
    case Phase::Idle:
        return;
    case Phase::Want:
        if (!AtvLive()) return Reset();
        switch (coop::atv_sync::AskPassenger(g_atv, true)) {
        case PassengerAsk::Granted: return Mount();
        case PassengerAsk::Refused: return Refused("the host's table");
        case PassengerAsk::Sent:    g_phase = Phase::Asked; g_askedMs = NowMs(); return;
        case PassengerAsk::Failed:  return Reset();
        }
        return;
    case Phase::Asked:
        if (NowMs() - g_askedMs > kAnswerWaitMs) {
            UE_LOGW("atv_passenger: no answer to the passenger ask on ATV %p in %llu ms -- given up", g_atv,
                    static_cast<unsigned long long>(kAnswerWaitMs));
            Reset();
        }
        return;
    case Phase::Seated:
        return TickSeated();
    }
}

void OnDisconnect() {
    if (g_phase == Phase::Seated)
        if (void* me = Me()) PS::Unsit(me);
    RestoreCapsule();
    Reset();
}

}  // namespace coop::atv_passenger
