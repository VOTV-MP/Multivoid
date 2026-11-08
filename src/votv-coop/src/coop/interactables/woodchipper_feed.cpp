// coop/interactables/woodchipper_feed.cpp -- see coop/interactables/woodchipper_feed.h.

#include "coop/interactables/woodchipper_feed.h"

#include "coop/element/intent_authority.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_syncer.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/woodchipper.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::woodchipper_feed {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace W = ue_wrap::woodchipper;
namespace N = coop::net;
using Clock = std::chrono::steady_clock;

constexpr float kReachUU = 400.f;    // the sender from the chipper, as its pour and press
constexpr float kNearBoxUU = 300.f;  // the host's copy of the prop from the eat box: its stream lags the client's
constexpr size_t kQueueCap = 8;
constexpr auto kAskGap = std::chrono::seconds(1);  // CLIENT: one ask a prop a second

const wchar_t* const kIntakeFn =
    L"BndEvt__eat_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature";

struct Feed {
    int slot;
    std::wstring chipper, prop;
    Clock::time_point since;
    bool nearBox = false;        // the host's copy came near the box: from then on only the drives' ends are awaited
    Clock::time_point nearAt{};
};
constexpr auto kCatchUp = std::chrono::milliseconds(1500);  // the host's copy may trail the client's this long
constexpr auto kStreamEnd = std::chrono::seconds(5);         // a thrown prop's stream ends this soon after its rest
std::vector<Feed> g_inbox;                                        // HOST
std::unordered_map<std::wstring, Clock::time_point> g_lastAsk;    // CLIENT: the prop's key, its last ask
Counts g_counts;

N::Session* g_session = nullptr;

std::wstring KeyOf(void* prop) {
    std::wstring k = ue_wrap::prop::GetInteractableKeyString(prop);
    if (k.empty() || k == L"None" || k.size() >= sizeof(N::WireKey::data)) return {};
    return k;
}

void* EatOf(void* chipper) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(chipper), L"eat");
    if (off < 0) return nullptr;
    void* eat = nullptr;
    std::memcpy(&eat, static_cast<const uint8_t*>(chipper) + off, sizeof(eat));
    return eat && R::IsLive(eat) ? eat : nullptr;
}

// The chipper's own begin-overlap on its eat box for the prop's mesh, as the engine raises it.
bool RunIntake(void* chipper, void* eat, void* prop, void* mesh) {
    void* fn = R::FindFunction(R::ClassOf(chipper), kIntakeFn);
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid()) return false;
    f.Set<void*>(L"OverlappedComponent", eat);
    f.Set<void*>(L"OtherActor", prop);
    f.Set<void*>(L"OtherComp", mesh);
    return ue_wrap::Call(chipper, f);
}

void Refuse(const Feed& f, const char* why) {
    ++g_counts.refused;
    UE_LOGW("woodchipper_feed: slot %d's feed of '%ls' to '%ls' refused -- %s", f.slot, f.prop.c_str(), f.chipper.c_str(),
            why);
}

// False while the feed waits for the host's copy to reach the box (its stream trails the client's); true once it is
// done or refused.
bool Serve(N::Session& s, Feed& f) {
    if (!s.IsSlotWorldReady(f.slot)) { Refuse(f, "its world is not ready"); return true; }
    void* chipper = coop::prop_key_index::Find(f.chipper);
    if (!chipper || !W::IsChipper(chipper)) { Refuse(f, "no chipper by that key"); return true; }
    if (coop::players::Registry::Get().LifeOf(static_cast<uint8_t>(f.slot)) != coop::players::PuppetLife::Alive)
        { Refuse(f, "the sender is down"); return true; }
    if (!coop::element::IntentTarget::ForClientIntent(s, static_cast<uint8_t>(f.slot), kReachUU).Authorize(chipper))
        { Refuse(f, "the sender is out of the chipper's reach"); return true; }
    float fuel = 0.f;
    bool active = false;
    if (!W::Read(chipper, fuel, active) || !active || !(fuel > 0.f)) { Refuse(f, "the chipper is not running"); return true; }
    void* prop = coop::prop_key_index::Find(f.prop);
    if (!prop) {  // eaten already: the host's chipper took its own copy as it entered, before this feed was served
        UE_LOGI("woodchipper_feed: slot %d's feed of '%ls' finds it gone -- the host's chipper ate it already", f.slot,
                f.prop.c_str());
        return true;
    }
    if (!R::IsLive(prop) || !ue_wrap::prop::IsDescendantOfProp(prop) || prop == chipper)
        { Refuse(f, "no prop by that key"); return true; }
    const int driver = coop::remote_prop::DrivingSlotOf(prop, f.slot);
    if (driver >= 0) { Refuse(f, "another player holds it"); return true; }
    void* mine = coop::players::Registry::Get().Local();
    E::MainPlayerGrabState gs{};
    if (mine && E::ReadMainPlayerGrabState(mine, gs) && gs.grabbingActor == prop) { Refuse(f, "the host holds it"); return true; }
    void* eat = EatOf(chipper);
    void* mesh = ue_wrap::prop::GetStaticMesh(prop);
    ue_wrap::FVector at{}, pl{};
    if (!eat || !mesh || !E::TryGetComponentLocation(eat, at) || !E::TryGetActorLocation(prop, pl))
        { Refuse(f, "the chipper's box or the prop's mesh does not read"); return true; }
    const float dx = pl.X - at.X, dy = pl.Y - at.Y, dz = pl.Z - at.Z;
    if (!f.nearBox) {
        if (dx * dx + dy * dy + dz * dz > kNearBoxUU * kNearBoxUU) {
            if (Clock::now() - f.since < kCatchUp) return false;
            Refuse(f, "the host's copy never came near the box");
            return true;
        }
        f.nearBox = true;
        f.nearAt = Clock::now();
    }
    // The intake reads the body a frame later (its Delay(0)) and takes only a simulating one, so nothing may drive the
    // host's copy then: the sender let go as it fed it, and its hold's release and the stream of the falling copy that
    // follows end here on their own, which the feed waits for.
    if (coop::remote_prop::DrivingSlotOf(prop) == f.slot || coop::prop_drive_stream::IsParked(prop)) {
        if (Clock::now() - f.nearAt < kStreamEnd) return false;
        Refuse(f, "the sender's drive never let it go");
        return true;
    }
    // The host's copy goes where the client's is, in the box, simulating.
    E::SetActorLocation(prop, at);
    E::SetActorSimulatePhysics(prop, true);
    E::SetActorRootPhysicsVelocity(prop, ue_wrap::FVector{0.f, 0.f, 0.f}, ue_wrap::FVector{0.f, 0.f, 0.f});
    const bool ran = RunIntake(chipper, eat, prop, mesh);
    if (ran) ++g_counts.fed;
    UE_LOGI("woodchipper_feed: slot %d fed '%ls' (%ls) to '%ls' -- the host's chipper takes its copy (ran=%d, #%u)", f.slot,
            f.prop.c_str(), R::ClassNameOf(prop).c_str(), f.chipper.c_str(), ran ? 1 : 0, g_counts.fed);
    return true;
}

}  // namespace

void OnClientIntake(void* chipper, void* other) {
    N::Session* s = g_session;
    if (!chipper || !other || other == chipper || !R::IsLive(other) || !ue_wrap::prop::IsDescendantOfProp(other)) return;
    // Only what this client's own hands hold or this client syncs: the host's word decides every other prop.
    void* me = coop::players::Registry::Get().Local();
    E::MainPlayerGrabState gs{};
    const bool held = me && E::ReadMainPlayerGrabState(me, gs) && gs.grabbingActor == other;
    if (!held && !coop::prop_syncer::AuthorsActorHere(other)) return;
    float fuel = 0.f;
    bool active = false;
    if (!W::Read(chipper, fuel, active) || !active || !(fuel > 0.f)) return;  // the copy shows the host's state
    const std::wstring chipKey = W::KeyOf(chipper), propKey = KeyOf(other);
    if (chipKey.empty() || propKey.empty() || chipKey.size() >= sizeof(N::WireKey::data)) return;
    const Clock::time_point now = Clock::now();
    auto it = g_lastAsk.find(propKey);
    if (it != g_lastAsk.end() && now - it->second < kAskGap) return;
    g_lastAsk[propKey] = now;
    if (!s || !s->connected() || s->role() != N::Role::Client) return;
    uint8_t buf[sizeof(N::WoodchipperPayload) + sizeof(N::WoodchipperFeedTail)] = {};
    N::WoodchipperPayload p{};
    N::WireKeyFromString(chipKey, p.key);
    p.op = N::kWoodchipperOpFeed;
    N::WoodchipperFeedTail t{};
    N::WireKeyFromString(propKey, t.prop);
    std::memcpy(buf, &p, sizeof(p));
    std::memcpy(buf + sizeof(p), &t, sizeof(t));
    if (!s->SendReliable(N::ReliableKind::WoodchipperIntent, buf, static_cast<int>(sizeof(buf)))) return;
    ++g_counts.asked;
    // The chipper takes it out of the hand, as single player's eats it from a held grab.
    if (held) E::ReleaseMainPlayerGrabIfHolding(me, other);
    UE_LOGI("woodchipper_feed: this player fed '%ls' (%ls) to '%ls'%s -- the host's chipper takes it (#%u)", propKey.c_str(),
            R::ClassNameOf(other).c_str(), chipKey.c_str(), held ? ", let go of it" : "", g_counts.asked);
}

void OnFeed(int senderSlot, const N::WoodchipperPayload& p, const N::WoodchipperFeedTail& tail) {
    if (senderSlot < 1 || senderSlot >= static_cast<int>(N::kMaxPeers) || g_inbox.size() >= kQueueCap) return;
    Feed f{senderSlot, N::StringFromWireKey(p.key), N::StringFromWireKey(tail.prop), Clock::now()};
    if (f.chipper.empty() || f.prop.empty()) return;
    g_inbox.push_back(std::move(f));
}

void Tick(N::Session& s) {
    g_session = &s;
    if (s.role() != N::Role::Host || g_inbox.empty()) return;
    std::vector<Feed> inbox;
    inbox.swap(g_inbox);
    for (Feed& f : inbox)
        if (!Serve(s, f)) g_inbox.push_back(std::move(f));
}

void OnDisconnect() {
    if (g_counts.asked || g_counts.fed || g_counts.refused)
        UE_LOGI("woodchipper_feed: session end -- client asked=%u; host fed=%u refused=%u", g_counts.asked, g_counts.fed,
                g_counts.refused);
    g_counts = Counts{};
    g_inbox.clear();
    g_lastAsk.clear();
    g_session = nullptr;
}

Counts GetCounts() { return g_counts; }

}  // namespace coop::woodchipper_feed
