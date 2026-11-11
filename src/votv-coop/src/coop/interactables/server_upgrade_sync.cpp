// coop/interactables/server_upgrade_sync.cpp -- see coop/interactables/server_upgrade_sync.h.

#include "coop/interactables/server_upgrade_sync.h"

#include "coop/element/element.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_lifecycle.h"  // DestroyLocalProp (echo-suppressed)

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/serverbox.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <vector>

namespace coop::server_upgrade_sync {
namespace {

namespace SB = ue_wrap::serverbox;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;
using coop::net::ServerUpgradeStatePayload;

constexpr uint8_t kOpInstall   = 0;
constexpr uint8_t kOpTakeOut   = 1;
constexpr uint8_t kOpCanonical = 2;
constexpr uint8_t kOpDeny      = 3;

UE_FACT_FN(kRowPlayerUsedOn, "server_upgrade_sync", L"serverBox_C", L"playerUsedOn", Declared);
UE_FACT_FN(kRowActionOptionIndex, "server_upgrade_sync", L"serverBox_C", L"actionOptionIndex", Declared);
constexpr int kTagInstall = 0x53555049;  // 'SUPI'
constexpr int kTagTakeOut = 0x5355504F;  // 'SUPO'

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false;

// A canonical that arrived before the servers resolved (a joiner's world-ready racing its load) waits here.
ServerUpgradeStatePayload g_parked{};
bool g_haveParked = false;
constexpr auto kParkedRetry = std::chrono::milliseconds(1000);
Clock::time_point g_nextParkedTry{};

// HOST: take-outs it refused, by sender, read by the client birth author to reap the prop the take-out
// handed over before its refusal landed. Bounded; the oldest record gives way.
struct DenyRec { uint8_t sender = 0xFF; Clock::time_point until{}; };
constexpr int kDenyRecs = 8;
constexpr auto kDenyTtl = std::chrono::seconds(10);
DenyRec g_denies[kDenyRecs];

uint64_t g_adopted = 0;  // canonicals written into this peer's boxes
HostCounts g_hostCounts{};  // [dev] client ops this host applied and refused
bool g_debugRefuse = false;  // [dev] the host refuses every client op
bool g_saidCap = false;      // a server list past the wire's cap, said once

// CLIENT: the upgrade this peer's last take-out handed over, and the box it came from, so a refusal of that
// take-out removes exactly it.
ue_wrap::CachedObjRef g_handed;
int32_t g_handedBox = -1;

// The actor in the local player's hand, or null.
void* HeldActor() {
    void* pawn = coop::players::Registry::Get().Local();
    ue_wrap::engine::MainPlayerGrabState gs{};
    return (pawn && ue_wrap::engine::ReadMainPlayerGrabState(pawn, gs)) ? gs.mainHand : nullptr;
}

// Every box's level in servers[] order, capped at the wire's box count. False before the servers resolve.
bool ReadLevels(ServerUpgradeStatePayload& p) {
    std::vector<void*> servers;
    if (SB::ReadServers(servers) == 0) return false;
    const size_t n = servers.size() < static_cast<size_t>(coop::net::kServerUpgradeBoxes)
                         ? servers.size() : static_cast<size_t>(coop::net::kServerUpgradeBoxes);
    if (n < servers.size() && !g_saidCap) {
        g_saidCap = true;
        UE_LOGW("server_upgrade: %zu servers, past the wire's %d -- the ones after it are not carried", servers.size(),
                coop::net::kServerUpgradeBoxes);
    }
    p.count = static_cast<uint8_t>(n);
    for (size_t i = 0; i < n; ++i) {
        int32_t lvl = 0;
        if (!SB::ReadUpgrades(servers[i], lvl)) return false;
        p.levels[i] = static_cast<uint8_t>(lvl < 0 ? 0 : (lvl > SB::kMaxUpgrades ? SB::kMaxUpgrades : lvl));
    }
    return true;
}

void HostBroadcastCanonical(coop::net::Session* s, int onlySlot = -1) {
    ServerUpgradeStatePayload p{};
    p.op = kOpCanonical;
    if (!ReadLevels(p)) return;
    if (onlySlot >= 0)
        s->SendReliableToSlot(onlySlot, coop::net::ReliableKind::ServerUpgradeState, &p, sizeof(p));
    else
        s->SendReliable(coop::net::ReliableKind::ServerUpgradeState, &p, sizeof(p));
}

// A refused op goes back to its author with the canonical behind it: the author's box left the canonical
// when its verb ran, and adopting it brings the two together again. MTA answers a refused element-data
// change the same way, with the server's value to that player alone
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2779, Packet_CustomData).
void SendDeny(coop::net::Session* s, uint8_t author, uint8_t refused, uint8_t box) {
    if (author == 0 || author >= coop::net::kMaxPeers) return;
    ServerUpgradeStatePayload d{};
    d.op = kOpDeny;
    d.refused = refused;
    d.box = box;
    s->SendReliableToSlot(author, coop::net::ReliableKind::ServerUpgradeState, &d, sizeof(d));
    HostBroadcastCanonical(s, author);
}

bool AdoptCanonical(const ServerUpgradeStatePayload& p) {
    std::vector<void*> servers;
    if (SB::ReadServers(servers) == 0) return false;
    size_t n = p.count;
    if (n > servers.size()) n = servers.size();
    if (n > static_cast<size_t>(coop::net::kServerUpgradeBoxes)) n = coop::net::kServerUpgradeBoxes;
    int changed = 0;
    for (size_t i = 0; i < n; ++i) {
        int32_t cur = 0;
        if (!SB::ReadUpgrades(servers[i], cur) || cur == p.levels[i]) continue;
        if (SB::WriteUpgrades(servers[i], p.levels[i])) ++changed;
    }
    ++g_adopted;
    if (changed)
        UE_LOGI("server_upgrade: canonical adopted -- %d box(es) moved (n=%llu)", changed,
                static_cast<unsigned long long>(g_adopted));
    return true;
}

// The two verbs, each body's entry reading its box's level and its exit sending what it changed. Neither
// runs the other, but a body can nest inside another watched body of the same box, so after a send every
// enclosing body's snapshot of that box moves to the level as sent, and a change is sent once.
struct InFlight {
    int     depth;  // the body's place on the gate's chain of watched bodies
    void*   stack;
    void*   box;
    int32_t before;
    void*   heldBefore;  // the local player's hand at the entry
};
std::vector<InFlight> g_inFlight;  // game thread only

// The gate's own chain is the scope: an entry at `depth` or deeper belongs to a body that ended without
// its exit (another watcher's Cancel, or a fault the firewall absorbed, skips every exit).
void DropFrom(int depth) {
    while (!g_inFlight.empty() && g_inFlight.back().depth >= depth) g_inFlight.pop_back();
}

sg::Verdict OnVerbPre(const sg::Call& call) {
    DropFrom(call.depth);
    int32_t lvl = 0;
    if (call.object && SB::ReadUpgrades(call.object, lvl))
        g_inFlight.push_back({call.depth, call.stack, call.object, lvl, HeldActor()});
    return sg::Verdict::Run;
}

void OnVerbPost(const sg::Call& call) {
    DropFrom(call.depth + 1);
    if (g_inFlight.empty() || g_inFlight.back().depth != call.depth || g_inFlight.back().stack != call.stack)
        return;  // its entry read no level
    const InFlight f = g_inFlight.back();
    g_inFlight.pop_back();
    int32_t live = 0;
    if (!SB::ReadUpgrades(f.box, live) || live == f.before) return;
    for (InFlight& outer : g_inFlight)
        if (outer.box == f.box) outer.before = live;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    const int32_t idx = SB::IndexOf(f.box);
    if (s->role() == coop::net::Role::Host) {
        UE_LOGI("server_upgrade: host box=%d %d -> %d at its verb -- canonical broadcast", idx, f.before, live);
        HostBroadcastCanonical(s);
        return;
    }
    if (idx < 0 || idx >= coop::net::kServerUpgradeBoxes) {
        UE_LOGW("server_upgrade: a box moved %d -> %d but is not in the first %d of the server list -- not sent",
                f.before, live, coop::net::kServerUpgradeBoxes);
        return;
    }
    ServerUpgradeStatePayload p{};
    p.op = live > f.before ? kOpInstall : kOpTakeOut;
    p.box = static_cast<uint8_t>(idx);
    if (p.op == kOpTakeOut) {
        // The take-out's "quick hold object" puts a new upgrade in a free hotbar slot and selects it, so it stands
        // in the main hand; with no free slot the game destroys it.
        void* held = HeldActor();
        g_handed.Reset();
        if (held && held != f.heldBefore && SB::IsUpgradeClass(R::ClassOf(held))) g_handed.Set(held);
        g_handedBox = idx;
    }
    s->SendReliableToSlot(0, coop::net::ReliableKind::ServerUpgradeState, &p, sizeof(p));
    UE_LOGI("server_upgrade: local %s box=%d (%d -> %d) -- op to host", p.op == kOpInstall ? "INSTALL" : "TAKE-OUT",
            idx, f.before, live);
}

void RecordDeny(uint8_t sender) {
    const auto now = Clock::now();
    DenyRec* slot = &g_denies[0];
    for (auto& d : g_denies) {
        if (d.sender == 0xFF || now >= d.until) { slot = &d; break; }
        if (d.until < slot->until) slot = &d;  // the bound: the oldest record gives way
    }
    *slot = {sender, now + kDenyTtl};
}

// The upgrade a refused take-out handed this client is a ghost. In the hand it goes as the box's own install
// consumes a held upgrade: its hand actor destroyed, and the game's holdActorDestroy_main takes it from its hotbar
// slot. Once out of the hand (dropped into the world before the refusal landed) the host reaps its birth, and here
// the untracked upgrades in reach of this player go.
void ClientRemoveTakeOutGhost(uint8_t box) {
    void* handed = (g_handedBox == box) ? g_handed.Get() : nullptr;
    g_handed.Reset();
    g_handedBox = -1;
    if (handed && handed == HeldActor()) {
        ue_wrap::engine::DestroyActor(handed);
        UE_LOGW("server_upgrade: take-out at box=%u refused by the host -- the upgrade it handed over removed", box);
        return;
    }
    void* pawn = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    if (!pawn || !ue_wrap::engine::TryGetActorLocation(pawn, at)) return;
    struct Ctx { ue_wrap::FVector at; int swept; } ctx{at, 0};
    SB::ForEachUpgrade([](void* c, void* obj) {
        auto* x = static_cast<Ctx*>(c);
        if (obj == HeldActor()) return;
        if (coop::prop_element_tracker::GetPropElementIdForActor(obj) != coop::element::kInvalidId) return;
        ue_wrap::FVector p{};
        if (!ue_wrap::engine::TryGetActorLocation(obj, p)) return;
        const float dx = p.X - x->at.X, dy = p.Y - x->at.Y, dz = p.Z - x->at.Z;
        if (dx * dx + dy * dy + dz * dz > 500.f * 500.f) return;
        coop::prop_lifecycle::DestroyLocalProp(obj, /*deferred*/ true);
        ++x->swept;
    }, &ctx);
    UE_LOGW("server_upgrade: take-out at box=%u refused by the host -- swept %d untracked upgrade(s) in reach", box,
            ctx.swept);
}

void HostApplyOp(coop::net::Session* s, const ServerUpgradeStatePayload& p, uint8_t senderSlot) {
    std::vector<void*> servers;
    if (SB::ReadServers(servers) == 0) {
        // Nothing to answer with: its author's box stays where its verb left it until the next canonical.
        UE_LOGW("server_upgrade: host op=%u box=%u from slot %u declined (servers unresolved)", p.op, p.box,
                senderSlot);
        return;
    }
    if (p.box >= servers.size()) {
        UE_LOGW("server_upgrade: host op=%u box=%u from slot %u -- no such box, refused", p.op, p.box, senderSlot);
        HostBroadcastCanonical(s, senderSlot);
        return;
    }
    void* box = servers[p.box];
    int32_t lvl = 0;
    if (!SB::ReadUpgrades(box, lvl)) {
        HostBroadcastCanonical(s, senderSlot);
        return;
    }
    if (p.op == kOpInstall) {
        if (lvl >= SB::kMaxUpgrades || g_debugRefuse) {
            // Two peers installed into one box at its last free place and this op came second: refuse and
            // refund the upgrade where its author stands; the host's spawn watcher expresses it to every peer.
            SendDeny(s, senderSlot, p.op, p.box);
            ++g_hostCounts.refused;
            ue_wrap::FVector at{};
            coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(senderSlot);
            void* body = rp ? rp->GetActor() : nullptr;
            bool placed = body && ue_wrap::engine::TryGetActorLocation(body, at);
            if (!placed && ue_wrap::engine::TryGetActorLocation(box, at)) {
                at.Z += 80.f;
                placed = true;
            }
            void* refund = placed ? SB::SpawnUpgradeProp(at) : nullptr;
            UE_LOGW("server_upgrade: install box=%u from slot %u -- %s, denied + refund %s", p.box, senderSlot,
                    g_debugRefuse ? "the refusal probe" : "the box is full", refund ? "spawned" : "NOT spawned (item lost)");
            return;
        }
        ++lvl;
    } else {
        if (lvl <= 0 || g_debugRefuse) {
            // Raced: the box was emptied first. Its author's handed-over prop is a ghost.
            RecordDeny(senderSlot);
            SendDeny(s, senderSlot, p.op, p.box);
            ++g_hostCounts.refused;
            UE_LOGW("server_upgrade: take-out box=%u from slot %u -- %s, deny sent", p.box, senderSlot,
                    g_debugRefuse ? "the refusal probe" : "raced (the box is empty)");
            return;
        }
        --lvl;
    }
    if (!SB::WriteUpgrades(box, lvl)) {
        UE_LOGW("server_upgrade: host write failed -- op=%u box=%u not applied", p.op, p.box);
        HostBroadcastCanonical(s, senderSlot);
        return;
    }
    HostBroadcastCanonical(s);
    if (p.op == kOpInstall) ++g_hostCounts.installs;
    else ++g_hostCounts.takeOuts;
    UE_LOGI("server_upgrade: host applied %s box=%u from slot %u -> %d -- canonical broadcast",
            p.op == kOpInstall ? "install" : "take-out", p.box, senderSlot, lvl);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_watched)
        g_watched = sg::WatchName(kRowPlayerUsedOn, kTagInstall, &OnVerbPre, &OnVerbPost) &&
                    sg::WatchName(kRowActionOptionIndex, kTagTakeOut, &OnVerbPre, &OnVerbPost);
}

void Tick() {
    if (!g_haveParked) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    const auto now = Clock::now();
    if (now < g_nextParkedTry) return;
    g_nextParkedTry = now + kParkedRetry;
    if (!AdoptCanonical(g_parked)) return;
    g_haveParked = false;
    UE_LOGI("server_upgrade: parked canonical applied once the servers resolved");
}

void OnState(const ServerUpgradeStatePayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const bool isHost = (s->role() == coop::net::Role::Host);
    switch (p.op) {
    case kOpCanonical:
        if (isHost) return;  // the host IS the canonical
        if (senderSlot != 0) {
            UE_LOGW("server_upgrade: canonical from non-host slot=%u -- dropping", senderSlot);
            return;
        }
        if (!AdoptCanonical(p)) {
            g_parked = p;
            g_haveParked = true;
            UE_LOGI("server_upgrade: canonical parked (servers unresolved)");
            return;
        }
        g_haveParked = false;  // an older parked canonical is behind this one
        return;
    case kOpDeny:
        if (isHost || senderSlot != 0) return;
        if (p.refused == kOpInstall)
            UE_LOGW("server_upgrade: install box=%u refused by the host -- its refund comes from the host",
                    p.box);
        else
            ClientRemoveTakeOutGhost(p.box);
        return;
    case kOpInstall:
    case kOpTakeOut:
        if (!isHost) {
            UE_LOGW("server_upgrade: op=%u reached a client -- dropping", p.op);
            return;
        }
        HostApplyOp(s, p, senderSlot);
        return;
    default:
        return;
    }
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    HostBroadcastCanonical(s, slot);
}

bool HostShouldReapUpgradeBirth(uint8_t senderSlot) {
    const auto now = Clock::now();
    for (auto& d : g_denies) {
        if (d.sender == senderSlot && now < d.until) {
            d = DenyRec{};  // one reap per refusal
            UE_LOGW("server_upgrade: an upgrade born on slot %u after its take-out was refused -- reaped",
                    senderSlot);
            return true;
        }
    }
    return false;
}

uint64_t CanonicalsAdopted() {
    return g_adopted;
}

HostCounts HostOpCounts() {
    return g_hostCounts;
}

void DebugRefuseOps(bool on) {
    if (on != g_debugRefuse) UE_LOGI("server_upgrade: [dev] the host %s client ops", on ? "refuses" : "applies");
    g_debugRefuse = on;
}

void OnDisconnect() {
    g_inFlight.clear();
    g_handed.Reset();
    g_handedBox = -1;
    g_haveParked = false;
    for (auto& d : g_denies) d = DenyRec{};
    g_adopted = 0;
    g_hostCounts = HostCounts{};
    g_debugRefuse = false;
    g_saidCap = false;
}

}  // namespace coop::server_upgrade_sync
