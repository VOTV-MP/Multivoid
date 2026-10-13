// coop/world/coord_tower_ops.cpp -- see coop/world/coord_tower_ops.h.

#include "coop/world/coord_tower_ops.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_lifecycle.h"  // a refused pull's fuse reaped
#include "coop/props/remote_prop.h"     // the destroys this host applies: a spent fuse
#include "coop/session/net_pump.h"      // IsInAnnouncedWorld
#include "coop/world/coord_tower_rows.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/coord_tower.h"
#include "ue_wrap/engine/engine.h"             // TryGetActorLocation
#include "ue_wrap/engine/engine_mainplayer.h"  // what the local player holds
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/game_rules.h"          // the difficulty an insert reads

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace coop::coord_tower_ops {
namespace {

namespace CT = ue_wrap::coord_tower;
namespace EL = coop::element;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
using coop::net::CoordTowerPayload;

static_assert(coop::net::kMaxPeers <= 4, "CoordTowerPayload's acknowledgements hold four slots");

// A slot's ops wait, in arrival order, while the towers have not resolved, their sender has no body here yet (a
// joiner's first pose follows its world-ready by seconds) or an insert's spend has not arrived (its destroy rides
// the bulk lane); each is taken as refused once it has waited this long (coop/world/power_grid's bound).
constexpr uint64_t kWaitMs = 10000;
constexpr size_t   kMaxQueued = 32;
// A lights-out puzzle is pressed a few times a second; a sender's ops run at this rate, the rest waiting their turn.
constexpr coop::net::IntentBudget kOpBudget{8.0f, 8.0f};
// A spend waits for its insert's claim; a player's inserts come seconds apart, so a few a slot cover any that race.
constexpr int kSpendsPerSlot = 4;

std::atomic<coop::net::Session*> g_session{nullptr};

coop::net::Session* Connected() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->connected()) ? s : nullptr;
}

bool IsClient(coop::net::Session* s) { return s && s->role() == coop::net::Role::Client; }

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// `a` is later than `b` in a 16-bit sequence that wraps.
bool SeqAfter(uint16_t a, uint16_t b) { return static_cast<int16_t>(static_cast<uint16_t>(a - b)) > 0; }

bool IsClaim(uint8_t op) { return op == coop::net::kCoordTowerOpPull || op == coop::net::kCoordTowerOpInsert; }

const char* OpName(uint8_t op) {
    switch (op) {
    case coop::net::kCoordTowerOpButton:  return "button press";
    case coop::net::kCoordTowerOpLever:   return "lever";
    case coop::net::kCoordTowerOpRetract: return "panel retract";
    case coop::net::kCoordTowerOpPull:    return "fuse pull";
    case coop::net::kCoordTowerOpInsert:  return "fuse insert";
    default:                              return "?";
    }
}

CT::LookAt PartOf(uint8_t op, int32_t index) {
    switch (op) {
    case coop::net::kCoordTowerOpButton:  return {CT::Part::Button, index};
    case coop::net::kCoordTowerOpLever:   return {CT::Part::Lever, -1};
    case coop::net::kCoordTowerOpRetract: return {CT::Part::Retract, -1};
    default:                              return {CT::Part::Fuse, index};
    }
}

void* TowerById(int32_t id) {
    void* towers[coop::net::kCoordTowers];
    const int32_t n = CT::ReadAll(towers, coop::net::kCoordTowers);
    for (int32_t i = 0; i < n; ++i)
        if (CT::IdOf(towers[i]) == id) return towers[i];
    return nullptr;
}

uint64_t g_rollsRefused = 0;

// ---- the host ------------------------------------------------------------------------------------------------

coop::roster_ledger::PerSlotState<uint16_t> g_ack;
coop::roster_ledger::PerSlotState<uint16_t> g_refused;
struct Waiting { CoordTowerPayload p; uint64_t arrivedMs; };
std::deque<Waiting> g_waiting[coop::net::kMaxPeers];
coop::net::IntentBucket g_rate[coop::net::kMaxPeers];
struct Spend { int32_t tower = -1; uint64_t ms = 0; };  // -1 an empty place
struct Spends { Spend ring[kSpendsPerSlot]; uint8_t next = 0; };
coop::roster_ledger::PerSlotState<Spends> g_spends;
uint64_t g_taken = 0, g_refusedCount = 0, g_refunds = 0, g_queuesFull = 0, g_outOfReach = 0;

// A client's destroy this host applies: a good fuse its sender spent within reach of a tower's fuses is a spend
// there, judged from the host's puppet of the sender with the lane's own reach.
void OnDestroyHeard(int senderSlot, void* actor) {
    auto* s = Connected();
    if (!s || s->role() != coop::net::Role::Host) return;
    if (senderSlot <= 0 || senderSlot >= coop::net::kMaxPeers || !CT::IsFuse(actor)) return;
    const auto sender = static_cast<uint8_t>(senderSlot);
    void* towers[coop::net::kCoordTowers];
    const int32_t n = CT::ReadAll(towers, coop::net::kCoordTowers);
    const EL::IntentTarget target = EL::IntentTarget::ForClientIntent(*s, sender, kTowerReachUU);
    for (int32_t i = 0; i < n; ++i) {
        ue_wrap::FVector at{};
        if (!CT::PartLocation(towers[i], CT::LookAt{CT::Part::Fuse, 0}, at)) continue;
        const EL::IntentSubject reach = target.AuthorizeSegment(at, at);
        if (!reach) continue;
        Spends& sp = g_spends[sender];
        sp.ring[sp.next] = {CT::IdOf(towers[i]), NowMs()};
        sp.next = static_cast<uint8_t>((sp.next + 1) % kSpendsPerSlot);
        UE_LOGI("coord_tower: slot %u spent a fuse at tower %d (%.0f uu, allowed %.0f)", sender, CT::IdOf(towers[i]),
                reach.distUU, reach.reachUU);
        return;
    }
}

// The newest spend `slot` made at tower `id`, burnt. False when there is none.
bool TakeSpend(uint8_t slot, int32_t id) {
    Spends& sp = g_spends[slot];
    Spend* newest = nullptr;
    for (Spend& e : sp.ring)
        if (e.tower == id && (!newest || e.ms > newest->ms)) newest = &e;
    if (!newest) return false;
    *newest = Spend{};
    return true;
}

// A refused insert's fuse, given back where its inserter stands (above the tower's fuses when the body is unread);
// the host's spawn watcher gives it to every peer.
void Refund(uint8_t slot, void* tower) {
    ue_wrap::FVector at{};
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    void* body = rp ? rp->GetActor() : nullptr;
    bool placed = body && ue_wrap::engine::TryGetActorLocation(body, at);
    if (!placed && CT::PartLocation(tower, CT::LookAt{CT::Part::Fuse, 0}, at)) {
        at.Z += 80.f;
        placed = true;
    }
    void* fuse = placed ? CT::SpawnFuse(at) : nullptr;
    if (fuse) ++g_refunds;
    UE_LOGW("coord_tower: slot %u's refused insert refunded -- %s", slot,
            fuse ? "a fuse spawned where it stands" : "NOT spawned (the fuse is lost)");
}

// The insert's own branch on the difficulty (coordRadarDish playerUsedOn @4823): on the two easiest, the
// puzzle is solved with it.
bool InsertSolves() {
    ue_wrap::game_rules::Snapshot rules;
    if (!ue_wrap::game_rules::ReadLocal(rules)) return false;
    for (const auto& f : rules.fields)
        if (f.key == "difficulty") return f.ival <= 1;
    return false;
}

enum class Take : uint8_t { Done, Wait };

Take HostTakeOp(coop::net::Session* s, const Waiting& wt, uint8_t sender) {
    const CoordTowerPayload& p = wt.p;
    const bool waited = NowMs() - wt.arrivedMs >= kWaitMs;
    if (!CT::EnsureResolved() && !waited) return Take::Wait;
    if (!SeqAfter(p.seq, g_ack[sender])) {
        UE_LOGW("coord_tower: host dropped slot %u's %s seq %u (last taken %u)", sender, OpName(p.op), p.seq,
                g_ack[sender]);
        return Take::Done;
    }
    void* tower = TowerById(p.tower);
    const CT::LookAt part = PartOf(p.op, p.index);
    ue_wrap::FVector at{};
    const bool located = tower && CT::PartLocation(tower, part, at);
    EL::IntentSubject reach{};
    if (located) reach = EL::IntentTarget::ForClientIntent(*s, sender, kTowerReachUU).AuthorizeSegment(at, at);
    if (located && reach.outcome == EL::IntentOutcome::NoBody && !waited) return Take::Wait;
    // An insert rests on the fuse its own game spent, which the host saw destroyed beside this tower: taken here,
    // whatever the outcome, so one spend answers one insert. None yet waits for the destroy behind it.
    bool spent = false;
    if (located && p.op == coop::net::kCoordTowerOpInsert) {
        spent = TakeSpend(sender, p.tower);
        if (!spent && !waited) return Take::Wait;
    }
    CT::State st;
    const char* refusal = !tower ? "no tower of that id" : !located ? "that part is not there"
                        : !reach ? EL::OutcomeName(reach.outcome) : !CT::Read(tower, st) ? "the tower does not read"
                        : nullptr;
    const bool slotOk = p.index < st.fuseCount;
    if (!refusal && p.op == coop::net::kCoordTowerOpPull) {
        // The pull's own gates (coordRadarDish actionOptionIndex @3586-@3782), on the host's copy.
        if (!st.opened) refusal = "its panel is closed";
        else if (st.isAnim) refusal = "its panel is moving";
        else if (st.leverMoving) refusal = "its lever is moving";
        else if (!slotOk || st.fuses[p.index] != 2) refusal = "that fuse is not blown on the host";
    } else if (!refusal && p.op == coop::net::kCoordTowerOpInsert) {
        if (!spent) refusal = "no fuse of the inserter's was spent at it";
        else if (!slotOk || st.fuses[p.index] != 0) refusal = "that fuse slot is not empty on the host";
    }
    g_ack[sender] = p.seq;
    if (refusal) {
        ++g_refusedCount;
        // A client standing out of reach presses often: said at first and then now and then.
        const bool quiet =
            reach.outcome == EL::IntentOutcome::OutOfReach && ++g_outOfReach > 3 && g_outOfReach % 20 != 0;
        if (!quiet)
            UE_LOGW("coord_tower: host refused slot %u's %s of tower %d (seq %u) -- %s (%.0f uu, allowed %.0f)",
                    sender, OpName(p.op), p.tower, p.seq, refusal, reach.distUU, reach.reachUU);
        if (IsClaim(p.op)) g_refused[sender] = p.seq;
        if (spent) Refund(sender, tower);
        coop::coord_tower_rows::HostSendNow();  // answered before the sender's next op is taken
        return Take::Done;
    }
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(sender);
    void* puppet = rp ? rp->GetActor() : nullptr;
    switch (p.op) {
    case coop::net::kCoordTowerOpButton:
    case coop::net::kCoordTowerOpLever:
    case coop::net::kCoordTowerOpRetract:
        // The tower's own use, on its own gates: the panel open, the lever at rest, the tower broken.
        CT::Use(tower, puppet, part);
        break;
    case coop::net::kCoordTowerOpPull:
        // The pull's writes on the host's copy; its fuse is already in its puller's hand.
        CT::WriteFuse(tower, p.index, 0);
        CT::UpdFuses(tower);
        CT::Play(tower, CT::Sound::FusePulled);
        break;
    case coop::net::kCoordTowerOpInsert:
        // The insert's writes (playerUsedOn @4823..@5317), on the host's copy; its fuse is already spent.
        if (InsertSolves()) CT::SolvePuzzle(tower);
        CT::WriteFuse(tower, p.index, 1);
        CT::Play(tower, CT::Sound::FuseInserted);
        CT::UpdFuses(tower);
        break;
    default: break;
    }
    ++g_taken;
    UE_LOGI("coord_tower: host took slot %u's %s of tower %d (index %u, seq %u)", sender, OpName(p.op), p.tower,
            p.index, p.seq);
    coop::coord_tower_rows::HostSendNow();
    return Take::Done;
}

void HostOffer(coop::net::Session* s, const CoordTowerPayload& p, uint8_t sender) {
    if (sender == 0 || sender >= coop::net::kMaxPeers || p.op == coop::net::kCoordTowerOpRows ||
        p.op > coop::net::kCoordTowerOpInsert)
        return;
    auto& q = g_waiting[sender];
    const Waiting w{p, NowMs()};
    if (q.empty() && g_rate[sender].Take(kOpBudget, NowMs())) {
        if (HostTakeOp(s, w, sender) == Take::Done) return;
        g_rate[sender].Refund(kOpBudget);  // a wait runs nothing
    }
    if (q.size() >= kMaxQueued) {
        // Every waiting op refused with the newest, in order, as a refusal of each would answer it: a claim's own
        // rows name it refused, so a pull's fuse is reaped, and an insert's spend is refunded.
        const size_t n = q.size();
        q.push_back(w);
        for (const Waiting& r : q) {
            if (SeqAfter(r.p.seq, g_ack[sender])) g_ack[sender] = r.p.seq;
            if (!IsClaim(r.p.op)) continue;
            g_refused[sender] = r.p.seq;
            if (r.p.op == coop::net::kCoordTowerOpInsert && TakeSpend(sender, r.p.tower))
                if (void* tower = TowerById(r.p.tower)) Refund(sender, tower);
            coop::coord_tower_rows::HostSendNow();
        }
        q.clear();
        g_refusedCount += n + 1;
        if (++g_queuesFull <= 3 || g_queuesFull % 100 == 0)
            UE_LOGE("coord_tower: host refused slot %u's %zu waiting ops and seq %u -- its queue is full", sender, n,
                    p.seq);
        coop::coord_tower_rows::HostSendNow();
        return;
    }
    q.push_back(w);
}

void HostDrainWaiting(coop::net::Session* s) {
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        auto& q = g_waiting[slot];
        while (!q.empty() && g_rate[slot].Take(kOpBudget, NowMs())) {
            if (HostTakeOp(s, q.front(), slot) == Take::Wait) {
                g_rate[slot].Refund(kOpBudget);
                break;
            }
            q.pop_front();
        }
    }
}

// ---- a client ------------------------------------------------------------------------------------------------

// My claims the host has not answered, oldest first.
struct Claim { uint16_t seq; uint8_t op; int32_t tower; uint8_t slot; bool solved; };
std::vector<Claim> g_claims;
uint16_t g_seq = 0;
uint16_t g_lastAck = 0;
uint64_t g_sent = 0, g_reaped = 0;
// The use and the insert in flight: the fuse slot the pre saw it would move.
void*   g_pullTower = nullptr;
int32_t g_pullSlot = -1;
void*   g_insertTower = nullptr;
int32_t g_insertSlot = -1;
bool    g_insertWasBroken = false;

void ClientSend(coop::net::Session* s, uint8_t op, int32_t tower, int32_t index, bool solved = false) {
    CoordTowerPayload p{};
    p.op = op;
    p.tower = tower;
    p.index = static_cast<uint8_t>(std::max(index, 0));
    p.seq = ++g_seq;
    if (IsClaim(op)) g_claims.push_back({p.seq, op, tower, p.index, solved});
    s->SendReliableToSlot(0, coop::net::ReliableKind::CoordTowerState, &p, sizeof(p));
    ++g_sent;
    UE_LOGI("coord_tower: local %s of tower %d (index %d, seq %u) -- sent to the host", OpName(op), tower, index,
            p.seq);
}

// A refused pull: the fuse its own game put in my hand goes, when it is still there (the floppy crate's reap).
void Reap(const Claim& c) {
    void* player = coop::players::Registry::Get().Local();
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (player && ue_wrap::engine::ReadMainPlayerGrabState(player, gs)) {
        void* held = gs.grabbingActor ? gs.grabbingActor : gs.holdingActor;
        if (held && CT::IsPulledFuse(held)) {
            coop::prop_lifecycle::DestroyLocalProp(held, /*deferred*/true);
            ++g_reaped;
            UE_LOGW("coord_tower: my pull of tower %d's fuse %u was refused -- the fuse it handed me is reaped",
                    c.tower, c.slot);
            return;
        }
    }
    UE_LOGW("coord_tower: my pull of tower %d's fuse %u was refused -- its fuse has left my hand, nothing reaped",
            c.tower, c.slot);
}

bool ClientGatesOpen(const sg::Call& call) {
    return !IsClient(Connected()) || RedOpen() || coop::coord_tower_rows::Applying() || !call.object ||
           !coop::net_pump::IsInAnnouncedWorld(call.object);
}

// ---- the watches ---------------------------------------------------------------------------------------------

// A client's own roll and its lever's judgement never run: the host's world rolls and judges every tower, and
// its rows run each change here. Every route: the fuckuper's timer, an explosion, a tower that loads broken.
sg::Verdict OnRollPre(const sg::Call&) {
    if (!IsClient(Connected()) || RedOpen()) return sg::Verdict::Run;
    if (++g_rollsRefused == 1)
        UE_LOGI("coord_tower: this client refuses its towers' own rolls and lever judgements -- the host's towers "
                "are the author");
    return sg::Verdict::Cancel;
}

// A client's use: a press of a button, the lever or the retract goes to the host, which runs it; a pull runs here
// and its move is claimed as the use ends.
int32_t g_actionOff = -1;
void* g_actionFn = nullptr;
sg::Verdict OnUsePre(const sg::Call& call) {
    g_pullTower = nullptr;
    if (ClientGatesOpen(call)) return sg::Verdict::Run;
    if (g_actionFn != call.function) {
        g_actionFn = call.function;
        g_actionOff = R::FindParamOffset(call.function, L"action");
    }
    if (g_actionOff < 0 || call.locals[g_actionOff] != 4) return sg::Verdict::Run;
    CT::LookAt at;
    const int32_t id = CT::IdOf(call.object);
    if (id < 0 || !CT::ReadLookAt(call.object, at)) return sg::Verdict::Run;
    switch (at.part) {
    case CT::Part::Button:
        ClientSend(Connected(), coop::net::kCoordTowerOpButton, id, at.index);
        return sg::Verdict::Cancel;
    case CT::Part::Lever:
        ClientSend(Connected(), coop::net::kCoordTowerOpLever, id, -1);
        return sg::Verdict::Cancel;
    case CT::Part::Retract:
        ClientSend(Connected(), coop::net::kCoordTowerOpRetract, id, -1);
        return sg::Verdict::Cancel;
    case CT::Part::Fuse: {
        CT::State st;
        if (CT::Read(call.object, st) && at.index >= 0 && at.index < st.fuseCount && st.fuses[at.index] == 2) {
            g_pullTower = call.object;
            g_pullSlot = at.index;
        }
        return sg::Verdict::Run;
    }
    default:
        return sg::Verdict::Run;
    }
}

void OnUsePost(const sg::Call& call) {
    if (!call.object || call.object != g_pullTower) return;
    g_pullTower = nullptr;
    CT::State st;
    auto* s = Connected();
    if (!IsClient(s) || !CT::Read(call.object, st) || g_pullSlot >= st.fuseCount || st.fuses[g_pullSlot] != 0) return;
    ClientSend(s, coop::net::kCoordTowerOpPull, CT::IdOf(call.object), g_pullSlot);
}

// A client's insert runs here, and its move is claimed as it ends.
int32_t g_holdOff = -1;
void* g_insertFn = nullptr;
sg::Verdict OnInsertPre(const sg::Call& call) {
    g_insertTower = nullptr;
    if (ClientGatesOpen(call)) return sg::Verdict::Run;
    if (g_insertFn != call.function) {
        g_insertFn = call.function;
        g_holdOff = R::FindParamOffset(call.function, L"holdObject");
    }
    if (g_holdOff < 0) return sg::Verdict::Run;
    void* held = *reinterpret_cast<void* const*>(call.locals + g_holdOff);
    CT::State st;
    int32_t slot = -1;
    if (!CT::IsFuse(held) || !CT::Read(call.object, st) || !CT::ReadFuseLook(call.object, slot) || slot < 0 ||
        slot >= st.fuseCount || st.fuses[slot] != 0)
        return sg::Verdict::Run;
    g_insertTower = call.object;
    g_insertSlot = slot;
    g_insertWasBroken = st.isBroken;
    return sg::Verdict::Run;
}

void OnInsertPost(const sg::Call& call) {
    if (!call.object || call.object != g_insertTower) return;
    g_insertTower = nullptr;
    CT::State st;
    auto* s = Connected();
    if (!IsClient(s) || !CT::Read(call.object, st) || g_insertSlot >= st.fuseCount || st.fuses[g_insertSlot] != 1)
        return;
    ClientSend(s, coop::net::kCoordTowerOpInsert, CT::IdOf(call.object), g_insertSlot,
               g_insertWasBroken && !st.isBroken);
}

UE_FACT_FN(kRowScrambleRadarDish, "coord_tower_ops", L"coordRadarDish_C", L"Scramble Radar Dish", Declared);
UE_FACT_FN(kRowLeverTlFinished, "coord_tower_ops", L"coordRadarDish_C", L"leverTL__FinishedFunc", Declared);
UE_FACT_FN(kRowActionOptionIndex, "coord_tower_ops", L"coordRadarDish_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowPlayerUsedOn, "coord_tower_ops", L"coordRadarDish_C", L"playerUsedOn", Declared);

struct WatchDef {
    const ue_wrap::facts::Function* row;
    int tag;
    sg::PreFn pre;
    sg::PostFn post;
};
constexpr WatchDef kWatches[] = {
    { &kRowScrambleRadarDish, 0x43545352 /*'CTSR'*/, &OnRollPre,   nullptr },
    { &kRowLeverTlFinished,   0x4354464E /*'CTFN'*/, &OnRollPre,   nullptr },
    { &kRowActionOptionIndex, 0x4354414F /*'CTAO'*/, &OnUsePre,    &OnUsePost },
    { &kRowPlayerUsedOn,      0x43545555 /*'CTUU'*/, &OnInsertPre, &OnInsertPost },
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));
bool g_registered = false;
bool g_settled = false;

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_registered) return;
    g_registered = true;
    coop::remote_prop::AddDestroyListener(&OnDestroyHeard);
    for (const WatchDef& w : kWatches)
        if (!sg::WatchName(*w.row, w.tag, w.pre, w.post))
            UE_LOGE("coord_tower: the script-body gate refused the watch on %ls::%ls -- that act stays each peer's own",
                    w.row->Class(), w.row->Name());
}

void Tick() {
    if (auto* s = Connected(); s && s->role() == coop::net::Role::Host) HostDrainWaiting(s);
    if (g_settled || !g_registered) return;
    sg::ResolvePendingNames();
    int live = 0, settled = 0;
    for (const WatchDef& w : kWatches) {
        if (sg::NameWatchLive(*w.row, w.tag)) ++live;
        if (sg::NameWatchSettled(*w.row, w.tag)) ++settled;
    }
    if (live == kWatchCount) {
        g_settled = true;
        UE_LOGI("coord_tower: the towers' gates are live (the roll, the lever's judgement, a use, an insert)");
    } else if (settled == kWatchCount) {
        g_settled = true;
        UE_LOGE("coord_tower: %d of %d tower gates are dead -- those acts stay each peer's own", kWatchCount - live,
                kWatchCount);
    }
}

void OnOp(const CoordTowerPayload& p, uint8_t sender) {
    auto* s = Connected();
    if (!s || s->role() != coop::net::Role::Host) return;
    HostOffer(s, p, sender);
}

void FillAcks(CoordTowerPayload& p) {
    for (int i = 0; i < coop::net::kMaxPeers; ++i) {
        p.ack[i] = g_ack[i];
        p.refused[i] = g_refused[i];
    }
}

void OnAcks(uint16_t ack, uint16_t refused) {
    g_lastAck = ack;
    size_t kept = 0;
    for (const Claim& c : g_claims) {
        if (SeqAfter(c.seq, ack)) {
            g_claims[kept++] = c;
            continue;
        }
        if (c.seq == refused) {
            if (c.op == coop::net::kCoordTowerOpPull) Reap(c);
            else UE_LOGW("coord_tower: my insert into tower %d's slot %u was refused -- the host gives the fuse back",
                         c.tower, c.slot);
        }
    }
    g_claims.resize(kept);
}

void Overlay(coop::net::CoordTowerRow& row) {
    for (const Claim& c : g_claims) {
        if (c.tower != row.id || c.slot >= row.fuseCount) continue;
        if (c.op == coop::net::kCoordTowerOpPull) {
            row.fuses[c.slot] = 0;
        } else {
            row.fuses[c.slot] = 1;
            if (c.solved) {
                // The insert solved the puzzle here, as the host's copy will once it takes the claim.
                row.flags = static_cast<uint8_t>(row.flags & ~coop::net::kCoordTowerBroken);
                row.lights = static_cast<uint16_t>((1u << row.lightCount) - 1u);
            }
        }
    }
}

void OnPeerLeft(uint8_t slot) {
    if (slot == 0 || slot >= coop::net::kMaxPeers) return;
    g_waiting[slot].clear();
    g_rate[slot].Reset();
}

uint64_t OpsSent() { return g_sent; }
size_t PendingClaims() { return g_claims.size(); }
uint64_t OpsRefused() { return g_refusedCount; }
uint64_t RollsRefused() { return g_rollsRefused; }
uint16_t LastSentSeq() { return g_seq; }
uint16_t LastAck() { return g_lastAck; }
uint64_t Reaped() { return g_reaped; }

bool RedOpen() {
    static const bool red = [] {
        const std::string v = coop::config::ResolveString(::coop::config_registry::rows::tower_drill);
        return v == "red" || v == "joinred";
    }();
    return red;
}

void OnDisconnect() {
    if (g_sent || g_taken || g_refusedCount || g_rollsRefused)
        UE_LOGI("coord_tower: session end -- ops sent %llu, taken %llu, refused %llu, refunds %llu, reaped %llu, "
                "rolls refused %llu", static_cast<unsigned long long>(g_sent),
                static_cast<unsigned long long>(g_taken), static_cast<unsigned long long>(g_refusedCount),
                static_cast<unsigned long long>(g_refunds), static_cast<unsigned long long>(g_reaped),
                static_cast<unsigned long long>(g_rollsRefused));
    for (auto& q : g_waiting) q.clear();
    for (auto& b : g_rate) b.Reset();
    for (int slot = 0; slot < g_spends.size(); ++slot) g_spends[slot] = Spends{};
    g_claims.clear();
    g_seq = g_lastAck = 0;
    g_pullTower = g_insertTower = nullptr;
    g_sent = g_reaped = g_taken = g_refusedCount = g_refunds = g_queuesFull = g_outOfReach = g_rollsRefused = 0;
}

}  // namespace coop::coord_tower_ops
