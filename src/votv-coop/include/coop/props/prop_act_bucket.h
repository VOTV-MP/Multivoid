// coop/props/prop_act_bucket.h -- a client's verbs on a world water bucket, a concrete bucket and a cement bag: six entries of
// prop_act's family registry (coop/props/prop_act_families), a file per verb group.
//
// A client's PRE (on its own action, not our replay) lets the game refuse where the BP would refuse or do nothing; a verb it cannot
// form from the copy, the hold or the amount it reads is cancelled and said once, never run unasked on a host-owned copy;
// otherwise it announces the hold, asks the host and cancels. The host judges against its own copy, the hand lane's record of the
// hold and the held item's ledger (coop/props/bucket_hold_ledger), RECORDS what the commit will apply, and grants; the target is
// reserved for the slot until its commit. The client replays its half (the game's verb, or a spend of the held item for the mix,
// the trowel and the transfer) and commits 0 or 1; at 1 the host re-reads the judge's condition and writes the recorded value, and
// bucket_sync tells every peer; at 0 the asker's copy is put back. The shape is MTA's vehicle in/out request: a request judged on
// the server's own state, a confirm, the client plays, a notify the server re-checks before it writes, an abort that releases
// (CGame.cpp:3018-3500, REQUEST_IN :3079, CONFIRMED :3196, NOTIFY_IN :3373, NOTIFY_IN_ABORT :3419). The host's own player's verb
// on a target a slot has reserved is cancelled for the round trip, as MTA refuses a vehicle a player is entering to another
// (CGame.cpp:3129-3150); that removes the race of a failed apply. Game thread.

#pragma once

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/props/bucket_hold_ledger.h"
#include "coop/props/bucket_sync.h"
#include "coop/props/prop_act_families.h"
#include "coop/props/prop_act_request.h"

#include "ue_wrap/actors/bucket.h"
#include "ue_wrap/actors/miner.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include <wchar.h>

// The families, by PropActFamily byte:
//   6 soap: a soap used on a bucket   7 jar: a clean jar filled from a brewed bucket   8 dip: a sponge in a bucket
//   9 mix: a cement bag onto a bucket   10 trowel: one unit out of a concrete bucket   11 transfer: a unit between two bags
namespace coop::prop_act_bucket {

namespace N  = coop::net;
namespace B  = ue_wrap::bucket;
namespace R  = ue_wrap::reflection;
namespace EL = coop::element;
namespace sg = ue_wrap::script_gate;
namespace RQ = coop::prop_act_request;
namespace PF = coop::prop_act_families;
using coop::prop_act_host::Tally;

// The water bucket's own limits the judges and the PREs read: the soap item's amount, a mix's concrete, a bag's cap.
inline constexpr int32_t kSoapAmountMax = 10;
inline constexpr int32_t kMixNeedMax = 20;

// What the judge found for a slot, kept from its grant to the commit that spends it: the family, the verb's recorded value and the
// class the hand lane held the item under. The commit applies from these and never reads the hand again, which the replay emptied.
struct Judged {
    bool valid = false;
    uint8_t family = 0;
    int32_t value = 0;
    std::wstring cls;
};
inline Judged g_judged[N::kMaxPeers];

// A family's per-slot budget of asks that spend nothing the host sees: `{burst, per second}`. Soap and jar are bounded by the hand
// lane's use count (hand_spend::Spend) instead. A bucket per family and slot, reset with the slot.
inline constexpr N::IntentBudget kDipBudget{4.0f, 2.0f};
inline constexpr N::IntentBudget kMixBudget{4.0f, 1.0f};
inline constexpr N::IntentBudget kTrowelBudget{4.0f, 2.0f};
inline constexpr N::IntentBudget kTransferBudget{8.0f, 4.0f};
inline N::IntentBucket g_budget[6][N::kMaxPeers];   // by family byte - 6

inline bool InRoster(uint8_t slot) { return slot >= 1 && slot < N::kMaxPeers; }
inline N::IntentBucket& BudgetOf(N::PropActFamily f, uint8_t slot) { return g_budget[static_cast<uint8_t>(f) - 6][slot]; }

// The wire's verb word: the first bytes little-endian, every byte after them zero.
inline bool VerbZeroFrom(const PF::Verb& v, size_t from) {
    for (size_t i = from; i < sizeof(PF::Verb); ++i)
        if (v[i] != 0) return false;
    return true;
}
inline bool VerbInt(const PF::Verb& v, int32_t& out) {
    if (!VerbZeroFrom(v, 4)) return false;
    std::memcpy(&out, v, 4);
    return true;
}
inline bool VerbByte(const PF::Verb& v, uint8_t& out) {
    if (!VerbZeroFrom(v, 1)) return false;
    out = static_cast<uint8_t>(v[0]);
    return true;
}
inline void PutVerbInt(PF::Verb& v, int32_t x) {
    std::memset(v, 0, sizeof(v));
    std::memcpy(v, &x, 4);
}
inline void PutVerbByte(PF::Verb& v, uint8_t x) {
    std::memset(v, 0, sizeof(v));
    v[0] = static_cast<char>(x);
}

// Whether `actor` is of the class named `cls` or a class below it.
inline bool IsClass(void* actor, const wchar_t* cls) {
    void* base = ue_wrap::object_index::ClassByName(cls);
    void* const c = actor ? R::ClassOf(actor) : nullptr;
    return base && c && R::IsDescendantOfAny(c, &base, 1);
}

inline bool IsJarName(const std::wstring& name) { return ::_wcsicmp(name.c_str(), L"cleanjar") == 0; }

// A pointer parameter of the watched call, read off its frame; null when the frame or the parameter is not there.
inline void* ParamObject(const sg::Call& c, R::ParamOffset& po) {
    const int32_t off = c.locals ? po.Of(c.function) : -1;
    void* p = nullptr;
    if (off >= 0) std::memcpy(&p, c.locals + off, sizeof(p));
    return p;
}

// A PRE that cannot form the ask from what it reads (the copy, the hold, the amount) cancels the call and says why, once per cause
// per session: the game's own verb never runs unasked on a host-owned copy (for the mix it makes a concrete bucket of the client's
// own). Only the game's own refusals, where the BP shows a hint or does nothing, let the call run.
inline const char* g_unreadSaid[16] = {};
inline sg::Verdict CancelUnread(const char* why) {
    for (const char*& said : g_unreadSaid) {
        if (said && std::strcmp(said, why) == 0) return sg::Verdict::Cancel;
        if (!said) {
            said = why;
            UE_LOGW("prop_act: a client's verb on a shared bucket or bag was cancelled, not asked (%s)", why);
            break;
        }
    }
    return sg::Verdict::Cancel;
}

// The local player's hold as the hand lane would announce it: the player, the hold's name and its actor. The reason when it cannot
// be (the caller cancels its call and says why).
inline const char* ReadClientHold(void*& local, std::wstring& name, void*& held) {
    local = coop::players::Registry::Get().Local();
    if (!local || !ue_wrap::miner::ReadHold(local, name, held)) return "the player's hold cannot be read";
    if (!held) return "nothing is held";
    if (!coop::hand_item::IsLocalHandViewActor(held)) return "the hold is not the hand view's actor";
    return nullptr;
}

// The ask: the hold goes on the one ordered lane ahead of it, so the host's record of the hand is the item asked about, then the
// request; the call is cancelled. `verb` may be null for a family with no verb word.
inline sg::Verdict AskFamily(N::Session& s, void* local, void* held, void* target, EL::ElementId eid, N::PropActFamily family,
                             const PF::Verb* verb) {
    RQ::Pending p;
    p.eid = eid;
    p.row = RQ::kFamily;
    p.target.Set(target);
    if (verb) std::memcpy(p.verb, *verb, sizeof(p.verb));
    coop::hand_item::AnnounceHeldNow(s, local, held);
    RQ::Ask(p, family);
    return sg::Verdict::Cancel;
}

// The hand evidence a judge rests on: the hold the sender announced ahead of this ask, the owed state applied first. True when the
// hand holds an item of class `base` or below; `cls` is the class it was held under.
inline bool HandHolds(N::Session& session, uint8_t slot, const wchar_t* base, std::wstring& cls) {
    coop::hand_item::ApplyOwedNow(session, slot);
    cls = coop::hand_item::HeldClass(slot);
    return !cls.empty() && coop::hand_item::HeldClassIsA(slot, base);
}

// A commit of 1 on a target the host's own player or tick changed inside the round trip: counted, the host's row sent to the asker
// alone, and the term that refused returned. The asker's spent held side stays spent (an Omega's put-on has the same cost); the
// host's value is the truth. MTA corrects the source of a refused write alone (CGame.cpp:2775-2793, `pSourcePlayer->Send`): the
// other peers' copies did not change.
inline std::optional<Tally> FailedApply(uint8_t slot, void* target, const char* what, Tally term) {
    coop::bucket_sync::Note(coop::bucket_sync::Count::FamilyApplyFailures);
    coop::bucket_sync::SendRowTo(slot, target);
    UE_LOGI("prop_act: slot %u's %s found its target changed -- not applied, the asker's copy is put back",
            static_cast<unsigned>(slot), what);
    return term;
}

// The red `apply`: the family's apply is skipped, the commit taken.
inline bool ApplySkippedByRed(uint8_t slot, const char* what) {
    if (coop::bucket_sync::dev::RedOf() != coop::bucket_sync::dev::Red::Apply) return false;
    UE_LOGW("prop_act: bucket_red=apply -- slot %u's %s is not written at its commit", static_cast<unsigned>(slot), what);
    return true;
}

// The red `judge`: the family's own check is skipped, the ask granted.
inline bool JudgeSkippedByRed(uint8_t slot, const char* what) {
    if (coop::bucket_sync::dev::RedOf() != coop::bucket_sync::dev::Red::Judge) return false;
    UE_LOGW("prop_act: bucket_red=judge -- slot %u's %s is granted without its judge", static_cast<unsigned>(slot), what);
    return true;
}

// A commit of 0: the host's row goes to the asker alone, whose copy ran the game's verb (as FailedApply).
inline void NoteAborted(uint8_t slot, void* target) {
    if (!InRoster(slot) || !target) return;
    coop::bucket_sync::SendRowTo(slot, target);
}

// The slot's reservation ended, the slot left, the session ended: the judged records and the budgets go (one entry carries them
// for the six, as the six share the records).
void OnRelease(uint8_t slot);
void OnPeerGone(uint8_t slot);
void OnDisconnect();

// The watched calls' PREs, shared by the families that watch them (each is registered by the file that owns its row).
sg::Verdict PreUsedOn(const sg::Call& c);    // prop_bucket_C.playerUsedOn: soap (6) or jar (7) by the held item
sg::Verdict PreDipped(const sg::Call& c);    // prop_bucket_C.dipped: dip (8)
sg::Verdict PreBagLmb(const sg::Call& c);    // prop_cementBag_C.playerHandUse_LMB: mix (9) or a transfer to a world bag (11)
sg::Verdict PreBagRmb(const sg::Call& c);    // prop_cementBag_C.playerHandUse_RMB: a transfer from a world bag (11)
sg::Verdict PreFixerLmb(const sg::Call& c);  // prop_wallfixer_C.playerHandUse_LMB: trowel (10)

// A transfer between the held bag and a world bag, asked of the host: `direction` 0 the held bag gives, 1 it takes. Run when the game's
// own refusal applies (the bag to give is empty, the bag to fill is full).
sg::Verdict AskTransfer(N::Session& s, void* heldBag, void* worldBag, uint8_t direction);

}  // namespace coop::prop_act_bucket
