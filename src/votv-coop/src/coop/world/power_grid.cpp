// coop/world/power_grid.cpp -- see coop/world/power_grid.h.

#include "coop/world/power_grid.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"         // what the sender's hand holds
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld: a client's own world load runs natively
#include "coop/world/power_decay.h"  // the wear dice, which only the host rolls
#include "coop/world/power_panel.h"  // the canonical a client's own generator verbs rewrote
#include "coop/world/power_puzzle.h"  // the rows' second half: each generator's repair puzzle
#include "coop/world/power_upgrade.h"  // the spends an install rests on, and a refused one's refund

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/generator.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/weapon_catalog.h"  // whether the held item swings

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace coop::power_grid {
namespace {

namespace EL  = coop::element;
namespace GEN = ue_wrap::generator;
namespace R   = ue_wrap::reflection;
namespace sg  = ue_wrap::script_gate;
using coop::net::PowerGridPayload;
using coop::net::kPowerGridGenerators;

static_assert(coop::net::kMaxPeers <= 4, "PowerGridPayload::ack holds four slots");
static_assert(coop::net::kMaxPeers <= 8, "the owed rows are one bit a slot");
static_assert(kInputBurst >= coop::net::kPowerPuzzleFields, "a flush of every held field fits one burst");

constexpr int32_t kMaxUpgrade   = 6;    // the insert's own gate, `upgradeLevel < 6`
constexpr int32_t kFullWear     = 100;  // what the Activate button's repair and service write the wear back to
// A slot's ops wait, in arrival order, while the generators have not resolved or their sender has no body here
// yet (a joiner's first pose follows its world-ready by seconds, 3 s measured on the LAN rig), and an install
// while the destroy of the upgrade its insert spent has not arrived: it was sent first, on the same ordered Default
// lane, so it lands ahead of the op unless the link is slow. Each is taken as refused once it has waited this long, and
// answered while the host has generators to read: unlike a door verb, which waits for its body without a bound, a
// repair or an install is a prediction its sender already shows.
constexpr uint64_t kWaitMs = 10000;
constexpr size_t   kMaxQueued = 32;
// A puzzle input waits this long at the head for its sender's claim on the panel, which rides another kind.
constexpr uint64_t kClaimWaitMs = 2000;
// A player swings at a generator a few times a second at most; a sender's acts run at this rate, the rest waiting
// their turn (door_verb_intent's bound). Its puzzle inputs run at their own (power_grid.h).
constexpr coop::net::IntentBudget kOpBudget{4.0f, 4.0f};
constexpr coop::net::IntentBudget kInputBudget{kInputBurst, kInputPerSecond};

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

// ---- the rows and the ops ------------------------------------------------------------------------------------

// HOST: what the last broadcast said, the last predicted op taken from each slot, the ops that wait, each
// sender's rate, and the joiners owed their rows.
PowerGridPayload g_lastSent{};
bool g_haveSent = false;
coop::roster_ledger::PerSlotState<uint16_t> g_ack;
bool g_ackDirty = false;
struct Waiting { PowerGridPayload p; uint64_t arrivedMs; };
std::deque<Waiting> g_waiting[coop::net::kMaxPeers];
coop::net::IntentBucket g_rate[coop::net::kMaxPeers];       // a sender's acts
coop::net::IntentBucket g_inputRate[coop::net::kMaxPeers];  // a sender's puzzle inputs
uint8_t g_owed = 0;
uint64_t g_opsTaken = 0, g_opsRefused = 0, g_noisyAnswered = 0, g_inputsTaken = 0, g_queuesFull = 0;
uint64_t g_rowsBroadcast = 0;
bool g_puzzleDirty = false;
uint64_t g_lastRowsMs = 0;

// The generator verbs whose exit sends the rows, recorded as each begins and held by slot and serial: only the
// outermost of them sends, so a break's rows follow the blackout canonical its solar() sent, and a client never
// applies a break before it. upd() is one, for its own call at a world's load.
enum Verb : int { kVerbBreak, kVerbDamage, kVerbFullFix, kVerbUpdUpgrades, kVerbActivate, kVerbUpd, kVerbCount };
ue_wrap::CachedObjRef g_verbFn[kVerbCount];

void RecordVerb(int verb, void* fn) {
    if (!g_verbFn[verb].Is(fn)) g_verbFn[verb].Set(fn);
}

// CLIENT: my predicted ops the host has not taken, oldest first; the host's last rows, and whether they still
// wait for this peer's generators; the Activate press in flight, its generator and its row as it began.
struct Pending { uint16_t seq; uint8_t op; uint8_t index; };
std::vector<Pending> g_pending;
uint16_t g_seq = 0;
PowerGridPayload g_rows{};
bool g_haveRows = false;
bool g_rowsWaiting = false;
void* g_pressGen = nullptr;
GEN::Row g_pressRow{};
// Set while this client runs the host's rows on its generators: an updUpgrades then is the rows', not a player's
// install. A player's install made through our own call (the drill's) is still one, so the test is this scope, not
// fromOurCode (power_puzzle's own writes, the precedent).
bool g_applying = false;
uint64_t g_editsRefused = 0, g_opsSent = 0, g_hitsSent = 0, g_rowsApplied = 0;

// The hit's parameters, per function (one generator class, so one resolve).
struct HitParams { void* fn = nullptr; int32_t actor = -1; int32_t damage = -1; };
HitParams g_hitParams;

// [dev] grid_drill=red: a client writes the rows raw, as the old lanes left each peer to its own generators.
bool RedApply() {
    static const bool red = coop::config::ResolveString(::coop::config_registry::rows::grid_drill) == "red";
    return red;
}

// [dev] grid_drill=upgradered: the host judges an install as if its spend were recorded, the unpaired shape.
bool UnpairedInstalls() {
    static const bool red = coop::config::ResolveString(::coop::config_registry::rows::grid_drill) == "upgradered";
    return red;
}

// [dev] the grid drill's stand-in for a generator another install filled first: the host refuses installs.
bool g_devRefuseInstalls = false;

// `a` is later than `b` in a 16-bit sequence that wraps.
bool SeqAfter(uint16_t a, uint16_t b) { return static_cast<int16_t>(static_cast<uint16_t>(a - b)) > 0; }

uint8_t LocalSlot() { return coop::players::Registry::Get().LocalPeerId(); }

// An Activate press, an install and a puzzle input run on their client first, and the host's rows reconcile
// them; a hit breaks a generator, which only the host's world does.
bool Predicted(uint8_t op) {
    return op == coop::net::kPowerGridOpActivate || op == coop::net::kPowerGridOpUpgrade ||
           op == coop::net::kPowerGridOpPuzzle;
}

const char* OpName(uint8_t op) {
    switch (op) {
    case coop::net::kPowerGridOpActivate: return "Activate press";
    case coop::net::kPowerGridOpUpgrade:  return "upgrade";
    case coop::net::kPowerGridOpHit:      return "hit";
    case coop::net::kPowerGridOpPuzzle:   return "puzzle input";
    default:                              return "?";
    }
}

bool ReadRows(PowerGridPayload& p) {
    std::vector<void*> gens;
    if (GEN::ReadGenerators(gens) == 0) return false;
    const size_t n = std::min(gens.size(), static_cast<size_t>(kPowerGridGenerators));
    p.op = coop::net::kPowerGridOpRows;
    p.count = static_cast<uint8_t>(n);
    for (size_t i = 0; i < n; ++i) {
        GEN::Row r{};
        coop::net::PowerGridRow& w = p.rows[i];
        w = {};
        if (!gens[i] || !GEN::ReadRow(gens[i], r)) continue;
        w.present = 1;
        w.broken = r.broken ? 1 : 0;
        w.cyc = r.cyc ? 1 : 0;
        w.upgradeLevel = static_cast<uint8_t>(std::clamp(r.upgradeLevel, 0, kMaxUpgrade));
        w.cycle = r.cycle;
        coop::power_puzzle::Fill(gens[i], w.puzzle);
    }
    return true;
}

bool SameRows(const PowerGridPayload& a, const PowerGridPayload& b) {
    if (a.count != b.count) return false;
    for (int i = 0; i < a.count && i < kPowerGridGenerators; ++i) {
        const auto& x = a.rows[i];
        const auto& y = b.rows[i];
        if (x.present != y.present || x.broken != y.broken || x.cyc != y.cyc || x.upgradeLevel != y.upgradeLevel ||
            x.cycle != y.cycle || !coop::power_puzzle::SamePuzzle(x.puzzle, y.puzzle))
            return false;
    }
    return true;
}

// ---- the host ------------------------------------------------------------------------------------------------

// The rows with every slot's acknowledgement: to `onlySlot`, or to every peer when they or an acknowledgement
// changed since the last broadcast.
void HostSendRows(coop::net::Session* s, int onlySlot = -1) {
    PowerGridPayload p{};
    if (!ReadRows(p)) return;
    for (int i = 0; i < coop::net::kMaxPeers; ++i) p.ack[i] = g_ack[i];
    if (onlySlot >= 0) {
        s->SendReliableToSlot(onlySlot, coop::net::ReliableKind::PowerGridState, &p, sizeof(p));
        return;
    }
    g_puzzleDirty = false;
    if (!g_ackDirty && g_haveSent && SameRows(p, g_lastSent)) return;
    s->SendReliable(coop::net::ReliableKind::PowerGridState, &p, sizeof(p));
    ++g_rowsBroadcast;
    g_lastSent = p;
    g_haveSent = true;
    g_ackDirty = false;
    g_lastRowsMs = NowMs();
}

// A hit is a swing: the player's attack swings only an item whose list_weapons row carries a montage and the
// attack flag (door_verb_intent's hit, the precedent).
bool HitSwings(uint8_t slot, std::wstring& item) {
    item = coop::hand_item::HeldItem(slot);
    ue_wrap::weapon_catalog::Swing swing;
    return ue_wrap::weapon_catalog::Ready() && ue_wrap::weapon_catalog::Lookup(item, swing) && swing.canSwing;
}

enum class Take : uint8_t { Done, Wait };

Take HostTakeOp(coop::net::Session* s, const Waiting& wt, uint8_t sender) {
    const PowerGridPayload& p = wt.p;
    const bool waited = NowMs() - wt.arrivedMs >= kWaitMs;
    const bool resolved = GEN::EnsureResolved();
    if (!resolved && !waited) return Take::Wait;
    const bool predicted = Predicted(p.op);
    if (!predicted && p.op != coop::net::kPowerGridOpHit) return Take::Done;
    if (predicted && !SeqAfter(p.seq, g_ack[sender])) {
        // Already taken: its sender still shows it only if no rows have answered it yet.
        UE_LOGW("power_grid: host dropped slot %u's %s seq %u (last taken %u)", sender, OpName(p.op), p.seq,
                g_ack[sender]);
        HostSendRows(s, sender);
        return Take::Done;
    }
    std::vector<void*> gens;
    if (resolved) GEN::ReadGenerators(gens);
    void* gen = p.index < gens.size() ? gens[p.index] : nullptr;
    GEN::Row r{};
    const bool read = gen && GEN::ReadRow(gen, r);
    EL::IntentSubject reach{};
    if (read) reach = EL::IntentTarget::ForClientIntent(*s, sender, kGeneratorReachUU).Authorize(gen);
    if (reach.outcome == EL::IntentOutcome::NoBody && !waited) return Take::Wait;
    // An install rests on the upgrade its insert spent, which the host saw destroyed beside this generator: taken
    // here, whatever the outcome, so one spend answers one install. None yet waits for the destroy behind it.
    bool spent = false;
    if (read && p.op == coop::net::kPowerGridOpUpgrade) {
        spent = coop::power_upgrade::TakeSpend(sender, p.index) || UnpairedInstalls();
        if (!spent && !waited) return Take::Wait;
    }
    std::wstring item;
    const char* refusal = !read ? "that generator is not there" : !reach ? EL::OutcomeName(reach.outcome) : nullptr;
    if (!refusal && p.op == coop::net::kPowerGridOpPuzzle) {
        // Judged and, when taken, written here: an input changes nothing the rows' verbs below run.
        bool wait = false;
        refusal = coop::power_puzzle::HostTake(sender, p, gen, r.broken, NowMs() - wt.arrivedMs >= kClaimWaitMs,
                                               wait);
        if (wait) return Take::Wait;
    }
    // A press judged on a panel whose last click is still moving would read the flags before the values: its move's
    // end runs the setters. The press waits for it, a fifth of a second at most.
    if (!refusal && p.op == coop::net::kPowerGridOpActivate && coop::power_puzzle::Settling(gen) && !waited)
        return Take::Wait;
    if (predicted) {
        g_ack[sender] = p.seq;
        g_ackDirty = true;
    }
    if (!refusal) {
        switch (p.op) {
        case coop::net::kPowerGridOpActivate:
            // The button's own check (generator actionOptionIndex @3044), on the host's copy of the puzzle, which
            // the presser's inputs reached ahead of the press.
            if (!coop::power_puzzle::Solved(gen)) refusal = "its puzzle is not solved on the host";
            break;
        case coop::net::kPowerGridOpUpgrade:
            // The insert spends the held upgrade before the op comes: two players installing at the last free place
            // leave the second one's spent, and its refusal refunds it.
            if (!spent) refusal = "no upgrade of the installer's was spent at it";
            else if (r.upgradeLevel >= kMaxUpgrade) refusal = "its upgrades are full";
            else if (g_devRefuseInstalls) refusal = "the drill refuses installs as if its upgrades were full";
            break;
        case coop::net::kPowerGridOpHit:
            if (!HitSwings(sender, item)) refusal = "the hitter's held item does not swing";
            break;
        default: break;
        }
    }
    const bool hit = p.op == coop::net::kPowerGridOpHit;
    const bool quiet = hit || p.op == coop::net::kPowerGridOpPuzzle;  // swings and drags come several a second
    const bool say = !quiet || ++g_noisyAnswered <= 3 || g_noisyAnswered % 20 == 0;
    if (refusal) {
        ++g_opsRefused;
        if (say)
            UE_LOGW("power_grid: host refused slot %u's %s of generator %u (seq %u) -- %s (%.0f uu, allowed %.0f)",
                    sender, OpName(p.op), p.index, p.seq, refusal, reach.distUU, reach.reachUU);
        if (predicted) HostSendRows(s, sender);  // its acknowledgement reverts the sender's prediction
        if (spent) coop::power_upgrade::Refund(sender, gen);
        return Take::Done;
    }
    switch (p.op) {
    case coop::net::kPowerGridOpActivate:
        // The button's branch by the host's own generator (@1257): a broken one mended as the Activate route
        // mends it, a whole one serviced, which is all the press writes to it but its 2D cue.
        if (r.broken) GEN::Repair(gen);
        else GEN::WriteCycle(gen, kFullWear);
        break;
    case coop::net::kPowerGridOpUpgrade:
        GEN::WriteUpgradeLevel(gen, r.upgradeLevel + 1);
        GEN::CallUpdUpgrades(gen);
        break;
    case coop::net::kPowerGridOpHit:
        GEN::CallBreak(gen);  // the whole body of the generator's addDamage
        break;
    default: break;
    }
    if (p.op == coop::net::kPowerGridOpPuzzle) ++g_inputsTaken;
    else ++g_opsTaken;
    if (say)
        UE_LOGI("power_grid: host took slot %u's %s of generator %u (seq %u)%s%ls", sender, OpName(p.op), p.index,
                p.seq, hit ? " with " : "", item.c_str());
    // The change and the acknowledgement to every peer: nothing new when the verb's own exit already sent them. A
    // puzzle input's go with the panel's coalesced rows, at most every 100 ms however fast the inputs come.
    if (p.op == coop::net::kPowerGridOpPuzzle) g_puzzleDirty = true;
    else HostSendRows(s);
    return Take::Done;
}

// Every waiting op of `slot` refused with its newest, whose acknowledgement answers them all: its queue is full. An
// install among them spent its upgrade before it came, and is refunded as its own refusal would refund it.
void HostRefuseQueue(coop::net::Session* s, uint8_t slot, const Waiting& newest) {
    const uint16_t newestSeq = newest.p.seq;
    const size_t n = g_waiting[slot].size();
    g_waiting[slot].push_back(newest);
    std::vector<void*> gens;
    for (const Waiting& w : g_waiting[slot]) {
        if (w.p.op != coop::net::kPowerGridOpUpgrade || !coop::power_upgrade::TakeSpend(slot, w.p.index)) continue;
        if (gens.empty()) GEN::ReadGenerators(gens);
        if (w.p.index < gens.size() && gens[w.p.index]) coop::power_upgrade::Refund(slot, gens[w.p.index]);
    }
    g_waiting[slot].clear();
    if (SeqAfter(newestSeq, g_ack[slot])) g_ack[slot] = newestSeq;
    g_ackDirty = true;
    g_opsRefused += n + 1;
    if (++g_queuesFull <= 3 || g_queuesFull % 100 == 0)
        UE_LOGE("power_grid: host refused slot %u's %zu waiting ops and seq %u -- its queue is full (%llu times)", slot,
                n, newestSeq, static_cast<unsigned long long>(g_queuesFull));
    HostSendRows(s, slot);
}

// A puzzle input is a value a drag writes several times a second, the rest a player's act: each spends its own
// rate, and an input still waits its turn behind the acts.
bool Spend(uint8_t slot, uint8_t op) {
    return op == coop::net::kPowerGridOpPuzzle ? g_inputRate[slot].Take(kInputBudget, NowMs())
                                               : g_rate[slot].Take(kOpBudget, NowMs());
}

void Refund(uint8_t slot, uint8_t op) {
    // a wait runs nothing
    if (op == coop::net::kPowerGridOpPuzzle) g_inputRate[slot].Refund(kInputBudget);
    else g_rate[slot].Refund(kOpBudget);
}

// An op from the wire is taken at once when its slot has none waiting and, for an act, a token to spend.
void HostOffer(coop::net::Session* s, const PowerGridPayload& p, uint8_t sender) {
    if (sender == 0 || sender >= coop::net::kMaxPeers || p.op == coop::net::kPowerGridOpRows) return;
    auto& q = g_waiting[sender];
    const Waiting w{p, NowMs()};
    if (q.empty() && Spend(sender, p.op)) {
        if (HostTakeOp(s, w, sender) == Take::Done) return;
        Refund(sender, p.op);
    }
    if (q.size() >= kMaxQueued) {
        HostRefuseQueue(s, sender, w);
        return;
    }
    q.push_back(w);
}

void HostDrainWaiting(coop::net::Session* s) {
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        auto& q = g_waiting[slot];
        while (!q.empty() && Spend(slot, q.front().p.op)) {
            if (HostTakeOp(s, q.front(), slot) == Take::Wait) {
                Refund(slot, q.front().p.op);
                break;
            }
            q.pop_front();
        }
    }
}

// The rows each owed joiner waits for, once the generators resolve.
void HostServeOwed(coop::net::Session* s) {
    if (!g_owed || !GEN::EnsureResolved()) return;
    PowerGridPayload probe{};
    if (!ReadRows(probe)) return;
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot)
        if (g_owed & (1u << slot)) HostSendRows(s, slot);
    g_owed = 0;
}

// ---- a client ------------------------------------------------------------------------------------------------

void ClientSendOp(coop::net::Session* s, uint8_t op, int32_t index, float damage = 0.f) {
    coop::power_puzzle::FlushInputs();  // an op rests on the inputs before it
    PowerGridPayload p{};
    p.op = op;
    p.index = static_cast<uint8_t>(index);
    p.damage = damage;
    if (Predicted(op)) {
        p.seq = NextSeq();
        g_pending.push_back({p.seq, op, p.index});
    }
    s->SendReliableToSlot(0, coop::net::ReliableKind::PowerGridState, &p, sizeof(p));
    ++g_opsSent;
    if (op != coop::net::kPowerGridOpHit || ++g_hitsSent <= 3 || g_hitsSent % 20 == 0)
        UE_LOGI("power_grid: local %s of generator %d (seq %u) -- sent to the host", OpName(op), index, p.seq);
}

// The host's row with my untaken ops on top: the row the host will send once it takes them.
GEN::Row Expected(const coop::net::PowerGridRow& w, int32_t index) {
    GEN::Row e{};
    e.broken = w.broken != 0;
    e.cyc = w.cyc != 0;
    e.cycle = w.cycle;
    e.upgradeLevel = w.upgradeLevel;
    for (const Pending& d : g_pending) {
        if (d.index != index) continue;
        if (d.op == coop::net::kPowerGridOpActivate) {
            e.broken = false;  // a broken one mended, a whole one serviced: the wear full either way
            e.cycle = kFullWear;
        } else if (d.op == coop::net::kPowerGridOpUpgrade && e.upgradeLevel < kMaxUpgrade) {
            ++e.upgradeLevel;
        }
    }
    return e;
}

// Bring each generator to the host's row with my untaken ops on top, through the game's own verbs, run as the
// host's so this client's gates pass them.
void ClientReconcile() {
    std::vector<void*> gens;
    if (GEN::ReadGenerators(gens) == 0) {
        g_rowsWaiting = true;
        return;
    }
    g_rowsWaiting = false;
    g_applying = true;
    bool panelVerbs = false;
    const size_t n = std::min({gens.size(), static_cast<size_t>(g_rows.count),
                               static_cast<size_t>(kPowerGridGenerators)});
    for (size_t i = 0; i < n; ++i) {
        const coop::net::PowerGridRow& w = g_rows.rows[i];
        GEN::Row r{};
        if (!w.present || !gens[i] || !GEN::ReadRow(gens[i], r)) continue;
        const GEN::Row e = Expected(w, static_cast<int32_t>(i));
        if (e.broken != r.broken) {
            // A break blacks the base out through the panel and scrambles the puzzle; a repair runs as the host
            // runs a player's, its turn-on at the generator included.
            if (RedApply()) GEN::WriteBroken(gens[i], e.broken);
            else if (e.broken) GEN::CallBreak(gens[i]);
            else GEN::Repair(gens[i]);
            panelVerbs = true;
            UE_LOGI("power_grid: generator %zu %s as the host's", i, e.broken ? "broke" : "was repaired");
            GEN::ReadRow(gens[i], r);
        }
        if (e.cycle != r.cycle) {
            // One step of the host's wear runs as its damage(), whose scrambled sine page puts this copy's next
            // service a solved puzzle away, as the host's is; any other gap (a join, a service) is written.
            if (!RedApply() && !e.broken && !r.broken && e.cycle >= 1 && e.cycle == r.cycle - 1)
                GEN::CallDamage(gens[i]);
            else
                GEN::WriteCycle(gens[i], e.cycle);
        }
        if (e.cyc != r.cyc) GEN::WriteCyc(gens[i], e.cyc);
        if (e.upgradeLevel != r.upgradeLevel) {
            GEN::WriteUpgradeLevel(gens[i], e.upgradeLevel);
            if (!RedApply()) GEN::CallUpdUpgrades(gens[i]);
            UE_LOGI("power_grid: generator %zu upgrades %d -> %d as the host's", i, r.upgradeLevel, e.upgradeLevel);
        }
    }
    // A break's solar() cleared this panel's breakers along with the power; only the canonical writes them, and a
    // rolled-back repair's break has no canonical behind it.
    if (panelVerbs) coop::power_panel::ReassertCanonical();
    // The puzzles last: the verbs above may have solved one (fullFix), and the host's is the one it holds.
    coop::power_puzzle::Reconcile(gens);
    g_applying = false;
    ++g_rowsApplied;
}

void ClientTakeRows(const PowerGridPayload& p) {
    const uint8_t me = LocalSlot();
    const uint16_t ack = me < coop::net::kMaxPeers ? p.ack[me] : 0;
    size_t kept = 0;
    for (const Pending& d : g_pending)
        if (SeqAfter(d.seq, ack)) g_pending[kept++] = d;
    g_pending.resize(kept);
    coop::power_puzzle::OnRows(p, ack);
    g_rows = p;
    g_haveRows = true;
    if (GEN::EnsureResolved()) ClientReconcile();
    else g_rowsWaiting = true;
}

// ---- the watches ---------------------------------------------------------------------------------------------

// A client's own break, wear and fullFix never run: only the host's world breaks and wears a generator, and the
// host's rows run each edge here as the host's.
sg::Verdict OnOwnEditPre(const sg::Call& call) {
    const int verb = call.tag == 0x50474252 /*'PGBR'*/ ? kVerbBreak : call.tag == 0x5047444D /*'PGDM'*/ ? kVerbDamage
                                                                                                      : kVerbFullFix;
    RecordVerb(verb, call.function);
    auto* s = Connected();
    if (!IsClient(s) || call.fromOurCode) return sg::Verdict::Run;
    if (!call.object || !coop::net_pump::IsInAnnouncedWorld(call.object)) return sg::Verdict::Run;
    if (++g_editsRefused == 1)
        UE_LOGI("power_grid: this client refuses its own generator break, wear and fullFix -- the host's "
                "generators are the author");
    return sg::Verdict::Cancel;
}

// The host's own edges: the outermost one's exit sends the rows when they changed.
void OnGenHostPost(const sg::Call& call) {
    auto* s = Connected();
    if (!s || s->role() != coop::net::Role::Host) return;
    for (const auto& ref : g_verbFn) {
        void* const fn = ref.Get();
        if (fn && fn != call.function && sg::IsBodyActive(fn)) return;  // an outer verb's exit sends them
    }
    HostSendRows(s);
}

// upd(): recorded only, for the nesting above; its exit is a host's rows.
sg::Verdict OnUpdPre(const sg::Call& call) {
    RecordVerb(kVerbUpd, call.function);
    return sg::Verdict::Run;
}

// A client's install: the insert raised the level and spent the held upgrade before its updUpgrades, a level
// above the one expected; each step goes to the host.
sg::Verdict OnUpdUpgradesPre(const sg::Call& call) {
    RecordVerb(kVerbUpdUpgrades, call.function);
    auto* s = Connected();
    if (!IsClient(s) || g_applying || !g_haveRows) return sg::Verdict::Run;
    if (!call.object || !coop::net_pump::IsInAnnouncedWorld(call.object)) return sg::Verdict::Run;
    const int32_t idx = GEN::IndexOf(call.object);
    GEN::Row r{};
    if (idx < 0 || idx >= kPowerGridGenerators || idx >= g_rows.count || !g_rows.rows[idx].present ||
        !GEN::ReadRow(call.object, r))
        return sg::Verdict::Run;
    const int32_t level = std::clamp(r.upgradeLevel, 0, kMaxUpgrade);
    for (int32_t lvl = Expected(g_rows.rows[idx], idx).upgradeLevel; lvl < level; ++lvl)
        ClientSendOp(s, coop::net::kPowerGridOpUpgrade, idx);
    return sg::Verdict::Run;
}

// A client's Activate press, as it begins: its generator and that generator's row.
sg::Verdict OnActivatePre(const sg::Call& call) {
    RecordVerb(kVerbActivate, call.function);
    if (!IsClient(Connected()) || !call.object) return sg::Verdict::Run;
    GEN::Row r{};
    if (GEN::ReadRow(call.object, r)) {
        g_pressGen = call.object;
        g_pressRow = r;
    }
    return sg::Verdict::Run;
}

// ...and as it ends. A broken generator it left whole was repaired, a whole one whose wear it restored was
// serviced: either ran here as the prediction, and goes to the host as the press, whose branch the host's own
// generator decides. The host's own press sends the rows.
void OnActivatePost(const sg::Call& call) {
    auto* s = Connected();
    if (!s || !call.object) return;
    if (s->role() == coop::net::Role::Host) {
        OnGenHostPost(call);
        return;
    }
    if (call.object != g_pressGen) return;
    g_pressGen = nullptr;
    if (!coop::net_pump::IsInAnnouncedWorld(call.object)) return;
    const int32_t idx = GEN::IndexOf(call.object);
    GEN::Row r{};
    if (idx < 0 || idx >= kPowerGridGenerators || !GEN::ReadRow(call.object, r)) return;
    if ((g_pressRow.broken && !r.broken) || (!g_pressRow.broken && !r.broken && r.cycle > g_pressRow.cycle))
        ClientSendOp(s, coop::net::kPowerGridOpActivate, idx);
}

// A client's own hit: its player's goes to the host, whose world breaks the generator; every other hit, a
// creature's or an explosion's, is the host's world's and runs there (door_verb_intent's hit, the precedent).
sg::Verdict OnHitPre(const sg::Call& call) {
    auto* s = Connected();
    if (!IsClient(s) || call.fromOurCode) return sg::Verdict::Run;
    if (!call.object || !coop::net_pump::IsInAnnouncedWorld(call.object)) return sg::Verdict::Run;
    if (g_hitParams.fn != call.function) {
        g_hitParams.fn = call.function;
        g_hitParams.actor = R::FindParamOffset(call.function, L"actor");
        g_hitParams.damage = R::FindParamOffset(call.function, L"damage");
        if (g_hitParams.actor < 0 || g_hitParams.damage < 0)
            UE_LOGW("power_grid: the generator's addDamage parameters did not resolve (actor=%d damage=%d) -- a "
                    "client's hit on a generator is lost", g_hitParams.actor, g_hitParams.damage);
    }
    if (g_hitParams.actor < 0 || g_hitParams.damage < 0) return sg::Verdict::Cancel;
    void* actor = *reinterpret_cast<void* const*>(call.locals + g_hitParams.actor);
    const float damage = *reinterpret_cast<const float*>(call.locals + g_hitParams.damage);
    const int32_t idx = GEN::IndexOf(call.object);
    if (coop::players::Registry::Get().IsLocal(actor) && idx >= 0 && idx < kPowerGridGenerators)
        ClientSendOp(s, coop::net::kPowerGridOpHit, idx, damage);
    return sg::Verdict::Cancel;
}

UE_FACT_FN(kRowBreak, "power_grid", L"generator_C", L"break", Declared);
UE_FACT_FN(kRowDamage, "power_grid", L"generator_C", L"damage", Declared);
UE_FACT_FN(kRowFullFix, "power_grid", L"generator_C", L"fullFix", Declared);
UE_FACT_FN(kRowUpdUpgrades, "power_grid", L"generator_C", L"updUpgrades", Declared);
UE_FACT_FN(kRowUpd, "power_grid", L"generator_C", L"upd", Declared);
UE_FACT_FN(kRowActionOptionIndex, "power_grid", L"generator_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowAddDamage, "power_grid", L"generator_C", L"addDamage", Declared);

struct WatchDef {
    const ue_wrap::facts::Function* row;
    int tag;
    sg::PreFn pre;
    sg::PostFn post;
};
constexpr WatchDef kWatches[] = {
    { &kRowBreak,             0x50474252 /*'PGBR'*/, &OnOwnEditPre,     &OnGenHostPost },
    { &kRowDamage,            0x5047444D /*'PGDM'*/, &OnOwnEditPre,     &OnGenHostPost },
    { &kRowFullFix,           0x50474658 /*'PGFX'*/, &OnOwnEditPre,     &OnGenHostPost },
    { &kRowUpdUpgrades,       0x50475550 /*'PGUP'*/, &OnUpdUpgradesPre, &OnGenHostPost },
    { &kRowUpd,               0x50475544 /*'PGUD'*/, &OnUpdPre,         &OnGenHostPost },
    { &kRowActionOptionIndex, 0x5047414F /*'PGAO'*/, &OnActivatePre,    &OnActivatePost },
    { &kRowAddDamage,         0x50474144 /*'PGAD'*/, &OnHitPre,         nullptr },
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));
bool g_registered = false;
bool g_settled = false;

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    coop::power_decay::Install(session);
    coop::power_puzzle::Install(session);
    coop::power_upgrade::Install(session);
    if (g_registered) return;
    g_registered = true;
    for (const WatchDef& w : kWatches)
        if (!sg::WatchName(*w.row, w.tag, w.pre, w.post))
            UE_LOGE("power_grid: the script-body gate refused the watch on %ls::%ls -- that edge is each peer's own",
                    w.row->Class(), w.row->Name());
}

void Tick() {
    if (auto* s = Connected(); s && s->role() == coop::net::Role::Host) {
        HostDrainWaiting(s);
        HostServeOwed(s);
        if (g_puzzleDirty && NowMs() - g_lastRowsMs >= kPuzzleRowsEveryMs) HostSendRows(s);
    } else if (g_rowsWaiting && GEN::EnsureResolved()) {
        ClientReconcile();
    }
    coop::power_decay::Tick();
    coop::power_puzzle::Tick();
    if (g_settled || !g_registered) return;
    sg::ResolvePendingNames();
    int live = 0, settled = 0;
    for (const WatchDef& w : kWatches) {
        if (sg::NameWatchLive(*w.row, w.tag)) ++live;
        if (sg::NameWatchSettled(*w.row, w.tag)) ++settled;
    }
    if (live == kWatchCount) {
        g_settled = true;
        UE_LOGI("power_grid: the grid's gates are live (break, wear, fullFix, upgrades, upd, the Activate press, a "
                "hit)");
    } else if (settled == kWatchCount) {
        g_settled = true;
        UE_LOGE("power_grid: %d of %d grid gates are dead -- those edges stay each peer's own", kWatchCount - live,
                kWatchCount);
    }
}

void OnReliable(const coop::net::PowerGridPayload& payload, uint8_t senderSlot) {
    auto* s = Connected();
    if (!s) return;
    if (s->role() == coop::net::Role::Host) {
        HostOffer(s, payload, senderSlot);
        return;
    }
    if (senderSlot != 0 || payload.op != coop::net::kPowerGridOpRows) return;
    ClientTakeRows(payload);
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = Connected();
    if (!s || s->role() != coop::net::Role::Host || slot <= 0 || slot >= coop::net::kMaxPeers) return;
    g_owed |= static_cast<uint8_t>(1u << slot);
    HostServeOwed(s);
    if (g_owed & (1u << slot))
        UE_LOGI("power_grid: slot %d's rows wait for the host's generators to resolve", slot);
}

uint16_t NextSeq() { return ++g_seq; }

void HostPuzzleChanged() { g_puzzleDirty = true; }

void OnPeerLeft(uint8_t slot) {
    if (slot == 0 || slot >= coop::net::kMaxPeers) return;
    g_waiting[slot].clear();
    g_rate[slot].Reset();
    g_inputRate[slot].Reset();
    g_owed = static_cast<uint8_t>(g_owed & ~(1u << slot));
}

size_t PendingOps() { return g_pending.size(); }
uint64_t ClientOpsSent() { return g_opsSent; }
uint64_t HostOpsTaken() { return g_opsTaken; }
uint64_t HostOpsRefused() { return g_opsRefused; }
uint64_t HostInputsTaken() { return g_inputsTaken; }
uint64_t HostRowsBroadcast() { return g_rowsBroadcast; }

void DevRefuseInstalls(bool on) {
    if (on != g_devRefuseInstalls) UE_LOGI("power_grid: [dev] the host %s installs", on ? "refuses" : "judges");
    g_devRefuseInstalls = on;
}

void DevSendInstall(int32_t index) {
    if (auto* s = Connected(); IsClient(s)) ClientSendOp(s, coop::net::kPowerGridOpUpgrade, index);
}

bool LastRows(coop::net::PowerGridPayload& out) {
    if (!g_haveRows) return false;
    out = g_rows;
    return true;
}

void OnDisconnect() {
    coop::power_decay::OnDisconnect();
    coop::power_puzzle::OnDisconnect();
    coop::power_upgrade::OnDisconnect();
    g_devRefuseInstalls = false;
    if (g_editsRefused || g_opsSent || g_rowsApplied || g_opsTaken || g_inputsTaken || g_opsRefused)
        UE_LOGI("power_grid: session end -- own edits refused %llu, ops sent %llu, rows applied %llu; as host: ops "
                "taken %llu, puzzle inputs taken %llu, refused %llu, rows broadcast %llu",
                static_cast<unsigned long long>(g_editsRefused), static_cast<unsigned long long>(g_opsSent),
                static_cast<unsigned long long>(g_rowsApplied), static_cast<unsigned long long>(g_opsTaken),
                static_cast<unsigned long long>(g_inputsTaken), static_cast<unsigned long long>(g_opsRefused),
                static_cast<unsigned long long>(g_rowsBroadcast));
    g_editsRefused = g_opsSent = g_hitsSent = g_rowsApplied = 0;
    g_opsTaken = g_opsRefused = g_noisyAnswered = g_inputsTaken = g_queuesFull = g_rowsBroadcast = 0;
    g_haveSent = g_ackDirty = g_puzzleDirty = false;
    g_lastRowsMs = 0;
    for (int slot = 0; slot < coop::net::kMaxPeers; ++slot) {
        g_waiting[slot].clear();
        g_rate[slot].Reset();
        g_inputRate[slot].Reset();
    }
    for (auto& ref : g_verbFn) ref.Reset();
    g_owed = 0;
    g_pending.clear();
    g_seq = 0;
    g_haveRows = g_rowsWaiting = false;
    g_pressGen = nullptr;
}

}  // namespace coop::power_grid
