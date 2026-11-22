// coop/props/prop_act_bucket_bag.cpp -- the transfer family (11) between the held cement bag and a world bag; see
// coop/props/prop_act_bucket.h.
//
// A held bag's left click on a world bag moves a unit from the held bag to it (transferUnit(this, hit)), its right click moves one
// from the world bag to the held bag (transferUnit(hit, this)); the game refuses when the giver is empty or the receiver full
// (prop_cementBag.cpp:300-337). The client's replay is the held bag's side alone, a unit more or less through ue_wrap::bucket; the
// world bag's side is the host's apply. The left click is watched in prop_act_bucket_concrete.cpp, which also owns the mix.

#include "coop/props/prop_act_bucket.h"

#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/facts/function_row.h"

namespace coop::prop_act_bucket {
namespace {

namespace E = ue_wrap::engine;

UE_FACT_FN(kRowBagRmb, "prop_act_bucket", L"prop_cementBag_C", L"playerHandUse_RMB", Declared);
constexpr int kTagBagRmb = 0x50425230;   // "PBR0"
constexpr const char* kDead =
    "a client's unit from a world cement bag to its held bag moves on its own copies only, unasked, and the copies diverge";

// Whether the game lets a unit move from a bag of `fromUnits` to one of `toUnits` with cap `toMax`.
bool Moves(int32_t fromUnits, int32_t toUnits, int32_t toMax) { return fromUnits > 0 && toUnits < toMax; }

// Whether the slot's held bag, by its ledger, can give a unit (direction 0) or has room for one (1); `max` is the bags' cap.
bool HeldMoves(uint8_t slot, int32_t dir, int32_t max) {
    const int32_t units = coop::bucket_hold::BagUnits(slot);
    return dir == 0 ? units > 0 : units < max;
}

// ---- the client's half ----

// The held bag's side: a unit less when it gives (direction 0), a unit more when it takes (1), on the copies' own numbers.
bool RunTransfer(void* world, void* local, const PF::Verb& verb) {
    uint8_t dir = 0;
    std::wstring name;
    void* held = nullptr;
    int32_t hu = 0, hm = 0, wu = 0, wm = 0;
    if (!VerbByte(verb, dir) || dir > 1 || !ue_wrap::miner::ReadHold(local, name, held) || !held ||
        B::KindOf(held) != B::Kind::Bag || !B::ReadBag(held, hu, hm) || !B::ReadBag(world, wu, wm))
        return false;
    if (dir == 0 ? !Moves(hu, wu, wm) : !Moves(wu, hu, hm)) return false;
    return B::WriteBagUnits(held, dir == 0 ? hu - 1 : hu + 1);
}

uint8_t SpentCommit(void*, const PF::Verb&) { return 1; }

// ---- the host's half ----

std::optional<Tally> Judge(N::Session& session, uint8_t slot, const N::PropActRequestPayload& req, void* world) {
    if (!InRoster(slot)) return Tally::FitFamilyArg;
    g_judged[slot] = Judged{};
    if (JudgeSkippedByRed(slot, "transfer")) return std::nullopt;
    uint8_t dir = 0;
    int32_t wu = 0, wm = 0;
    if (B::KindOf(world) != B::Kind::Bag || !VerbByte(req.verb, dir) || dir > 1) return Tally::FitFamilyArg;
    if (!B::ReadBag(world, wu, wm)) return Tally::FitFamilyUnread;
    std::wstring cls;
    if (!HandHolds(session, slot, L"prop_cementBag_C", cls)) return Tally::FitFamilyHand;
    if (dir == 0 ? wu >= wm : wu <= 0) return Tally::FitFamilyRule;
    if (!HeldMoves(slot, dir, wm)) return Tally::FitFamilyHand;   // the held bag's ledger cannot give, or has no room
    if (!BudgetOf(N::PropActFamily::BagTransfer, slot).Take(kTransferBudget, N::NowMs())) return Tally::FitFamilyBudget;
    g_judged[slot] = Judged{true, req.family, dir, cls};
    return std::nullopt;
}

// A transfer's commit of 1: the world bag's units read again on the host's copy, the held bag's ledger moved (the client's replay
// moved the bag), then a unit more or less.
std::optional<Tally> Apply(uint8_t slot, void* world) {
    if (!InRoster(slot) || !g_judged[slot].valid || g_judged[slot].family != static_cast<uint8_t>(N::PropActFamily::BagTransfer))
        return Tally::FitFamilyArg;
    const int32_t dir = g_judged[slot].value;
    if (ApplySkippedByRed(slot, "transfer")) return std::nullopt;
    int32_t wu = 0, wm = 0;
    if (!B::ReadBag(world, wu, wm)) return FailedApply(slot, world, "transfer", Tally::FitFamilyUnread);
    if (!HeldMoves(slot, dir, wm)) return FailedApply(slot, world, "transfer", Tally::FitFamilyHand);
    coop::bucket_hold::SetBagUnits(slot, coop::bucket_hold::BagUnits(slot) + (dir == 0 ? -1 : 1));
    if (dir == 0 ? wu >= wm : wu <= 0) return FailedApply(slot, world, "transfer", Tally::FitFamilyRule);
    B::WriteBagUnits(world, dir == 0 ? wu + 1 : wu - 1);
    coop::bucket_sync::Note(coop::bucket_sync::Count::FamilyApplies);
    coop::bucket_sync::MarkDirty(world);
    UE_LOGI("prop_act: slot %u moved a cement unit %s a world bag (now %d)", static_cast<unsigned>(slot),
            dir == 0 ? "into" : "out of", static_cast<int>(dir == 0 ? wu + 1 : wu - 1));
    return std::nullopt;
}

// ---- the registry's entry ----

const PF::WatchDef kTransferWatch[] = {{kRowBagRmb, kTagBagRmb, &PreBagRmb, "bag transfer from a world bag", kDead}};
const PF::Family kTransfer = {
    static_cast<uint8_t>(N::PropActFamily::BagTransfer), "bag transfer", EL::ElementType::Prop, PF::Apply::AtCommit, kTransferWatch, 1,
    &RunTransfer, &SpentCommit, &Judge, nullptr, &Apply, &NoteAborted, nullptr, nullptr, nullptr};
const PF::Registrar kRegisterTransfer(kTransfer);

}  // namespace

sg::Verdict AskTransfer(N::Session& s, void* heldBag, void* worldBag, uint8_t direction) {
    EL::ElementId eid = EL::kInvalidId;
    if (!PF::SharedTarget(worldBag, EL::ElementType::Prop, eid)) return sg::Verdict::Run;
    int32_t hu = 0, hm = 0, wu = 0, wm = 0;
    if (!B::ReadBag(heldBag, hu, hm) || !B::ReadBag(worldBag, wu, wm)) return CancelUnread("a cement bag's units cannot be read");
    if (direction == 0 ? !Moves(hu, wu, wm) : !Moves(wu, hu, hm)) return sg::Verdict::Run;   // the game's own hint: empty or full
    void* local = nullptr;
    void* held = nullptr;
    std::wstring name;
    if (const char* why = ReadClientHold(local, name, held)) return CancelUnread(why);
    if (held != heldBag) return sg::Verdict::Run;
    PF::Verb v;
    PutVerbByte(v, direction);
    return AskFamily(s, local, held, worldBag, eid, N::PropActFamily::BagTransfer, &v);
}

// The held bag's right click on a world bag: a unit from it to the held bag.
sg::Verdict PreBagRmb(const sg::Call& c) {
    static R::ParamOffset sPlayer{L"player"};
    N::Session* const s = RQ::internal::LinkedSession();
    if (!s) return sg::Verdict::Run;
    void* const player = ParamObject(c, sPlayer);
    if (s->role() == N::Role::Host) {
        void* const hit = player ? E::ReadMainPlayerHitActor(player) : nullptr;
        return B::KindOf(hit) == B::Kind::Bag ? RQ::internal::HostPre(hit) : sg::Verdict::Run;
    }
    if (RQ::internal::ReplayInProgress()) return sg::Verdict::Cancel;
    if (!player || !coop::players::Registry::Get().IsLocal(player) || B::KindOf(c.object) != B::Kind::Bag)
        return sg::Verdict::Run;
    void* const hit = E::ReadMainPlayerHitActor(player);
    return B::KindOf(hit) == B::Kind::Bag ? AskTransfer(*s, c.object, hit, 1) : sg::Verdict::Run;
}

}  // namespace coop::prop_act_bucket
