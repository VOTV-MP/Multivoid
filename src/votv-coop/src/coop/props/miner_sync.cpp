// coop/props/miner_sync.cpp -- see coop/props/miner_sync.h. The install, the watches and the client's asks.
//
// prop_miner_C's graph, from its bytecode (0.9.0n). playerUsedOn, on a blocking hit: `upgrade` with the player's
// holding_name, and on success the 2D sound warpbox_tile and K2_DestroyActor on the player's holding_actor. `upgrade`:
// scrap_metal adds a slot (16 at most), miner_gpu a GPU while a slot is free (16 at most), miner_fan a cooler (6 at
// most), each then `upd`, and a hint when it is refused. actionOptionIndex: action 4 claims, when powered and the points
// are above 0: a hint naming them, lib_C::addPoints, and accumulatedPoints to 0. ReceiveTick: overheat cools by
// delta / (14..16), and while powered `i_calc` steps and `calc` mines. getData and loadData carry the four counts and
// overheat in the rig's save record.

#include "coop/props/miner_sync.h"

#include "miner_internal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/actors/miner.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <string>

namespace coop::miner_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace M  = ue_wrap::miner;
namespace sg = ue_wrap::script_gate;
namespace N  = coop::net;

UE_FACT_FN(kRowTick, "miner_sync", L"prop_miner_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowUse, "miner_sync", L"prop_miner_C", L"playerUsedOn", Declared);
UE_FACT_FN(kRowAction, "miner_sync", L"prop_miner_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowFire, "miner_sync", L"prop_miner_C", L"fireDamage", Declared);
constexpr int kTagTick = 0x4D4E5201, kTagUse = 0x4D4E5202, kTagAction = 0x4D4E5203, kTagFire = 0x4D4E5204;  // 'MNR' 1..4

constexpr uint8_t kActionClaim = 4;  // EInteractionActions: the rig's claim

N::Session* g_session = nullptr;
bool g_watched = false;
bool g_settled = false;
bool g_taken[4] = {};  // tick, use, action, fire: whether the gate took the watch

// A use on the client's copy, between its start and its end: the rig and its numbers before, and the item the hand held.
struct UseNote {
    ue_wrap::CachedObjRef rig;
    M::Fields before;
    uint8_t item = 0;
    bool open = false;
};
UseNote g_note;
uint32_t g_useCancelled = 0, g_claimNotAsked = 0;

bool ClaimAskOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::miner_claim_ask_off);
    return off;
}

// The tick and the fire: a client's copy mines no points of its own and cools by the host's word, and its fire spawns no
// debris or loose GPUs of its own (`fireDamage`): the host's rig burns, and what it spawns reaches every peer.
sg::Verdict OnTickPre(const sg::Call&) { return detail::IsClient() ? sg::Verdict::Cancel : sg::Verdict::Run; }

// HOST: the end of a rig's own tick says what changed in it.
void OnTickPost(const sg::Call& c) { detail::OnRigTickPost(c.object); }

bool SendIntent(const N::MinerPayload& p) {
    const bool ok = g_session->SendReliable(N::ReliableKind::MinerIntent, &p, sizeof(p));
    if (!ok) detail::CountSendFailed();
    return ok;
}

// The use. A client's copy runs the game's own use, which counts the item in and spends it from the hand; the host is asked
// after it, and judges the hand the item left. The item the hand holds is announced ahead of the ask on the one ordered lane.
// A use the lane cannot stand behind (no session, no element id, not the hand view's item, not one of the three) runs
// nothing and keeps the item.
sg::Verdict OnUsePre(const sg::Call& c) {
    static R::ParamOffset sPlayer{L"player"};
    if (!detail::IsClient()) return sg::Verdict::Run;
    g_note.open = false;
    void* const player = c.function && c.locals && sPlayer.Of(c.function) >= 0
        ? *reinterpret_cast<void* const*>(c.locals + sPlayer.Of(c.function))
        : nullptr;
    void* const local = coop::players::Registry::Get().Local();
    std::wstring name;
    void* held = nullptr;
    const int item = player && player == local && M::ReadHold(player, name, held) ? M::ItemOfHoldName(name) : -1;
    M::Fields before;
    if (!g_session->connected() || item < 0 || !held || !coop::hand_item::IsLocalHandViewActor(held) ||
        !detail::EidOf(c.object) || !M::Read(c.object, before)) {
        const uint32_t n = ++g_useCancelled;
        if ((n & (n - 1)) == 0)
            UE_LOGI("miner_sync: CLIENT a use on a rig with '%ls' in hand runs nothing here, the item is kept (#%u)", name.c_str(), n);
        return sg::Verdict::Cancel;
    }
    coop::hand_item::AnnounceHeldNow(*g_session, local, held);
    g_note.rig.Set(c.object);
    g_note.before = before;
    g_note.item = static_cast<uint8_t>(item);
    g_note.open = true;
    return sg::Verdict::Run;
}

void OnUsePost(const sg::Call& c) {
    if (detail::IsHost()) {  // the host's own use: every peer is told at once, not at the rig's next tick
        detail::SendState(c.object, -1);
        return;
    }
    if (!g_note.open || !g_note.rig.Is(c.object)) return;
    g_note.open = false;
    M::Fields after;
    if (!M::Read(c.object, after)) return;
    if (after.gpus == g_note.before.gpus && after.slots == g_note.before.slots && after.fans == g_note.before.fans) return;  // the rig refused it
    N::MinerPayload p{};
    p.eid = detail::EidOf(c.object);
    p.op = N::kMinerOpAdd;
    p.item = g_note.item;
    const bool ok = SendIntent(p);
    UE_LOGI("miner_sync: CLIENT used %ls on rig eid %u -- asked the host (ok=%d)", M::ItemHoldName(p.item), p.eid, ok ? 1 : 0);
}

// The claim: asked of the host, whose rig pays into the shared balance. A claim run on a copy would pay only this peer's own
// view of the balance.
sg::Verdict OnActionPre(const sg::Call& c) {
    static R::ParamOffset sAction{L"action"};
    if (!detail::IsClient() || !c.object || !c.function || !c.locals) return sg::Verdict::Run;
    const int32_t off = sAction.Of(c.function);
    if (off < 0 || c.locals[off] != kActionClaim) return sg::Verdict::Run;
    if (ClaimAskOff()) {
        ++g_claimNotAsked;
        UE_LOGI("miner_sync: CLIENT claim: not asked (miner_claim_ask_off)");
        return sg::Verdict::Cancel;
    }
    N::MinerPayload p{};
    p.eid = detail::EidOf(c.object);
    if (!p.eid || !g_session->connected()) return sg::Verdict::Cancel;
    p.op = N::kMinerOpClaim;
    const bool ok = SendIntent(p);
    UE_LOGI("miner_sync: CLIENT claimed rig eid %u's points -- asked the host (ok=%d)", p.eid, ok ? 1 : 0);
    return sg::Verdict::Cancel;
}

}  // namespace

namespace detail {

N::Session* SessionPtr() { return g_session; }
bool IsHost() { return g_session && g_session->running() && g_session->role() == N::Role::Host; }
bool IsClient() { return g_session && g_session->running() && g_session->role() == N::Role::Client; }

}  // namespace detail

void Install(coop::net::Session* session) {
    g_session = session;
    detail::RegisterRecordGate();
    if (g_watched) return;
    g_watched = true;
    g_taken[0] = sg::WatchName(kRowTick, kTagTick, &OnTickPre, &OnTickPost);
    g_taken[1] = sg::WatchName(kRowUse, kTagUse, &OnUsePre, &OnUsePost);
    g_taken[2] = sg::WatchName(kRowAction, kTagAction, &OnActionPre, nullptr);
    g_taken[3] = sg::WatchName(kRowFire, kTagFire, &OnTickPre, nullptr);
    if (!g_taken[0] || !g_taken[1] || !g_taken[2] || !g_taken[3])
        UE_LOGE("miner_sync: a watch was refused (tick=%d use=%d action=%d fire=%d)", g_taken[0] ? 1 : 0, g_taken[1] ? 1 : 0,
                g_taken[2] ? 1 : 0, g_taken[3] ? 1 : 0);
}

// A refused watch is dead at once and settles alone; the accepted ones are driven to their settle.
static bool WatchSettled(int i, const ue_wrap::facts::Function& row, int tag) {
    return !g_taken[i] || sg::NameWatchSettled(row, tag);
}
static bool WatchLive(int i, const ue_wrap::facts::Function& row, int tag) { return g_taken[i] && sg::NameWatchLive(row, tag); }

void Tick() {
    M::ResolveNames();
    if (detail::IsHost()) detail::FlushOwed();
    if (!g_watched || g_settled) return;
    sg::ResolvePendingNames();
    if (!WatchSettled(0, kRowTick, kTagTick) || !WatchSettled(1, kRowUse, kTagUse) ||
        !WatchSettled(2, kRowAction, kTagAction) || !WatchSettled(3, kRowFire, kTagFire))
        return;
    g_settled = true;
    const bool t = WatchLive(0, kRowTick, kTagTick), u = WatchLive(1, kRowUse, kTagUse),
               a = WatchLive(2, kRowAction, kTagAction), f = WatchLive(3, kRowFire, kTagFire);
    if (t && u && a && f) UE_LOGI("miner_sync: the watches on prop_miner_C's tick, use, actions and fire are live");
    else UE_LOGE("miner_sync: a watch is dead (tick=%d use=%d action=%d fire=%d) -- a client's rig runs its own", t ? 1 : 0,
                 u ? 1 : 0, a ? 1 : 0, f ? 1 : 0);
}

void OnBound(uint32_t eid, void* actor) {
    if (detail::IsClient()) detail::OnBound(eid, actor);
}

void SendJoinSnapshotForSlot(int slot) { detail::SendSnapshot(slot); }

void OnIntent(int senderSlot, const coop::net::MinerPayload& p) { detail::JudgeIntent(senderSlot, p); }

void OnState(const coop::net::MinerPayload& p) {
    if (!detail::IsClient()) return;
    if (p.op == N::kMinerOpAnswer) detail::HeardAnswer(p);
    else if (p.op == N::kMinerOpState) detail::ApplyOrPark(p);
}

void OnDisconnect() {
    if (g_useCancelled || g_claimNotAsked)
        UE_LOGI("miner_sync: session end -- client uses left unrun %u, claims not asked %u", g_useCancelled, g_claimNotAsked);
    g_useCancelled = g_claimNotAsked = 0;
    g_note = UseNote{};
    detail::ClearState();
    detail::ClearJudge();
    detail::ClearRecord();
}

}  // namespace coop::miner_sync
