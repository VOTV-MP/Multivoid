// coop/props/body_push.cpp -- see coop/props/body_push.h.

#include "coop/props/body_push.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_hold_claim.h"
#include "coop/props/prop_syncer.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine_attach.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace coop::props::body_push {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
using ue_wrap::FRotator;
using ue_wrap::FVector;
using Clock = std::chrono::steady_clock;

constexpr float kListCm = 400.f;    // the props kept to watch, from this peer's player
constexpr float kTouchCm = 150.f;   // a push's prop from this peer's player
constexpr float kMoveCm = 3.f;      // a move between two reads
constexpr float kTurnDeg = 3.f;     // a turn between two reads
constexpr size_t kListCap = 64;
constexpr auto kListEvery = std::chrono::seconds(1);
constexpr auto kReadEvery = std::chrono::milliseconds(100);

struct Watched {
    ue_wrap::CachedObjRef ref;
    FVector loc{};
    FRotator rot{};
};
std::vector<Watched> g_list;
Clock::time_point g_nextList{}, g_nextRead{};
uint32_t g_pushes = 0;

float Dist2(const FVector& a, const FVector& b) {
    const float x = a.X - b.X, y = a.Y - b.Y, z = a.Z - b.Z;
    return x * x + y * y + z * z;
}

float TurnDeg(const FRotator& a, const FRotator& b) {
    const float p = std::fabs(ue_wrap::NormalizeAxis(a.Pitch - b.Pitch));
    const float y = std::fabs(ue_wrap::NormalizeAxis(a.Yaw - b.Yaw));
    const float r = std::fabs(ue_wrap::NormalizeAxis(a.Roll - b.Roll));
    return std::fmax(p, std::fmax(y, r));
}

struct ListCtx {
    FVector me;
    std::vector<Watched>* out;
};

void EachProp(void* ctx, void* obj, int32_t) {
    auto* c = static_cast<ListCtx*>(ctx);
    if (c->out->size() >= kListCap) return;
    FVector loc{};
    FRotator rot{};
    if (!E::TryGetRootPoseRaw(obj, loc, rot) || Dist2(loc, c->me) > kListCm * kListCm) return;
    Watched w;
    w.ref.Set(obj);
    w.loc = loc;
    w.rot = rot;
    c->out->push_back(w);
}

void EachClass(void* ctx, void* cls, void* anyInstance) {
    if (!anyInstance || !ue_wrap::prop::IsDescendantOfProp(anyInstance)) return;
    ue_wrap::object_index::ForEachInstance(cls, &EachProp, ctx);
}

// This peer's player is the nearest body to `at`, within the touch distance.
bool MineToPush(const FVector& at, const FVector& me) {
    const float mine = Dist2(at, me);
    if (mine > kTouchCm * kTouchCm) return false;
    coop::players::Registry& reg = coop::players::Registry::Get();
    for (uint8_t slot = 0; slot < coop::net::kMaxPeers; ++slot) {
        coop::RemotePlayer* rp = reg.Puppet(slot);
        void* body = rp && rp->valid() ? rp->GetActor() : nullptr;
        FVector by{};
        if (body && E::TryGetRootLocationRaw(body, by) && Dist2(at, by) < mine) return false;
    }
    return true;
}

// A prop this lane may push: a keyed prop under the one-holder rule, not an ATV (its own lane), nobody holding or
// driving it, and on a client no stream parking it.
bool Pushable(void* actor, bool isHost) {
    if (!coop::prop_hold_claim::IsArbitrated(actor)) return false;
    if (ue_wrap::atv::IsAtv(actor) || coop::prop_drive_host::InAHand(actor)) return false;
    if (!isHost && coop::prop_drive_stream::IsParked(actor)) return false;
    return true;
}

}  // namespace

void Tick(coop::net::Session& s, bool isHost) {
    if (!s.connected()) return;
    void* me = coop::players::Registry::Get().Local();
    FVector mine{};
    if (!me || !E::TryGetRootLocationRaw(me, mine)) return;
    const Clock::time_point now = Clock::now();
    if (now >= g_nextList) {
        g_nextList = now + kListEvery;
        std::vector<Watched> fresh;
        ListCtx ctx{mine, &fresh};
        ue_wrap::object_index::ForEachClass(&EachClass, &ctx);
        // Keep the last read of a prop already watched, so a move across the refresh is not lost.
        for (Watched& f : fresh)
            for (const Watched& old : g_list)
                if (old.ref.Raw() == f.ref.Raw()) {
                    f.loc = old.loc;
                    f.rot = old.rot;
                }
        g_list.swap(fresh);
    }
    if (now < g_nextRead) return;
    g_nextRead = now + kReadEvery;
    for (Watched& w : g_list) {
        void* actor = w.ref.Get();
        FVector loc{};
        FRotator rot{};
        if (!actor || !E::TryGetRootPoseRaw(actor, loc, rot)) continue;
        const bool moved = Dist2(loc, w.loc) > kMoveCm * kMoveCm || TurnDeg(rot, w.rot) > kTurnDeg;
        w.loc = loc;
        w.rot = rot;
        if (!moved || !MineToPush(loc, mine) || !Pushable(actor, isHost)) continue;
        ++g_pushes;
        if (g_pushes == 1 || (g_pushes % 50) == 0)
            UE_LOGI("body_push: this player pushed '%ls' %p -- %s (#%u)", R::ClassNameOf(actor).c_str(), actor,
                    isHost ? "it coasts on the driven-prop channel" : "this peer syncs it", g_pushes);
        if (isHost) coop::prop_drive_host::Coast(actor, "body push");
        else coop::prop_syncer::Claim(actor, coop::net::PropSyncReason::BodyPush);
    }
}

void OnDisconnect() {
    if (g_pushes) UE_LOGI("body_push: session end -- %u pushes taken", g_pushes);
    g_pushes = 0;
    g_list.clear();
    g_nextList = g_nextRead = Clock::time_point{};
}

}  // namespace coop::props::body_push
