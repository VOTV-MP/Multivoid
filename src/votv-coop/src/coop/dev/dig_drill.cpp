// coop/dev/dig_drill.cpp -- see coop/dev/dig_drill.h.

#include "coop/dev/dig_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_hand.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/active_drive.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/dig_tool.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>
#include <string>

namespace coop::dev::dig_drill {
namespace {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
using coop::rig_verdict::Outcome;

constexpr const char* kTag = "dig-drill";
constexpr uint8_t  kWoodChips = 3;         // enum_chipPileType's wood: its scrap is a trWood_ item
constexpr float    kBesideUU = 120.f;      // the pile's offset from the client's body, inside a dig's reach
constexpr float    kFloorUU = 85.f;        // a standing body's capsule centre above its feet
constexpr float    kFindUU = 400.f;        // the client looks for the host's pile within the dig's reach
constexpr float    kItemUU = 50.f;         // the scrap spawns 25 cm above the pile and lands within a few cm
constexpr uint64_t kSpawnAfterMs = 6000;   // host: after the client's world-ready
constexpr uint64_t kBodyWaitMs = 30000;    // host: the client's body readable by then
constexpr uint64_t kToolAfterMs = 3000;    // client: after its own world-ready
constexpr uint64_t kDigAfterFoundMs = 3000;  // client: the hand state reaches the host first
constexpr uint64_t kJudgeAfterDigMs = 6000;  // client
constexpr uint64_t kHostJudgeMs = 30000;   // host: after its spawn
constexpr uint64_t kGiveUpMs = 60000;      // client: no pile found by then

enum class Mode { Off, Shovel, Empty };
enum class Step { Wait, Acted, Found, Dug, Done };

Mode ModeNow() {
    static const Mode s = [] {
        const std::string v = coop::config::ResolveEnum(coop::config_registry::rows::dig_drill);
        return v == "shovel" ? Mode::Shovel : v == "empty" ? Mode::Empty : Mode::Off;
    }();
    return s;
}

Step     g_step = Step::Wait;
uint64_t g_atMs = 0;
uint64_t g_readyMs = 0;
void*    g_pile = nullptr;
int32_t  g_pileIdx = -1;
ue_wrap::FVector g_pileLoc{};
// The prop nearest the pile before the dig: a world prop already standing there is not the dig's scrap.
void*    g_before = nullptr;

// What stands where the pile stood: the pile itself, and the nearest prop's name and distance.
struct Scene {
    bool         pile = false;
    bool         item = false;
    std::wstring itemName;
    float        itemDist = -1.f;
};

Scene Look(const ue_wrap::FVector& at) {
    Scene s;
    s.pile = ue_wrap::prop::FindNearestChipPile(at, 30.f) != nullptr;
    const ue_wrap::prop::NearestResult n = ue_wrap::prop::FindNearest(at);
    if (n.prop && n.prop != g_before && n.dist <= kItemUU && !ue_wrap::dig_tool::IsDigTool(n.prop)) {
        s.item = true;
        s.itemName = R::ClassNameOf(n.prop);
        s.itemDist = n.dist;
    }
    return s;
}

// The verdict a scene earns in this mode: a shovel's dig leaves an item and no pile; an empty hand's, the reverse.
bool Holds(const Scene& s) { return ModeNow() == Mode::Shovel ? (!s.pile && s.item) : (s.pile && !s.item); }

void TickHost(coop::net::Session& session, uint64_t now) {
    if (g_step == Step::Wait) {
        if (!session.IsSlotWorldReady(1)) return;
        if (!g_readyMs) g_readyMs = now;
        if (now - g_readyMs < kSpawnAfterMs) return;
        coop::RemotePlayer* puppet = coop::players::Registry::Get().Puppet(1);
        ue_wrap::FVector body{};
        if (!puppet || !puppet->valid() || !E::TryGetActorLocation(puppet->GetActor(), body)) {
            if (now - g_readyMs < kBodyWaitMs) return;  // the puppet follows the world-ready by a moment
            g_step = Step::Done;
            coop::rig_verdict::Say(kTag, Outcome::Invalid, "host: the client's body could not be read");
            return;
        }
        void* cls = R::FindClass(L"actorChipPile_C");
        const ue_wrap::FVector at{body.X + kBesideUU, body.Y, body.Z - kFloorUU};
        const ue_wrap::FRotator rot{};
        void* pile = cls ? E::BeginDeferredSpawn(cls, at, rot) : nullptr;
        if (pile) ue_wrap::prop::SetChipType(pile, kWoodChips);
        if (!pile || !E::FinishDeferredSpawn(pile, at, rot)) {
            g_step = Step::Done;
            coop::rig_verdict::Say(kTag, Outcome::Invalid, "host: the chip pile could not be spawned");
            return;
        }
        g_pileLoc = at;
        g_before = ue_wrap::prop::FindNearest(at).prop;
        g_step = Step::Acted;
        g_atMs = now;
        UE_LOGI("[DIG-DRILL] host spawned a wood chip pile at (%.0f,%.0f,%.0f) beside the client", at.X, at.Y, at.Z);
        return;
    }
    if (g_step != Step::Acted || now - g_atMs < kHostJudgeMs) return;
    g_step = Step::Done;
    const Scene s = Look(g_pileLoc);
    UE_LOGI("[DIG-DRILL] host reads: pile=%d item=%d (%ls at %.0f cm)", s.pile ? 1 : 0, s.item ? 1 : 0,
            s.itemName.c_str(), s.itemDist);
    if (Holds(s))
        coop::rig_verdict::Say(kTag, Outcome::Pass, "host: %s", ModeNow() == Mode::Shovel
                                   ? "the client's dig was the host's: the pile is gone and its scrap lies there"
                                   : "the empty-handed dig was refused: the pile stands and no scrap lies there");
    else
        coop::rig_verdict::Say(kTag, Outcome::Fail, "host: pile=%d item=%d after the client's dig", s.pile ? 1 : 0,
                               s.item ? 1 : 0);
}

void TickClient(uint64_t now) {
    void* local = coop::players::Registry::Get().Local();
    if (g_step == Step::Wait) {
        if (!coop::net_pump::HasAnnouncedWorldReady() || !local) return;
        if (!g_readyMs) g_readyMs = now;
        if (now - g_readyMs < kToolAfterMs) return;
        if (ModeNow() == Mode::Shovel && !coop::dev::drill_hand::TakeIntoHandNow(L"shovel", L"prop_shovel_C")) {
            g_step = Step::Done;
            coop::rig_verdict::Say(kTag, Outcome::Invalid, "client: a shovel could not be taken into the hand");
            return;
        }
        UE_LOGI("[DIG-DRILL] client hand holds '%ls'", ue_wrap::inventory::ReadHeldItemName().c_str());
        g_step = Step::Acted;
        g_atMs = now;
        return;
    }
    if (g_step == Step::Acted) {
        ue_wrap::FVector me{};
        void* pile = E::TryGetActorLocation(local, me) ? ue_wrap::prop::FindNearestChipPile(me, kFindUU) : nullptr;
        if (!pile) {
            if (now - g_atMs >= kGiveUpMs) {
                g_step = Step::Done;
                coop::rig_verdict::Say(kTag, Outcome::Invalid, "client: no chip pile came within reach");
            }
            return;
        }
        if (!E::TryGetActorLocation(pile, g_pileLoc)) return;
        g_pile = pile;
        g_pileIdx = R::InternalIndexOf(pile);
        g_before = ue_wrap::prop::FindNearest(g_pileLoc).prop;
        g_step = Step::Found;
        g_atMs = now;
        UE_LOGI("[DIG-DRILL] client found the pile at (%.0f,%.0f,%.0f)", g_pileLoc.X, g_pileLoc.Y, g_pileLoc.Z);
        return;
    }
    if (g_step == Step::Found) {
        if (now - g_atMs < kDigAfterFoundMs) return;
        if (!R::IsLiveByIndex(g_pile, g_pileIdx)) {
            g_step = Step::Done;
            coop::rig_verdict::Say(kTag, Outcome::Invalid, "client: the pile was gone before the dig");
            return;
        }
        const bool ran = ue_wrap::dig_tool::TurnIntoScrap(g_pile);
        UE_LOGI("[DIG-DRILL] client dug the pile (the call %s)", ran ? "dispatched" : "did not dispatch");
        g_step = Step::Dug;
        g_atMs = now;
        return;
    }
    if (g_step != Step::Dug || now - g_atMs < kJudgeAfterDigMs) return;
    g_step = Step::Done;
    const Scene s = Look(g_pileLoc);
    UE_LOGI("[DIG-DRILL] client reads: pile=%d item=%d (%ls at %.0f cm)", s.pile ? 1 : 0, s.item ? 1 : 0,
            s.itemName.c_str(), s.itemDist);
    if (!Holds(s))
        coop::rig_verdict::Say(kTag, Outcome::Fail, "client: pile=%d item=%d after its own dig", s.pile ? 1 : 0,
                               s.item ? 1 : 0);
}

}  // namespace

void Tick(coop::net::Session& session) {
    if (ModeNow() == Mode::Off || g_step == Step::Done || !session.running()) return;
    const uint64_t now = coop::active_drive::NowMs();
    if (session.role() == coop::net::Role::Host) TickHost(session, now);
    else TickClient(now);
}

}  // namespace coop::dev::dig_drill
