// coop/dev/hostile_drill_atv_host.cpp -- the atv_ family's judge: see coop/dev/hostile_drill_atv.h. The host waits for the
// client's ready line, then runs the arm's phases: it spawns the ATVs the phase needs beside the client's puppet (through
// the ATV health drill's own spawn), says go with their keys, and judges the phase from the client's sent line by
// accounting: the counter deltas since the phase's baseline equal the phase's table, and a counter the table does not
// name stays at 0.

#include "hostile_drill_atv_detail.h"  // co-located private header (src tree, not include/)

#include "atv_seat.h"           // TrySit (co-located private header)
#include "atv_spawn_near.h"     // SpawnAtNav, KeyOf (co-located private header)
#include "explosion_drill_source.h"  // Guard, Unguard (co-located private header)

#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_hand.h"
#include "coop/interactables/atv_authority.h"
#include "coop/interactables/atv_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/world/explosion_recipe.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>

namespace coop::dev::hostile_drill::atv::detail {
namespace {

namespace A  = ue_wrap::atv;
namespace AU = coop::atv_authority;
namespace DC = coop::dev::drill_chat;
namespace E  = ue_wrap::engine;
namespace ER = coop::explosion_request;
namespace SN = coop::dev::atv_spawn_near;
namespace XD = coop::dev::explosion_drill::detail;
using coop::atv_sync::TableView;

constexpr float kNearCm = 600.f;       // an ATV "in reach": inside the host's 400 + bounds + 600 allowance
constexpr float kFarCm = 2000.f;       // and one out of it
constexpr int   kFan = 8;              // the directions a spawn tries
constexpr float kPoseSlackCm = 100.f;  // the host copy of an ATV nobody syncs may settle this far, no more
constexpr size_t kAtvSlot = static_cast<size_t>(coop::explosion_recipe::RequestKind::Atv) - 1;

enum class HStep : uint8_t { Begin, WaitClient, Prep, AwaitSent, Judge, Done };

// The counters a phase is judged on, read at its baseline and at each judge.
struct Snap {
    AU::Counters au{};
    ER::Counters er{};
};
Snap Take() { return Snap{AU::ReadCounters(), ER::GetCounters()}; }

struct Host {
    HStep                 step = HStep::Begin;
    int                   phase = 1;
    Clock::time_point     armAt{}, phaseAt{}, sentAt{}, judgedAt{};   // judgedAt: the last phase passed
    const char*           waiting = "slot 1's world-ready";
    bool                  clientReady = false, guarded = false, emptied = false, sat = false, sentSeen = false;
    ue_wrap::CachedObjRef atv[kMaxAtvs];
    std::wstring          key[kMaxAtvs];
    bool                  spawned[kMaxAtvs] = {};
    bool                  keyed[kMaxAtvs] = {};
    coop::dev::atv_seat::SitTry sit;
    Snap                  base{};
    ue_wrap::FVector      poseBase{};
    bool                  havePose = false;
    uint16_t              playerNo = 0;   // slot 1's occupant when the arm began
};
Host g;

// An arm's end: the verdict, then what the host held is put back (its seat, the guard).
void Finish(Arm a, const char* what, const std::string& why) {
    Verdict(a, what, why);
    if (g.sat) {
        if (void* atv = g.atv[0].Get()) A::Dismount(atv);
        g.sat = false;
    }
    if (g.guarded) {
        XD::Unguard();
        g.guarded = false;
    }
    g.step = HStep::Done;
}

// Slot 1 is world-ready with its bracket closed and a posed puppet: the host can place the sender.
bool PuppetReady(coop::net::Session* s) {
    if (!s->IsSlotWorldReady(kSlot)) { g.waiting = "slot 1 to be world-ready"; return false; }
    if (!coop::prop_snapshot::IsBracketClosed(kSlot) || coop::join_window_baseline::IsLateWindowOpen(kSlot)) {
        g.waiting = "slot 1's join window to close";
        return false;
    }
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(kSlot);
    if (!rp.valid() || !rp.HasPose()) { g.waiting = "a posed puppet for slot 1"; return false; }
    return true;
}

bool Bounded(Arm a) {
    if (Clock::now() - g.phaseAt <= kFailBound) return false;
    Finish(a, "INCONCLUSIVE", std::string("the precondition never held: waiting for ") + g.waiting);
    return true;
}

// ---- the host's half before a go line ----

// ATV `sp.idx`, born `sp.far ? 2000 : 600` cm from the client's puppet on the first fan direction that projects to the
// NavMesh. Null on success, else why not.
const char* SpawnBeside(const Spawn& sp) {
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(kSlot);
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(rp.GetActor(), at)) return "the puppet's place could not be read";
    void* me = coop::players::Registry::Get().Local();
    if (!me) return "the host's player is not there";
    const float r = sp.far ? kFarCm : kNearCm;
    for (int i = 0; i < kFan; ++i) {
        const float yaw = 6.2831853f * static_cast<float>((sp.dir + i) % kFan) / static_cast<float>(kFan);
        const ue_wrap::FVector point{at.X + std::cos(yaw) * r, at.Y + std::sin(yaw) * r, at.Z + SN::kLiftCm};
        SN::Spawned out;
        if (SN::SpawnAtNav(me, point, out)) {
            g.atv[sp.idx].Set(out.actor);
            return nullptr;
        }
        if (!out.noNav) return out.what;
    }
    return "no NavMesh point near the puppet in any direction";
}

// The host's own seat in ATV 0: the hand emptied, then the game's seat verb, then the table naming the host.
bool HostSits(Arm a) {
    void* player = coop::players::Registry::Get().Local();
    void* atv = g.atv[0].Get();
    if (!player || !atv) {
        Finish(a, "INCONCLUSIVE", "the host's player or the ATV is gone before the sit");
        return false;
    }
    if (!g.emptied) {
        if (!coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr)) {
            Finish(a, "INCONCLUSIVE", "the host's hand could not be emptied for the seat verb");
            return false;
        }
        g.emptied = true;
    }
    if (!coop::dev::atv_seat::TrySit(player, atv, g.sit)) { g.waiting = "the host's seat to be granted"; return false; }
    TableView t;
    if (!coop::atv_sync::ReadTable(g.key[0], t) || t.occupantSlot != 0 || t.syncerSlot != 0) {
        Finish(a, "INCONCLUSIVE", "the host's sit did not take: the table does not name the host occupant and syncer");
        return false;
    }
    g.sat = true;
    return true;
}

// The phase's spawns, their keys in the table, and the host's seat. True once the phase can say go.
bool PrepDone(Arm a, const Phase& ph) {
    for (int i = 0; i < ph.prep.nSpawns; ++i) {
        const Spawn& sp = ph.prep.spawns[i];
        if (g.spawned[sp.idx]) continue;
        if (const char* why = SpawnBeside(sp)) {
            Finish(a, "INCONCLUSIVE", std::string("an ATV: ") + why);
            return false;
        }
        g.spawned[sp.idx] = true;
    }
    for (int i = 0; i < ph.prep.nSpawns; ++i) {
        const int idx = ph.prep.spawns[i].idx;
        if (g.keyed[idx]) continue;
        void* atv = g.atv[idx].Get();
        if (!atv) {
            Finish(a, "INCONCLUSIVE", "a spawned ATV is gone before its key");
            return false;
        }
        const char* why = nullptr;
        const SN::Key k = SN::KeyOf(atv, &g.key[idx], &why);
        if (k == SN::Key::Bad) {
            Finish(a, "INCONCLUSIVE", std::string("a spawned ATV's key cannot ride a chat line: ") + why);
            return false;
        }
        if (k == SN::Key::Wait) { g.waiting = "a spawned ATV to enter the host's table"; return false; }
        g.keyed[idx] = true;
    }
    // The settle is measured from the previous phase's verdict, which follows the message it waits out.
    if (ph.prep.settleMs && Clock::now() - g.judgedAt < std::chrono::milliseconds(ph.prep.settleMs)) {
        g.waiting = "the host's seat bucket to refill";
        return false;
    }
    return !ph.prep.hostSits || g.sat || HostSits(a);
}

// "go <n> <key> <key> ...": every ATV spawned so far, in spawn order.
std::string GoLine(Arm a, int n) {
    std::string line = GoPrefix(a, n);
    for (int i = 0; i < kMaxAtvs; ++i) {
        if (!g.keyed[i]) continue;
        for (const wchar_t c : g.key[i]) line += static_cast<char>(c);
        line += ' ';
    }
    return line;
}

// ---- the judge ----

// One counter's delta against the table: over is a FAIL at once, under waits.
struct Account {
    std::string over, under, seen;
    // The strings are built only for a counter that moved or was wanted to.
    void Cmp(const char* lane, const char* name, uint64_t got, uint64_t want, uint64_t slack = 0) {
        if (!got && !want) return;
        const std::string nm = std::string(lane) + "." + name;
        const std::string pair = " " + nm + " +" + std::to_string(got) + " (wanted +" + std::to_string(want) + ");";
        if (got > want + slack) over += pair;
        else if (got < want) under += pair;
        if (got) seen += " " + nm + "=" + std::to_string(got);
    }
};

Account Judged(const Expect& w, const Snap& now) {
    static const char* const kKinds[4] = {"accepted_projectile", "accepted_hand", "accepted_self", "accepted_atv"};
    Account acc;
    for (unsigned r = 1; r < 16; ++r)
        acc.Cmp("atv", AtvReasonName(r), now.au.refused[r] - g.base.au.refused[r], w.refused[r]);
    acc.Cmp("atv", "admitted", now.au.admitted - g.base.au.admitted, w.admitted);
    acc.Cmp("atv", "not_syncer", now.au.notSyncer - g.base.au.notSyncer, w.notSyncer);
    acc.Cmp("atv", "late_syncer", now.au.lateSyncer - g.base.au.lateSyncer, 0);
    acc.Cmp("atv", "stream_paced", now.au.streamPaced - g.base.au.streamPaced, 0);
    for (size_t r = 0; r < kExplReasons; ++r)
        acc.Cmp("expl", ER::RefusalName(static_cast<ER::Refusal>(r)), now.er.refused[r] - g.base.er.refused[r],
                w.explRefused[r], r == static_cast<size_t>(ER::Refusal::Blown) ? w.blownSlack : 0);
    for (size_t k = 0; k < 4; ++k)
        acc.Cmp("expl", kKinds[k], now.er.requestsAccepted[k] - g.base.er.requestsAccepted[k],
                k == kAtvSlot ? w.explAccepted : 0);
    return acc;
}

// The forged blocks must leave the host copy of an ATV nobody syncs where it stood.
std::string PoseMoved() {
    void* atv = g.atv[0].Get();
    ue_wrap::FVector at{}, from = g.poseBase;
    ue_wrap::FRotator rot{};
    if (!atv || !A::GetRootTransform(atv, at, rot)) return "the host copy of the ATV is gone or does not read";
    const float dx = at.X - from.X, dy = at.Y - from.Y, dz = at.Z - from.Z;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d <= kPoseSlackCm) return {};
    char buf[96];
    std::snprintf(buf, sizeof(buf), "the host copy moved %.0f cm under blocks it refused", d);
    return buf;
}

void Judge(coop::net::Session* s, Arm a) {
    const Phase& ph = PhaseOf(a, g.phase);
    if (!s->HasPeerConn(kSlot)) return Finish(a, "INCONCLUSIVE", "the judged slot left before the verdict");
    if (coop::roster_ledger::Get(kSlot).playerNo != g.playerNo)
        return Finish(a, "INCONCLUSIVE", "slot-replaced (the judged slot's occupant changed)");
    const Account acc = Judged(ph.expect, Take());
    // A phase that needs the host seated judges nothing once the seat is gone: the game can throw the host out of the
    // ATV (a ragdoll from the spawn's landing was measured), and a takeover of an empty seat is then rightly
    // admitted. That is a rig loss, not a hole: the verdict is INCONCLUSIVE (the table is read as it stands).
    if (ph.prep.hostSits && g.sat && (!acc.over.empty() || !acc.under.empty())) {
        TableView t;
        if (!coop::atv_sync::ReadTable(g.key[0], t) || t.occupantSlot != 0)
            return Finish(a, "INCONCLUSIVE", "the host's seat was lost before the verdict (the game ejected the host): "
                                             "the takeovers met an empty seat");
    }
    const std::string head = "phase " + std::to_string(g.phase) + " of " + std::to_string(PhaseCount(a)) + ":";
    if (!acc.over.empty())
        return Finish(a, "FAIL", head + " more than the table:" + acc.over + (acc.under.empty() ? "" : " short:" + acc.under));
    if (!acc.under.empty()) {
        if (Clock::now() - g.sentAt <= kFailBound) return;
        return Finish(a, "FAIL", head + " timeout, short:" + acc.under + " got" + acc.seen);
    }
    if (a == Arm::StateForge) {
        const std::string moved = PoseMoved();
        if (!moved.empty()) return Finish(a, "FAIL", head + " " + moved);
    }
    UE_LOGI("[HOSTILE-DRILL] %s %s accounted as the table:%s", ArmName(a), head.c_str(),
            acc.seen.empty() ? " none" : acc.seen.c_str());
    if (g.phase >= PhaseCount(a))
        return Finish(a, "PASS", "all " + std::to_string(PhaseCount(a)) + " phases accounted, as the arm's table");
    ++g.phase;
    g.phaseAt = g.judgedAt = Clock::now();
    g.sentSeen = false;
    g.waiting = "the next phase's ATVs";
    g.step = HStep::Prep;
}

void AwaitSent(Arm a) {
    if (DC::PollDue()) g.sentSeen = g.sentSeen || DC::ChatHasLive(SentLine(a, g.phase));
    if (g.sentSeen) {
        g.sentAt = Clock::now();
        g.step = HStep::Judge;
        return;
    }
    if (Clock::now() - g.phaseAt > kFailBound)
        Finish(a, "INCONCLUSIVE", "the client never said phase " + std::to_string(g.phase) + " was sent");
}

// The baseline, the go line, and the wait for the client's sent line.
void SayGo(Arm a) {
    g.base = Take();
    g.havePose = false;
    if (a == Arm::StateForge) {
        ue_wrap::FRotator rot{};
        void* atv = g.atv[0].Get();
        g.havePose = atv && A::GetRootTransform(atv, g.poseBase, rot);
        if (!g.havePose) return Finish(a, "INCONCLUSIVE", "the host copy of the ATV does not read before the forgeries");
    }
    DC::Say(GoLine(a, g.phase));
    UE_LOGI("[HOSTILE-DRILL] %s phase %d of %d: go said", ArmName(a), g.phase, PhaseCount(a));
    g.phaseAt = Clock::now();
    g.sentAt = g.phaseAt;
    g.step = PhaseOf(a, g.phase).judgeAtGo ? HStep::Judge : HStep::AwaitSent;
}

}  // namespace

void HostTick(coop::net::Session* s, Arm a) {
    if (g.step == HStep::Done) return;
    if (g.step == HStep::Begin) {
        g.armAt = g.phaseAt = Clock::now();
        g.step = HStep::WaitClient;
    }
    if (g.step == HStep::WaitClient) {
        if (Clock::now() - g.armAt > kFailBound) {
            Finish(a, "INCONCLUSIVE", std::string("the precondition never held: waiting for ") + g.waiting);
            return;
        }
        // The client's line, latched on sight from the arm's first tick: a live chat row retires after seconds.
        if (!g.clientReady && DC::PollDue()) g.clientReady = DC::ChatHasLive(ClientReadyLine(a));
        if (!PuppetReady(s)) return;
        if (!g.clientReady) { g.waiting = "the client's ready line"; return; }
        if (a == Arm::ExplodeForge && !g.guarded) {
            XD::g_src.host = true;   // this process's role, for the guard's own log lines
            if (XD::Guard(false)) { g.waiting = "the guard (startInvinc, unlockFungunsInStory) to resolve"; return; }
            g.guarded = true;
        }
        g.playerNo = coop::roster_ledger::Get(kSlot).playerNo;
        g.phaseAt = Clock::now();
        g.waiting = "the first phase's ATVs";
        g.step = HStep::Prep;
        UE_LOGI("[HOSTILE-DRILL] %s armed on slot %u", ArmName(a), static_cast<unsigned>(kSlot));
    }
    switch (g.step) {
    case HStep::Prep:
        if (Bounded(a)) return;
        if (PrepDone(a, PhaseOf(a, g.phase))) SayGo(a);
        return;
    case HStep::AwaitSent: return AwaitSent(a);
    case HStep::Judge:     return Judge(s, a);
    default:               return;
    }
}

void HostReset() {
    if (g.sat) {
        if (void* atv = g.atv[0].Get()) A::Dismount(atv);
    }
    if (g.guarded) XD::Unguard();
    g = Host{};
}

}  // namespace coop::dev::hostile_drill::atv::detail
