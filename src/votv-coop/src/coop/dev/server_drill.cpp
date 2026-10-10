// coop/dev/server_drill.cpp -- see coop/dev/server_drill.h.

#include "coop/dev/server_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/interactables/serverbox_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/save/save_transfer.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/serverbox.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace coop::dev::server_drill {
namespace {

namespace SB = ue_wrap::serverbox;
namespace SS = coop::serverbox_sync;
namespace E  = ue_wrap::engine;

constexpr int32_t  kBandType      = 123;    // a repair type no roll makes
constexpr int      kBreakTries    = 8;      // boxes a `$srv` floppy may spare
constexpr float    kReachCm       = 150.f;  // the director's stop at the box's bay
constexpr float    kBayReachCm    = 180.f;  // a route must end this near the bay, flat
constexpr int      kWalkDeadlineS = 300;
constexpr uint64_t kMirrorBoundMs = 60000;   // CLIENT: world-ready to the host's break shown here
constexpr uint64_t kRowBoundMs    = 30000;   // CLIENT: its repair to the host's row showing it
constexpr uint64_t kRepairBoundMs = 600000;  // HOST: its break to the client's repair (the walk included)
constexpr uint64_t kCheckEveryMs  = 250;

enum class HStep : uint8_t { Wait, Break, AwaitRepair, Done };
enum class CStep : uint8_t { Ready, Find, OwnBreak, Walk, Repair, AwaitRow, Done };
HStep    g_host = HStep::Wait;
CStep    g_client = CStep::Ready;
uint64_t g_stepMs = 0;
uint64_t g_nextCheckMs = 0;
int      g_session = 1;
ue_wrap::CachedObjRef g_box;
int32_t  g_boxIdx = -1;
std::shared_ptr<coop::director::BackgroundWalk> g_walk;

const std::string& Mode() {
    static const std::string s = coop::config::ResolveString(::coop::config_registry::rows::server_drill);
    return s;
}
bool Join() { return Mode() == "join"; }
bool Enabled() { return Mode() == "run" || Join(); }
bool Expired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }

void Fail(const char* what) {
    UE_LOGW("[SERVER-DRILL] FAIL in session %d: %s", g_session, what);
    g_host = HStep::Done;
    g_client = CStep::Done;
}
void Abandon(const char* why) {
    UE_LOGW("[SERVER-DRILL] ABANDONED in session %d: %s", g_session, why);
    g_host = HStep::Done;
    g_client = CStep::Done;
}

void HGo(HStep s) { g_host = s; g_stepMs = ::GetTickCount64(); }
void CGo(CStep s) { g_client = s; g_stepMs = ::GetTickCount64(); }

bool Resolved() { return SB::EnsureBreakResolved() && SB::EnsureRepairResolved(); }


// HOST: the first healthy box with no upgrades and a bay (its take-out component) breaks on its own verb.
bool BreakOne() {
    std::vector<void*> servers;
    SB::ReadServers(servers);
    int tries = 0;
    for (size_t i = 0; i < servers.size() && tries < kBreakTries; ++i) {
        void* box = servers[i];
        int32_t lvl = -1;
        if (!box || SB::ReadIsBroken(box) || !SB::ReadUpgrades(box, lvl) || lvl != 0 || !SB::TakeOutComponent(box))
            continue;
        ++tries;
        if (!SB::CallBreakServer(box) || !SB::ReadIsBroken(box)) continue;  // a $srv floppy spared it
        SB::RepairState r;
        if (!SB::ReadRepairState(box, r)) return false;
        r.minigame = kBandType;
        if (!SB::WriteRepairState(box, r)) return false;
        g_box.Set(box);
        g_boxIdx = static_cast<int32_t>(i);
        return true;
    }
    return false;
}

void HostTick(coop::net::Session* s) {
    switch (g_host) {
    case HStep::Wait: {
        if (!s->running() || !Resolved()) return;
        if (!Join()) {
            if (s->AnyWorldReadyPeer()) HGo(HStep::Break);
            return;
        }
        // join: the break in a joiner's window, its world taken and not yet ready.
        for (int i = 1; i < coop::net::kMaxPeers; ++i)
            if (coop::save_transfer::WorldTakenFor(i) && !s->IsSlotWorldReady(i)) HGo(HStep::Break);
        if (g_host == HStep::Wait && s->AnyWorldReadyPeer())
            Abandon("a joiner's world was ready before the host saw its window");
        return;
    }
    case HStep::Break:
        if (!BreakOne()) {
            Abandon("no healthy box with no upgrades broke on the host's own verb");
            return;
        }
        UE_LOGI("[SERVER-DRILL] host (%s): box %d broken, its repair type %d", Mode().c_str(), g_boxIdx, kBandType);
        if (Join()) {
            g_host = HStep::Done;
            return;
        }
        HGo(HStep::AwaitRepair);
        return;
    case HStep::AwaitRepair: {
        void* box = g_box.Get();
        if (!box) {
            Abandon("the host's box died");
            return;
        }
        if (SB::ReadIsBroken(box)) {
            if (Expired(kRepairBoundMs)) Fail("the client's repair never reached the host");
            return;
        }
        UE_LOGI("[SERVER-DRILL] host DONE in session %d (run): box %d fixed by the client's repair (%llu run here)",
                g_session, g_boxIdx, static_cast<unsigned long long>(SS::LaneCounts().repairsRun));
        g_host = HStep::Done;
        return;
    }
    default:
        return;
    }
}

// CLIENT: the box the host broke, by its band type.
void* FindBandBox(int32_t& idx) {
    std::vector<void*> servers;
    SB::ReadServers(servers);
    for (size_t i = 0; i < servers.size(); ++i) {
        SB::RepairState r;
        if (!servers[i] || !SB::ReadIsBroken(servers[i]) || !SB::ReadRepairState(servers[i], r)) continue;
        if (r.minigame == kBandType) {
            idx = static_cast<int32_t>(i);
            return servers[i];
        }
    }
    return nullptr;
}

void* AnotherHealthyBox() {
    std::vector<void*> servers;
    SB::ReadServers(servers);
    for (void* b : servers)
        if (b && b != g_box.Get() && !SB::ReadIsBroken(b)) return b;
    return nullptr;
}

void ClientTick(void* player) {
    const SS::Counts c = SS::LaneCounts();
    switch (g_client) {
    case CStep::Ready:
        if (!coop::net_pump::HasAnnouncedWorldReady() || !Resolved() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        CGo(CStep::Find);
        return;
    case CStep::Find: {
        int32_t idx = -1;
        void* box = FindBandBox(idx);
        if (!box) {
            if (Expired(kMirrorBoundMs)) Fail("the host's break and its repair type never reached this client");
            return;
        }
        g_box.Set(box);
        g_boxIdx = idx;
        if (Join()) {
            UE_LOGI("[SERVER-DRILL] client DONE in session %d (join): box %d broken with the host's repair type, "
                    "carried by the connect snapshot -- PASS", g_session, idx);
            g_client = CStep::Done;
            return;
        }
        UE_LOGI("[SERVER-DRILL] client: box %d broken here with the host's repair type", idx);
        CGo(CStep::OwnBreak);
        return;
    }
    case CStep::OwnBreak: {
        void* other = AnotherHealthyBox();
        if (!other) {
            Abandon("no healthy box to try this client's own break on");
            return;
        }
        const uint64_t before = c.refusedBreaks;
        SB::CallBreakServer(other);
        if (SS::LaneCounts().refusedBreaks != before + 1 || SB::ReadIsBroken(other)) {
            Fail("this client's own breakServer was not refused");
            return;
        }
        ue_wrap::FVector at{}, bay{};
        void* comp = SB::TakeOutComponent(g_box.Get());
        if (!comp || !E::TryGetActorLocation(player, at)) {
            Abandon("the box's bay or the player's place is unread");
            return;
        }
        bay = E::GetComponentLocation(comp);
        if (!coop::director::RouteInLegs(player, at, bay, kBayReachCm)) {
            Abandon("no NavMesh route reaches the box's bay");
            return;
        }
        UE_LOGI("[SERVER-DRILL] client: its own breakServer refused; walking to box %d's bay", g_boxIdx);
        g_walk = coop::director::StartBackgroundWalk(bay, kReachCm, kWalkDeadlineS);
        CGo(CStep::Walk);
        return;
    }
    case CStep::Walk: {
        const int st = g_walk ? g_walk->state.load() : 2;
        if (st == 0) return;
        if (st == 2) {
            Abandon("the walk to the box did not arrive");
            return;
        }
        CGo(CStep::Repair);
        return;
    }
    case CStep::Repair: {
        void* box = g_box.Get();
        const uint64_t sent = c.repairsSent;
        if (!box || !SB::CallRepairEnd(box, true)) {
            Abandon("the repair widget's end did not run");
            return;
        }
        if (SS::LaneCounts().repairsSent != sent + 1 || !SB::ReadIsBroken(box)) {
            Fail("the repair ran here instead of reaching the host");
            return;
        }
        UE_LOGI("[SERVER-DRILL] client: the repair widget's fix refused here and sent to the host");
        CGo(CStep::AwaitRow);
        return;
    }
    case CStep::AwaitRow: {
        void* box = g_box.Get();
        if (box && SB::ReadIsBroken(box)) {
            if (Expired(kRowBoundMs)) Fail("the host's row never showed the box fixed");
            return;
        }
        UE_LOGI("[SERVER-DRILL] client DONE in session %d (run): the break and its type came from the host, its own "
                "break was refused, and its repair ran on the host and came back -- PASS", g_session);
        g_client = CStep::Done;
        return;
    }
    default:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (!Enabled() || !s) return;
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextCheckMs) return;
    g_nextCheckMs = now + kCheckEveryMs;
    if (s->role() == coop::net::Role::Host) {
        if (g_host != HStep::Done) HostTick(s);
        return;
    }
    if (!s->connected() || g_client == CStep::Done) return;
    void* player = coop::players::Registry::Get().Local();
    if (player) ClientTick(player);
}

void OnDisconnect() {
    if (!Enabled()) return;
    ++g_session;
    g_host = HStep::Wait;
    g_client = CStep::Ready;
    g_stepMs = g_nextCheckMs = 0;
    g_box.Reset();
    g_boxIdx = -1;
    g_walk.reset();
}

}  // namespace coop::dev::server_drill
