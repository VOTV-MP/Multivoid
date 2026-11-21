// coop/props/prop_act_omega.cpp -- see coop/props/prop_act_omega.h.

#include "coop/props/prop_act_omega.h"

#include "coop/creatures/kerfur_dress_sync.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/hand_spend.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_act_families.h"
#include "coop/props/prop_act_request.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/actors/kerfur_dress.h"
#include "ue_wrap/actors/miner.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

#include <wchar.h>

namespace coop::prop_act_omega {

using coop::prop_act_host::Tally;

namespace {

namespace N  = coop::net;
namespace KD = ue_wrap::kerfur_dress;
namespace R  = ue_wrap::reflection;
namespace EL = coop::element;
namespace sg = ue_wrap::script_gate;
namespace RQ = coop::prop_act_request;
namespace PF = coop::prop_act_families;

// Declared on kerfurOmega_C alone, so one name watch matches every Omega skin.
UE_FACT_FN(kRowPutOn,   "prop_act", L"kerfurOmega_C", L"playerUsedOn", Declared);
UE_FACT_FN(kRowTakeOff, "prop_act", L"kerfurOmega_C", L"upgradeTake",  Declared);
constexpr int kTagPutOn = 0x5041433A, kTagTakeOff = 0x5041433B;   // "PAC:" and "PAC;"
constexpr const char* kDead =
    "a client's put-on or take-off of an Omega's outfit runs on its own copy only, unasked, and the copies diverge";

// What the judge found for a slot, kept from its grant to the commit that spends it: the row and, for a put-on, the class
// the hand lane held the item under. The commit applies from these and never reads the hand again, which the replay has
// emptied by then.
struct Judged {
    bool valid = false;
    bool putOn = false;
    std::wstring row;
    std::wstring cls;
};
Judged g_judged[N::kMaxPeers];
N::IntentBucket g_drops[N::kMaxPeers];

bool SameName(const std::wstring& a, const wchar_t* b) { return ::_wcsicmp(a.c_str(), b) == 0; }

bool Contains(const std::vector<std::wstring>& names, const std::wstring& row) {
    return std::any_of(names.begin(), names.end(), [&](const std::wstring& n) { return SameName(n, row.c_str()); });
}

bool InRoster(uint8_t slot) { return slot >= 1 && slot < N::kMaxPeers; }

uint32_t EidOf(void* omega) { return static_cast<uint32_t>(EL::Registry::Get().EidForActor(omega)); }

// The game's rules for a put-on of `row`, read off `omega`'s copy. `unread` is set when a fact could not be read.
internal::Refusal WearRefusal(void* omega, const std::wstring& row, bool& unread) {
    KD::RowFacts facts;
    std::vector<std::wstring> drip, bones;
    unread = !KD::ReadRow(row, facts) || !KD::ReadDrip(omega, drip) || !KD::ReadDripBones(omega, bones);
    if (unread) return internal::Refusal::None;
    return internal::PutOnRefusal(facts.found, facts.type, SameName(row, L"kerfoacc_pant"), Contains(drip, row),
                                  facts.found && Contains(bones, facts.bone));
}

bool ToVerb(const std::wstring& name, char (&out)[N::kPropActVerbBytes]) {
    std::memset(out, 0, sizeof(out));
    if (name.empty() || name.size() >= sizeof(out)) return false;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] < 0x20 || name[i] > 0x7e) return false;
        out[i] = static_cast<char>(name[i]);
    }
    return true;
}

// ---- the client's half ----

// Why a PRE let the game's own verb run on a shared Omega's copy instead of asking the host; each is said once per session.
enum class Unasked : uint8_t { HoldUnread, NothingHeld, NotHandView, GameRefusal, RowName, ItemUnread, kCount };
bool g_unaskedSaid[static_cast<size_t>(Unasked::kCount)] = {};

sg::Verdict RunUnasked(Unasked why) {
    static const char* const kText[] = {"the player's hold cannot be read", "nothing is held",
                                        "the hold is not the hand view's actor", "the game refuses the row itself",
                                        "the row's name does not fit the wire's verb field", "the item cannot be read"};
    bool& said = g_unaskedSaid[static_cast<size_t>(why)];
    if (!said) {
        said = true;
        UE_LOGW("prop_act: a client's put-on or take-off on a shared Omega ran the game's own verb on its copy, unasked "
                "(%s)", kText[static_cast<size_t>(why)]);
    }
    return sg::Verdict::Run;
}

sg::Verdict PrePutOn(const sg::Call& c) {
    N::Session* const s = RQ::internal::LinkedSession();
    if (!s) return sg::Verdict::Run;
    if (s->role() == N::Role::Host) return RQ::internal::HostPre(c.object);
    void* const omega = c.object;
    EL::ElementId eid = EL::kInvalidId;
    if (RQ::internal::ReplayingTarget(omega) || !PF::SharedTarget(omega, EL::ElementType::Npc, eid)) return sg::Verdict::Run;
    // The row is the player's own hold: equipItem reads it there, not from the call's arguments.
    void* const local = coop::players::Registry::Get().Local();
    std::wstring name;
    void* held = nullptr;
    if (!local || !ue_wrap::miner::ReadHold(local, name, held)) return RunUnasked(Unasked::HoldUnread);
    if (!held) return RunUnasked(Unasked::NothingHeld);
    if (!coop::hand_item::IsLocalHandViewActor(held)) return RunUnasked(Unasked::NotHandView);
    // When the game would not equip the row, the game's own refusal runs as it does in single player: its hint, the item
    // kept -- and for kerfoacc_pant its quit of the player's own game. The host's judge refuses a pant, so it never ends the
    // host's game. A rule the copy cannot read is the host's to judge.
    bool unread = false;
    if (WearRefusal(omega, name, unread) != internal::Refusal::None && !unread) return RunUnasked(Unasked::GameRefusal);
    RQ::Pending p;
    p.eid = eid;
    p.row = RQ::kFamily;
    p.target.Set(omega);
    if (!ToVerb(name, p.verb)) return RunUnasked(Unasked::RowName);
    // The hold goes on the one ordered lane ahead of the ask, so the host's record of the hand is the item asked about.
    coop::hand_item::AnnounceHeldNow(*s, local, held);
    RQ::Ask(p, N::PropActFamily::OmegaPutOn);
    return sg::Verdict::Cancel;
}

sg::Verdict PreTakeOff(const sg::Call& c) {
    static R::ParamOffset sItem{L"item"};
    N::Session* const s = RQ::internal::LinkedSession();
    if (!s) return sg::Verdict::Run;
    if (s->role() == N::Role::Host) return RQ::internal::HostPre(c.object);
    EL::ElementId eid = EL::kInvalidId;
    const int32_t off = sItem.Of(c.function);
    if (!PF::SharedTarget(c.object, EL::ElementType::Npc, eid)) return sg::Verdict::Run;
    if (off < 0 || !c.locals) return RunUnasked(Unasked::ItemUnread);
    RQ::Pending p;
    p.eid = eid;
    p.row = RQ::kFamily;
    p.target.Set(c.object);
    if (!ToVerb(R::ToString(*reinterpret_cast<const R::FName*>(c.locals + off)), p.verb)) return RunUnasked(Unasked::RowName);
    if (coop::kerfur_dress_sync::dev::RedOf() == coop::kerfur_dress_sync::dev::Red::Ask) {
        UE_LOGW("prop_act: kdress_red=ask -- this client's take-off is cancelled and asks nothing");
        return sg::Verdict::Cancel;
    }
    // A take-off has no replay: the host takes the item off its copy and the outfit comes back as its word.
    RQ::Ask(p, N::PropActFamily::OmegaTakeOff);
    return sg::Verdict::Cancel;
}

bool RunPutOn(void* omega, void* local, const PF::Verb& verb) {
    std::wstring want, name;
    void* held = nullptr;
    // The hold may have changed in the round trip: the replay equips the player's hold of now, so only the asked one runs.
    if (!internal::RowText(verb, want) || !ue_wrap::miner::ReadHold(local, name, held) || !held ||
        !SameName(name, want.c_str()))
        return false;
    return KD::PutOn(omega, local, held, want);
}

// 1 when the copy wears the row and the hand no longer holds that hold, else 0.
uint8_t PutOnCommit(void* omega, const PF::Verb& verb) {
    std::wstring want, name;
    std::vector<std::wstring> drip;
    void* held = nullptr;
    if (!internal::RowText(verb, want) || !KD::ReadDrip(omega, drip) || !Contains(drip, want)) return 0;
    void* const local = coop::players::Registry::Get().Local();
    // The game destroys the held item when it equips; a hand that still holds the row's hold kept it.
    if (!local || (ue_wrap::miner::ReadHold(local, name, held) && SameName(name, want.c_str()))) return 0;
    return 1;
}

// ---- the host's half ----

// The family's own conditions: the term that refuses the request, none when it fits. A put-on records the judged row and the
// sender's held class for the commit; a take-off records its row and spends a token of the drop budget.
std::optional<Tally> Judge(N::Session& session, uint8_t slot, const N::PropActRequestPayload& req, void* omega) {
    if (!InRoster(slot)) return Tally::FitFamilyArg;
    g_judged[slot] = Judged{};
    std::wstring row;
    std::vector<std::wstring> drip;
    if (!ue_wrap::kerfur::IsKerfurActor(omega) || !internal::RowText(req.verb, row)) return Tally::FitFamilyArg;
    if (!KD::ReadDrip(omega, drip)) return Tally::FitFamilyUnread;
    if (req.family == static_cast<uint8_t>(N::PropActFamily::OmegaTakeOff)) {
        if (!Contains(drip, row)) return Tally::FitFamilyRule;
        if (!g_drops[slot].Take(kDropBudget, N::NowMs())) return Tally::FitFamilyBudget;
        g_judged[slot] = Judged{true, false, row, {}};
        return std::nullopt;
    }
    bool unread = false;
    const internal::Refusal r = WearRefusal(omega, row, unread);
    if (unread) return Tally::FitFamilyUnread;
    if (r != internal::Refusal::None) return internal::TermOf(r);
    // The hand evidence: the hold the sender announced ahead of this ask, the owed state applied first. The spend is the
    // commit's; this only reads.
    coop::hand_item::ApplyOwedNow(session, slot);
    const std::wstring held = coop::hand_item::HeldItem(slot);
    const std::wstring cls = coop::hand_item::HeldClass(slot);
    if (held.empty() || cls.empty() || !SameName(held, row.c_str())) return Tally::FitFamilyHand;
    g_judged[slot] = Judged{true, true, row, cls};
    return std::nullopt;
}

// A granted take-off, before its grant is answered: unequipItem on the host's copy, and the host's own open equipment list
// rebuilt. False when the item could not be taken off, and then the asker is refused.
bool ApplyTakeOff(uint8_t slot, void* omega) {
    if (!InRoster(slot) || !g_judged[slot].valid || g_judged[slot].putOn) return false;
    const std::wstring& row = g_judged[slot].row;
    const bool ok = KD::Unequip(omega, row);
    const bool listed = ok && KD::RefreshViewer(omega);
    UE_LOGI("prop_act: slot %u took off '%ls' on Omega eid=%u (ok=%d, host's equipment list rebuilt=%d)",
            static_cast<unsigned>(slot), row.c_str(), EidOf(omega), ok ? 1 : 0, listed ? 1 : 0);
    return ok;
}

// A put-on's commit of 1: the judged row's rules read again on the host's copy, the hold spent, the row written. The term
// that refused it, none when the row was written; a refused one puts the asker's copy back to the host's outfit.
std::optional<Tally> ApplyPutOn(uint8_t slot, void* omega) {
    if (!InRoster(slot) || !g_judged[slot].valid || !g_judged[slot].putOn) return Tally::FitFamilyArg;
    const Judged j = g_judged[slot];
    if (coop::kerfur_dress_sync::dev::RedOf() == coop::kerfur_dress_sync::dev::Red::Apply) {
        UE_LOGW("prop_act: kdress_red=apply -- slot %u's put-on of '%ls' is not written at its commit",
                static_cast<unsigned>(slot), j.row.c_str());
        return std::nullopt;
    }
    // The Omega's copy may have changed since the grant: the game's rules are read again, and a row they no longer allow is
    // not written.
    bool unread = false;
    const internal::Refusal r = WearRefusal(omega, j.row, unread);
    std::optional<Tally> refused;
    if (unread) refused = Tally::FitFamilyUnread;
    else if (r != internal::Refusal::None) refused = internal::TermOf(r);
    else if (!coop::hand_spend::Spend(slot, j.cls.c_str(), 1)) refused = Tally::FitFamilyHand;
    if (refused) {
        UE_LOGI("prop_act: slot %u's put-on of '%ls' refused at its commit (%s) -- its copy is put back",
                static_cast<unsigned>(slot), j.row.c_str(), unread ? "unread" : internal::RefusalName(r));
        coop::kerfur_dress_sync::SendOutfit(omega, slot);
        return refused;
    }
    std::vector<std::wstring> drip;
    const bool wrote = KD::ReadDrip(omega, drip) && (drip.push_back(j.row), KD::ApplyDrip(omega, drip));
    UE_LOGI("prop_act: slot %u put on '%ls' on Omega eid=%u (ok=%d)", static_cast<unsigned>(slot), j.row.c_str(),
            EidOf(omega), wrote ? 1 : 0);
    if (wrote) return std::nullopt;
    // The host kept no row: the asker's copy wears one the host does not, so it is put back, as MTA sends the server's value
    // back to a source whose change the server did not keep (CGame.cpp:2779-2793).
    UE_LOGW("prop_act: slot %u's put-on of '%ls' could not be written on the host's Omega -- its copy is put back",
            static_cast<unsigned>(slot), j.row.c_str());
    coop::kerfur_dress_sync::SendOutfit(omega, slot);
    return Tally::FitFamilyUnread;
}

// A put-on's abort: the asker's copy is put back to the host's outfit.
void NoteAborted(uint8_t slot, void* omega) {
    if (!InRoster(slot) || !omega) return;
    UE_LOGI("prop_act: slot %u's put-on aborted -- its copy is put back to the host's outfit", static_cast<unsigned>(slot));
    coop::kerfur_dress_sync::SendOutfit(omega, slot);
}

void OnRelease(uint8_t slot) {
    if (InRoster(slot)) g_judged[slot] = Judged{};
}

void OnPeerGone(uint8_t slot) {
    if (!InRoster(slot)) return;
    g_judged[slot] = Judged{};
    g_drops[slot].Reset();
}

void OnDisconnect() {
    for (bool& said : g_unaskedSaid) said = false;
    for (Judged& j : g_judged) j = Judged{};
    for (N::IntentBucket& b : g_drops) b.Reset();
}

// ---- the registry's entries ----

const PF::WatchDef kPutOnWatch[] = {{kRowPutOn, kTagPutOn, &PrePutOn, "put-on", kDead}};
const PF::WatchDef kTakeOffWatch[] = {{kRowTakeOff, kTagTakeOff, &PreTakeOff, "take-off", kDead}};

// The two entries share one record per slot, so the slot and session events are carried by the put-on's entry alone.
const PF::Family kPutOn = {
    static_cast<uint8_t>(N::PropActFamily::OmegaPutOn), "omega put-on", EL::ElementType::Npc, PF::Apply::AtCommit,
    kPutOnWatch, 1, &RunPutOn, &PutOnCommit, &Judge, nullptr, &ApplyPutOn, &NoteAborted, &OnRelease, &OnPeerGone,
    &OnDisconnect};
const PF::Family kTakeOff = {
    static_cast<uint8_t>(N::PropActFamily::OmegaTakeOff), "omega take-off", EL::ElementType::Npc, PF::Apply::AtGrant,
    kTakeOffWatch, 1, nullptr, nullptr, &Judge, &ApplyTakeOff, nullptr, nullptr, nullptr, nullptr, nullptr};
const PF::Registrar kRegisterPutOn(kPutOn);
const PF::Registrar kRegisterTakeOff(kTakeOff);

}  // namespace

namespace internal {

Refusal PutOnRefusal(bool rowFound, uint8_t type, bool isPant, bool worn, bool boneTaken) {
    if (!rowFound) return Refusal::NotARow;
    if (isPant) return Refusal::Pant;
    if (type != 0 && type != 1) return Refusal::Type;
    if (worn) return Refusal::Worn;
    if (type == 0 && boneTaken) return Refusal::Bone;
    return Refusal::None;
}

const char* RefusalName(Refusal r) {
    switch (r) {
    case Refusal::None:    return "none";
    case Refusal::NotARow: return "not-a-row";
    case Refusal::Pant:    return "pant";
    case Refusal::Type:    return "type";
    case Refusal::Worn:    return "worn";
    case Refusal::Bone:    return "bone";
    }
    return "";
}

Tally TermOf(Refusal r) { return r == Refusal::NotARow ? Tally::FitFamilyArg : Tally::FitFamilyRule; }

bool RowText(const char (&row)[N::kPropActVerbBytes], std::wstring& out) {
    out.clear();
    const size_t len = strnlen(row, sizeof(row));
    if (len == 0 || len >= sizeof(row)) return false;
    for (size_t i = 0; i < sizeof(row); ++i) {
        if (i < len ? (row[i] < 0x20 || row[i] > 0x7e) : row[i] != 0) return false;
    }
    out.assign(row, row + len);
    return true;
}

}  // namespace internal

}  // namespace coop::prop_act_omega
