// coop/interactables/serverbox_sync.cpp -- see coop/interactables/serverbox_sync.h.
//
// The engine's break state -- the box's IsBroken, damaged and minigame, the notify-free check() it re-skins from, the
// farm's three totals, the verbs and the repair widget -- is ue_wrap/devices/serverbox's; this lane owns the wire half:
// the row, its width, the poll, the repair intent and who may author any of it.

#include "coop/interactables/serverbox_sync.h"

#include "coop/element/intent_authority.h"  // IntentTarget: a repair's presser within reach of its box
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // kMaxPeers
#include "coop/player/roster_ledger.h"     // PerSlotState

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/serverbox.h"
#include "ue_wrap/world/economy.h"           // AddPoints and the saveSlot: the repair reward, paid on the host
#include "ue_wrap/engine/world_identity.h"   // Generation, the baseline's anchor
#include "ue_wrap/world/world_singleton.h"   // Gamemode, its other anchor

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace coop::serverbox_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace SB = ue_wrap::serverbox;
namespace sg = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};
Counts g_counts;

constexpr int       kMaxServers     = 64;    // the row's width; a base runs ~54 boxes
constexpr long long kPollIntervalMs = 1000;
constexpr float     kRepairReachUU  = 400.0f;  // the repair widget is used standing at the box
constexpr auto      kSayEvery       = std::chrono::seconds(10);

const wchar_t* const kBoxClass  = L"serverBox_C";
const wchar_t* const kBreakVerb = L"breakServer";
const wchar_t* const kTypeVerb  = L"break_type";
const wchar_t* const kFixVerb   = L"fix";
constexpr int kTagBreak = 0x53424231;  // 'SBB1'
constexpr int kTagType  = 0x53425432;  // 'SBT2'
constexpr int kTagFix   = 0x53424633;  // 'SBF3'

coop::net::Session* ClientSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() && s->role() == coop::net::Role::Client ? s : nullptr;
}
coop::net::Session* HostSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() && s->role() == coop::net::Role::Host ? s : nullptr;
}

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The box list has one owner, ue_wrap/devices/serverbox: this lane keeps only its own cap, the row's width.
int32_t ReadServers(std::vector<void*>& out) {
    out.clear();
    const int32_t num = static_cast<int32_t>(SB::ReadServers(out));
    if (num > kMaxServers) out.resize(kMaxServers);
    return num;
}

// The current row from the live gamemode. False if not readable yet. The repair group's fields ride when it resolved.
bool ReadState(coop::net::ServerStatePayload& p) {
    SB::Aggregates agg;
    if (!SB::ReadAggregates(agg)) return false;
    std::vector<void*> servers;
    const int32_t num = ReadServers(servers);
    if (num > kMaxServers) {
        static bool warned = false;
        if (!warned) { warned = true; UE_LOGW("serverbox_sync: %d servers > cap %d -- syncing first %d only",
                                              num, kMaxServers, kMaxServers); }
    }
    std::memset(&p, 0, sizeof(p));
    const bool repair = SB::EnsureRepairResolved();
    for (size_t i = 0; i < servers.size(); ++i) {
        void* sb = servers[i];
        if (!sb || !R::IsLive(sb)) continue;
        if (SB::ReadIsBroken(sb)) p.isBrokenMask |= (1ull << i);
        SB::RepairState r;
        if (repair && SB::ReadRepairState(sb, r)) {
            if (r.damaged) p.damagedMask |= (1ull << i);
            p.minigame[i] = static_cast<uint8_t>(r.minigame);
        }
    }
    p.brokenServers = agg.brokenServers;
    p.effCalc  = agg.efficiencyCalc;
    p.effDownl = agg.efficiencyDownload;
    p.serverCount = static_cast<uint8_t>(servers.size());
    return true;
}

// ---- host poll baseline ---------------------------------------------------------------------------
uint32_t g_polledWorldGen = 0;
void* g_polledGm = nullptr;  // the gamemode the baseline was read from; an identity, never dereferenced
bool  g_primed = false;
coop::net::ServerStatePayload g_last{};
long long g_lastPollMs = 0;

// The flags, the types and the broken count are the edges; the efficiencies also move off one (a download ramps
// serverEfficiency_downl), so they count with a small epsilon and the SAT console's sv.*/tw.* reads stay fresh.
bool StateChanged(const coop::net::ServerStatePayload& p) {
    return p.isBrokenMask != g_last.isBrokenMask || p.damagedMask != g_last.damagedMask ||
           p.brokenServers != g_last.brokenServers || p.serverCount != g_last.serverCount ||
           std::memcmp(p.minigame, g_last.minigame, sizeof(p.minigame)) != 0 ||
           std::fabs(p.effCalc  - g_last.effCalc)  > 0.005f ||
           std::fabs(p.effDownl - g_last.effDownl) > 0.005f;
}

// HOST: the row to every client, on a change of the baseline.
void BroadcastIfChanged(coop::net::Session* s, const char* why) {
    coop::net::ServerStatePayload p{};
    if (!ReadState(p) || !StateChanged(p)) return;
    g_last = p;
    if (s->SendReliable(coop::net::ReliableKind::ServerState, &p, sizeof(p)))
        UE_LOGI("serverbox_sync: host broadcast, %s (broken=%d mask=0x%llX damaged=0x%llX count=%d)", why,
                p.brokenServers, static_cast<unsigned long long>(p.isBrokenMask),
                static_cast<unsigned long long>(p.damagedMask), p.serverCount);
    else
        UE_LOGW("serverbox_sync: host broadcast send FAILED (broken=%d mask=0x%llX)", p.brokenServers,
                static_cast<unsigned long long>(p.isBrokenMask));
}

bool SendStateTo(coop::net::Session* s, int slot) {
    coop::net::ServerStatePayload p{};
    return ReadState(p) && s->SendReliableToSlot(slot, coop::net::ReliableKind::ServerState, &p, sizeof(p));
}

// ---- client apply ---------------------------------------------------------------------------------
void ApplyState(const coop::net::ServerStatePayload& p) {
    // The totals, so the SAT console's sv.*/tw.* queries read the host's.
    SB::Aggregates agg;
    agg.brokenServers      = p.brokenServers;
    agg.efficiencyCalc     = p.effCalc;
    agg.efficiencyDownload = p.effDownl;
    if (!SB::WriteAggregates(agg)) return;
    std::vector<void*> servers;
    ReadServers(servers);
    const bool repair = SB::EnsureRepairResolved();
    int reskinned = 0;
    const int32_t n = static_cast<int32_t>(servers.size());
    const int32_t take = n < p.serverCount ? n : p.serverCount;
    for (int32_t i = 0; i < take && i < kMaxServers; ++i) {
        void* sb = servers[i];
        if (!sb || !R::IsLive(sb)) continue;
        if (repair) {
            // What the repair widget reads: the type it enters with, and whether the repair pays.
            SB::RepairState r;
            r.damaged = (p.damagedMask >> i) & 1ull;
            r.minigame = p.minigame[i];
            SB::WriteRepairState(sb, r);
        }
        const bool desired = (p.isBrokenMask >> i) & 1ull;
        if (SB::ReadIsBroken(sb) == desired) continue;   // already matches -> no re-skin
        if (SB::ApplyBreak(sb, desired)) ++reskinned;
    }
    if (reskinned)
        UE_LOGI("serverbox_sync: client applied host state (broken=%d mask=0x%llX, %d server(s) re-skinned)",
                p.brokenServers, static_cast<unsigned long long>(p.isBrokenMask), reskinned);
}

// ---- the verbs at the gate ------------------------------------------------------------------------
std::chrono::steady_clock::time_point g_nextBreakSay{}, g_nextFixSay{};
coop::roster_ledger::PerSlotState<std::chrono::steady_clock::time_point> g_nextRefusedSay;

bool SayNow(std::chrono::steady_clock::time_point& next) {
    const auto now = std::chrono::steady_clock::now();
    if (now < next) return false;
    next = now + kSayEvery;
    return true;
}

std::wstring CallerName(const sg::Call& call) {
    return call.callerFunction ? R::ToString(R::NameOf(call.callerFunction)) : std::wstring(L"ProcessEvent");
}

// CLIENT: a break is the host's world event or its save's; the host's row brings it.
sg::Verdict OnBreakPre(const sg::Call& call) {
    if (!ClientSession()) return sg::Verdict::Run;
    ++g_counts.refusedBreaks;
    if (SayNow(g_nextBreakSay))
        UE_LOGI("serverbox_sync: this client's own %ls refused (called from %ls; %llu refused) -- the break is the "
                "host's", call.tag == kTagType ? kTypeVerb : kBreakVerb, CallerName(call).c_str(),
                static_cast<unsigned long long>(g_counts.refusedBreaks));
    return sg::Verdict::Cancel;
}

// The minigame's reward, read from ui_serverMinigame's end(correct): a box broken by damage pays nothing;
// any other pays SelectInt(50, 15, isLol) and runs fix; then a solve time under saveSlot.servertimeBest
// pays SelectInt(100, 30, isLol) more, and the best becomes the time (the first solve only sets it).
constexpr int32_t kRewardPlain = 15, kRewardLol = 50, kRecordPlain = 30, kRecordLol = 100;

// CLIENT: the widget's own fields the reward reads, at the moment its fix is sent.
void ReadWidgetReward(void* widget, coop::net::ServerRepairPayload& p) {
    static void* sCls = nullptr;
    static int32_t sOffLol = -1, sOffTime = -1;
    static uint8_t sMaskLol = 0;
    void* cls = R::ClassOf(widget);
    if (cls != sCls) {
        sCls = cls;
        sOffLol = -1; sOffTime = -1; sMaskLol = 0;
        if (!R::FindBoolProperty(cls, L"isLol", sOffLol, sMaskLol)) sOffLol = -1;
        sOffTime = R::FindPropertyOffset(cls, L"time");
    }
    if (sOffLol < 0 || sOffTime < 0) {
        UE_LOGW("serverbox_sync: the repair widget's isLol/time did not resolve -- the host fixes without the reward");
        return;
    }
    const auto* base = static_cast<const uint8_t*>(widget);
    float t = 0.f;
    std::memcpy(&t, base + sOffTime, sizeof(t));
    p.flags = static_cast<uint8_t>(coop::net::kRepairSettles |
                                   ((base[sOffLol] & sMaskLol) ? coop::net::kRepairLol : 0));
    const float ds = std::isfinite(t) && t > 0.f ? t * 10.f + 0.5f : 0.f;
    p.timeDs = static_cast<uint16_t>(ds >= 65535.f ? 65535.f : ds);
}

// CLIENT: the widget's points are the host's to pay with the fix, so its own credit is refused here: the
// shared balance would take it, then drop it when the host's row lands, and the host's later payment showed
// as the difference (a +15 reward, then +60 from the task, read +45).
uint64_t g_rewardsRefused = 0;
std::chrono::steady_clock::time_point g_nextRewardSay{};
sg::Verdict OnRewardPre(const sg::Call& call) {
    if (!ClientSession() || !SB::IsRepairWidget(call.callerObject)) return sg::Verdict::Run;
    ++g_rewardsRefused;
    if (SayNow(g_nextRewardSay))
        UE_LOGI("serverbox_sync: this client's repair reward refused (%llu) -- the host pays it with the fix",
                static_cast<unsigned long long>(g_rewardsRefused));
    return sg::Verdict::Cancel;
}

// HOST: the reward of a client's repair the host just ran, as the widget computes it, from the host's own
// box and saveSlot. Returns the base reward; `record` gets the record bonus.
int32_t PayRepairReward(const coop::net::ServerRepairPayload& p, bool damaged, int32_t& record) {
    record = 0;
    if (!(p.flags & coop::net::kRepairSettles) || damaged) return 0;
    const bool lol = (p.flags & coop::net::kRepairLol) != 0;
    const int32_t reward = lol ? kRewardLol : kRewardPlain;
    ue_wrap::economy::AddPoints(reward);
    if (p.timeDs == 0) return reward;
    void* save = ue_wrap::economy::SaveSlotPtr();
    static int32_t sOffBest = -1;
    if (save && sOffBest < 0) sOffBest = R::FindPropertyOffset(R::ClassOf(save), L"servertimeBest");
    if (!save || sOffBest < 0) return reward;
    float best = 0.f;
    std::memcpy(&best, static_cast<uint8_t*>(save) + sOffBest, sizeof(best));
    const float t = static_cast<float>(p.timeDs) / 10.f;
    if (t < best) {
        record = lol ? kRecordLol : kRecordPlain;
        ue_wrap::economy::AddPoints(record);
    }
    const float next = best == 0.f ? t : std::fmin(best, t);
    std::memcpy(static_cast<uint8_t*>(save) + sOffBest, &next, sizeof(next));
    return reward;
}

// CLIENT: its player's repair, the gamemode's repair widget calling fix, goes to the host; any other fix is refused.
sg::Verdict OnFixPre(const sg::Call& call) {
    auto* s = ClientSession();
    if (!s) return sg::Verdict::Run;
    if (!SB::IsRepairWidget(call.callerObject)) {
        ++g_counts.refusedFixes;
        if (SayNow(g_nextFixSay))
            UE_LOGI("serverbox_sync: this client's own fix refused (called from %ls; %llu refused) -- a repair is the "
                    "host's", CallerName(call).c_str(), static_cast<unsigned long long>(g_counts.refusedFixes));
        return sg::Verdict::Cancel;
    }
    const int32_t box = SB::IndexOf(call.object);
    if (box < 0 || box >= kMaxServers) {
        UE_LOGW("serverbox_sync: this player's repair names a box outside the server list (index %d) -- not sent", box);
        return sg::Verdict::Cancel;
    }
    coop::net::ServerRepairPayload p{};
    p.box = static_cast<uint8_t>(box);
    ReadWidgetReward(call.callerObject, p);
    if (s->SendReliableToSlot(0, coop::net::ReliableKind::ServerRepair, &p, sizeof(p))) ++g_counts.repairsSent;
    UE_LOGI("serverbox_sync: this player's repair of box %d sent to the host (%llu sent)", box,
            static_cast<unsigned long long>(g_counts.repairsSent));
    return sg::Verdict::Cancel;
}

struct Watch {
    const wchar_t* fn;
    int            tag;
    sg::PreFn      pre;
    bool           registered = false;
    bool           settled = false;
    const wchar_t* cls = kBoxClass;
};
Watch g_watches[] = {
    {kBreakVerb, kTagBreak, &OnBreakPre},
    {kTypeVerb, kTagType, &OnBreakPre},
    {kFixVerb, kTagFix, &OnFixPre},
    {L"addPoints", 0x53425034 /* 'SBP4' */, &OnRewardPre, false, false, L"lib_C"},
};
bool g_watchesSettled = false;

void DriveWatches() {
    if (g_watchesSettled) return;
    sg::ResolvePendingNames();
    bool all = true;
    for (Watch& w : g_watches) {
        if (w.settled) continue;
        if (!w.registered) {
            w.registered = sg::WatchClassName(w.cls, w.fn, w.tag, w.pre, nullptr);
            if (!w.registered) {
                w.settled = true;
                UE_LOGE("serverbox_sync: the gate took no watch on %ls::%ls -- a client authors that verb", w.cls,
                        w.fn);
                continue;
            }
        }
        if (sg::ClassNameWatchSettled(w.cls, w.fn, w.tag)) {
            w.settled = true;
            if (sg::ClassNameWatchLive(w.cls, w.fn, w.tag)) UE_LOGI("serverbox_sync: the watch on %ls::%ls is live",
                                                                     w.cls, w.fn);
            else UE_LOGE("serverbox_sync: the watch on %ls::%ls settled dead -- a client authors that verb", w.cls,
                         w.fn);
            continue;
        }
        all = false;
    }
    g_watchesSettled = all;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    DriveWatches();
}

void Tick() {
    if (!GT::IsGameThread()) return;
    DriveWatches();
    auto* s = HostSession();
    if (!s) return;
    const long long now = NowMs();
    if (now - g_lastPollMs < kPollIntervalMs) return;
    g_lastPollMs = now;
    if (!SB::EnsureBreakResolved()) return;
    // A world or save reload minted a new gamemode with its world -> the baseline is meaningless; re-prime silently
    // (a prime must never masquerade as an edge; the join edge and the next real transition deliver state). The
    // gamemode is the anchor as much as the generation: in a travel the world reads unknown for a while, and a
    // gamemode swapped inside it keeps the generation.
    const uint32_t gen = ue_wrap::world_identity::Generation();
    void* const gm = ue_wrap::world_singleton::Gamemode();
    if (gen != g_polledWorldGen || gm != g_polledGm || !g_primed) {
        coop::net::ServerStatePayload p{};
        if (!ReadState(p)) return;
        g_polledWorldGen = gen; g_polledGm = gm; g_primed = true; g_last = p;
        return;
    }
    BroadcastIfChanged(s, "a change");
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = HostSession();
    if (!s || slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    if (!SB::EnsureBreakResolved()) return;  // no world yet -> the first transition delivers state
    // Unconditional (even all-healthy): the joiner's load ran none of the verbs; this row is its state.
    if (SendStateTo(s, slot)) UE_LOGI("serverbox_sync: connect-snapshot sent to slot %d", slot);
    else UE_LOGW("serverbox_sync: connect-snapshot to slot %d send FAILED", slot);
}

void OnReliable(const coop::net::ServerStatePayload& payload, int senderPeerSlot) {
    if (!GT::IsGameThread()) {
        UE_LOGW("serverbox_sync: OnReliable off-game-thread -- dropping");
        return;
    }
    if (!ClientSession()) return;
    if (senderPeerSlot != 0) {
        UE_LOGW("serverbox_sync: ServerState from non-host senderPeerSlot=%d -- dropping", senderPeerSlot);
        return;
    }
    if (!SB::EnsureBreakResolved()) {
        UE_LOGW("serverbox_sync: ServerState arrived before resolution -- dropped (the next host change / "
                "connect-snapshot re-delivers)");
        return;
    }
    ApplyState(payload);
}

void OnRepair(const coop::net::ServerRepairPayload& payload, int senderPeerSlot) {
    if (!GT::IsGameThread()) {
        UE_LOGW("serverbox_sync: OnRepair off-game-thread -- dropping");
        return;
    }
    auto* s = HostSession();
    if (!s || senderPeerSlot < 1 || senderPeerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    if (!SB::EnsureBreakResolved() || !SB::EnsureRepairResolved()) return;
    const uint8_t slot = static_cast<uint8_t>(senderPeerSlot);
    std::vector<void*> servers;
    ReadServers(servers);
    void* box = payload.box < servers.size() ? servers[payload.box] : nullptr;
    const char* why = nullptr;
    SB::RepairState repair{};   // the damage flag the reward reads, taken before the fix clears it
    if (!box || !R::IsLive(box)) why = "no such box";
    else if (!SB::ReadIsBroken(box)) why = "the box is not broken here";
    else {
        const auto token = coop::element::IntentTarget::ForClientIntent(*s, slot, kRepairReachUU);
        if (!token.HasBody()) why = "the host has no body for that player";
        else if (const auto outcome = token.Authorize(box).outcome; outcome != coop::element::IntentOutcome::Ok)
            why = outcome == coop::element::IntentOutcome::NoTarget ? "the box's place did not read"
                                                                      : "the box is out of its reach";
        else if (!SB::ReadRepairState(box, repair) || !SB::CallFix(box)) why = "the box's fix did not run";
    }
    if (why) {
        ++g_counts.repairsRefused;
        SendStateTo(s, slot);
        if (SayNow(g_nextRefusedSay[slot]))
            UE_LOGI("serverbox_sync: slot %u's repair of box %u refused (%s) -- answered with the host's row", slot,
                    payload.box, why);
        return;
    }
    ++g_counts.repairsRun;
    int32_t record = 0;
    const int32_t reward = PayRepairReward(payload, repair.damaged, record);
    UE_LOGI("serverbox_sync: HOST ran slot %u's repair of box %u (reward %d, record %d%s)", slot, payload.box,
            reward, record, (payload.flags & coop::net::kRepairSettles) ? "" : ", the sender sent no reward fields");
    BroadcastIfChanged(s, "a client's repair");
}

void OnDisconnect() {
    if (g_counts.refusedBreaks || g_counts.refusedFixes || g_counts.repairsSent || g_counts.repairsRun ||
        g_counts.repairsRefused)
        UE_LOGI("serverbox_sync: session end -- refused breaks=%llu fixes=%llu, repairs sent=%llu run=%llu "
                "refused=%llu", static_cast<unsigned long long>(g_counts.refusedBreaks),
                static_cast<unsigned long long>(g_counts.refusedFixes),
                static_cast<unsigned long long>(g_counts.repairsSent),
                static_cast<unsigned long long>(g_counts.repairsRun),
                static_cast<unsigned long long>(g_counts.repairsRefused));
    g_counts = Counts{};
    g_polledWorldGen = 0; g_polledGm = nullptr; g_primed = false;
    g_last = coop::net::ServerStatePayload{};
    g_lastPollMs = 0;
    g_session.store(nullptr, std::memory_order_release);
}

Counts LaneCounts() { return g_counts; }

}  // namespace coop::serverbox_sync
