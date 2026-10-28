// coop/dev/bind_wait_selftest.cpp -- see coop/dev/bind_wait_selftest.h.

#include "coop/dev/bind_wait_selftest.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/bind_wait.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/log.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace coop::dev::bind_wait_selftest {
namespace {

namespace BW = coop::element::bind_wait;

// Above kMaxElements (65536) and below kInvalidId: no row of the registry and no wire eid is ever here.
constexpr uint32_t kBase = 0xFFFE0000u;
constexpr uint32_t kEids = 27;  // kBase .. kBase+26
constexpr uint16_t kKinds = 170;  // the kinds the checks use are all below this
// A pump with real holds in it is not the selftest's to run: it waits this many ticks for them to clear.
constexpr uint32_t kMaxWaitTicks = 600;

bool g_fired = false;
uint32_t g_waitTicks = 0;

bool Enabled() {
    static const bool on = coop::config::ResolveInt(::coop::config_registry::rows::bind_wait_selftest) != 0;
    return on;
}
bool RedOn() {
    return coop::config::ResolveInt(::coop::config_registry::rows::bind_wait_red) != 0;
}

// What the replay saw: the first byte of each message and the slot that sent it.
struct Call { uint8_t tag; int slot; };
std::vector<Call> g_calls;
uint32_t g_reholdEid = 0;   // check 3: the eid the 'R' replay defers and holds again
uint32_t g_giveUpEid = 0;   // check 7: the eid the 'K' replay gives up
int g_leaveSlot = 0;        // check 8: the slot the 'L' replay drops
uint32_t g_dropEid = 0;     // check 10: the eid whose kind-80 holds the 'D' replay drops

void Record(const uint8_t* bytes, size_t n, int slot) {
    const uint8_t tag = n > 0 ? bytes[0] : 0;
    g_calls.push_back(Call{tag, slot});
    if (tag == 'D') BW::Drop(g_dropEid, 80, "selftest: a replay drops a later hold of its own eid");
    if (tag == 'K') BW::GiveUp(g_giveUpEid, "selftest: a replay gives up a due eid");
    if (tag == 'L') BW::OnSlotLeft(g_leaveSlot);
    if (tag == 'Z') BW::Clear("selftest: a replay clears the wait");
    if (tag == 'R') {   // a replay that defers a new eid and binds it: due, but only at the next Pump
        BW::MarkDeferred(g_reholdEid, "selftest: a replay defers again");
        const uint8_t s = 'S';
        BW::Hold(g_reholdEid, 21, slot, &Record, &s, 1, false);
        BW::OnBound(g_reholdEid);
    }
}

bool HoldTag(uint32_t eid, uint16_t kind, int slot, uint8_t tag, bool coalesce) {
    return BW::Hold(eid, kind, slot, &Record, &tag, 1, coalesce);
}

bool HoldFlagged(uint32_t eid, uint16_t kind, int slot, uint8_t tag) {
    return BW::HoldReplayOnGiveUp(eid, kind, slot, &Record, &tag, 1);
}

bool Calls(const char* tags) {
    size_t i = 0;
    for (; tags[i]; ++i)
        if (i >= g_calls.size() || g_calls[i].tag != static_cast<uint8_t>(tags[i])) return false;
    return g_calls.size() == i;
}

// held - replayed - dropped - givenUp of a kind: zero once every hold of it is resolved. The kinds may carry
// the session's own history, so a check compares against the value taken before the first check.
int64_t g_balance0[kKinds];

int64_t Balance(uint16_t kind) {
    return static_cast<int64_t>(BW::HeldOf(kind)) - BW::ReplayedOf(kind) - BW::DroppedOf(kind) - BW::GivenUpOf(kind);
}

void TakeBalances() {
    for (uint16_t k = 0; k < kKinds; ++k) g_balance0[k] = Balance(k);
}

// After a check that left no hold stored: HeldOf == ReplayedOf + DroppedOf + GivenUpOf, kind by kind.
const char* Balanced(int check) {
    static char msg[96];
    for (uint16_t k = 0; k < kKinds; ++k)
        if (Balance(k) != g_balance0[k]) {
            std::snprintf(msg, sizeof msg, "check %d: the tallies of kind %u do not balance", check,
                          static_cast<unsigned>(k));
            return msg;
        }
    return nullptr;
}

// Check 9: a prop row that outlived its purged actor (a live row, an actor whose object slot is gone) is the
// window before BindUnboundReCreates re-binds it. A registry mirror at a free host id stands in for it, with
// an actor pointer that is never read: the slot index -1 reads as not live.
const char* CheckPurgeWindow() {
    namespace CE = coop::element;
    CE::ElementId eid = CE::kHostRangeSize - 1;
    while (eid > 1 && CE::Registry::Get().Get(eid)) --eid;
    if (CE::Registry::Get().Get(eid)) return "no free host eid for the purge-window row";
    CE::Element row(CE::ElementType::Prop);
    row.SetActor(reinterpret_cast<void*>(static_cast<uintptr_t>(0x10)), -1);
    if (!CE::Registry::Get().RegisterMirror(eid, &row)) return "the purge-window row was not registered";
    g_calls.clear();
    const char* failed = nullptr;
    if (!HoldTag(eid, 70, 1, 'W', false)) failed = "a hold for a row awaiting its re-bind was refused";
    else if (!BW::IsDeferred(eid)) failed = "a row awaiting its re-bind was not marked";
    else {
        BW::OnBound(eid);
        BW::Pump();
        if (!Calls("W")) failed = "the hold of a row awaiting its re-bind did not replay at the bind";
    }
    BW::GiveUp(eid, "selftest");
    g_calls.clear();
    return failed;   // `row` unregisters itself
}

// Check 17: one client slot's share of the pool (256 KiB) is spent by four 64 KiB holds; the fifth is refused and
// counted, while slot 0 and another client slot still hold. A replay and a give-up each release the bytes, so the
// same slot holds again.
const char* CheckSlotShare(uint32_t eid) {
    const std::vector<uint8_t> big(64 * 1024, 'q');
    auto holdBig = [&](uint16_t kind, int slot) {
        return BW::Hold(eid, kind, slot, &Record, big.data(), big.size(), false);
    };
    BW::MarkDeferred(eid, "selftest: slot share");
    const uint32_t refused0 = BW::RefusedCount();
    for (uint16_t k = 120; k < 124; ++k) holdBig(k, 3);
    if (BW::RefusedCount() != refused0) return "a slot was refused within its share";
    holdBig(124, 3);
    if (BW::RefusedCount() != refused0 + 1) return "the refusal past a slot's share was not counted";
    holdBig(125, 0);
    holdBig(126, 4);
    if (BW::RefusedCount() != refused0 + 1) return "slot 0 or another slot was refused by a slot's share";
    BW::OnBound(eid);
    BW::Pump();
    if (g_calls.size() != 6) return "the holds within the shares were not the ones replayed";
    g_calls.clear();
    BW::MarkDeferred(eid, "selftest: slot share, after a replay");
    holdBig(127, 3);
    if (BW::RefusedCount() != refused0 + 1) return "a replay did not release its bytes from the slot's share";
    BW::GiveUp(eid, "selftest");
    BW::MarkDeferred(eid, "selftest: slot share, after a give-up");
    for (uint16_t k = 128; k < 132; ++k) holdBig(k, 3);
    if (BW::RefusedCount() != refused0 + 1) return "a give-up did not release its bytes from the slot's share";
    BW::GiveUp(eid, "selftest");
    return nullptr;
}

// The checks. 1: a hold for an eid not deferred is refused; 2: holds A, B, A' (coalesced onto A), C replay as
// A', B, C at the bind; 3: a replay that defers and holds again leaves the running pump whole, its hold runs at
// the next; 4: a given-up eid's holds never replay; 5: a slot that left takes only its own holds; 6: the 65th
// hold of one eid is refused (RefusedCount counts it); 7: a give-up made by a replay voids a due eid the pump
// has not reached; 8: a slot that leaves during a pump has its remaining holds skipped; 9: an unmarked eid whose
// registry row holds a purged actor is held and replays at its bind; 10: a replay's Drop of a later hold of its
// own eid voids it mid-pass; 11: a Drop of a stored hold; 12: of a due hold; 13: HeldOf against CoalescedOf on
// two holds of a kind; 14: a GiveUp counts its holds into GivenUpOf, a coalesced pair once; 15: a slot that left does, kind by kind;
// 16: so does a pump's skip of an eid a replay gave up; 17: a client slot keeps at most 256 KiB of the pool,
// slot 0 and another slot are not limited by it, and a replay and a give-up release the bytes; 18: coalescing
// holds of one kind from two slots are both kept; 19: a Clear made by a replay ends the pass and counts the
// rest into GivenUpOf (a Clear ends the marks of the whole session, so only the checks that mark their own eids
// follow it); 20: a twin's give-up queues its flagged holds for the next Pump (ReplayedOf, bytes released) and drops
// the rest into GivenUpOf; 14, 15 and 16 also cover a flagged hold on the GiveUp, slot-left and pump-skip paths.
// The first failing check's name, or null.
const char* CheckGreen() {
    g_calls.clear();
    TakeBalances();
    // 1.
    if (HoldTag(kBase + 0, 1, 1, 'x', false)) return "a hold for an eid not deferred was taken";
    if (const char* b = Balanced(1)) return b;
    // 2.
    const uint32_t e2 = kBase + 1;
    BW::MarkDeferred(e2, "selftest: order");
    if (!BW::IsDeferred(e2)) return "a marked eid does not read as deferred";
    if (!HoldTag(e2, 10, 1, 'A', true) || !HoldTag(e2, 11, 1, 'B', true) || !HoldTag(e2, 10, 1, 'a', true) ||
        !HoldTag(e2, 12, 1, 'C', true))
        return "a hold for a deferred eid was refused";
    BW::OnBound(e2);
    if (BW::IsDeferred(e2)) return "a bound eid still reads as deferred";
    BW::Pump();
    if (!Calls("aBC")) return "the replay order is not A' B C";
    g_calls.clear();
    if (const char* b = Balanced(2)) return b;
    // 3.
    const uint32_t e3 = kBase + 2;
    g_reholdEid = kBase + 3;
    BW::MarkDeferred(e3, "selftest: re-hold");
    HoldTag(e3, 20, 1, 'R', false);
    HoldTag(e3, 22, 1, 'X', false);
    BW::OnBound(e3);
    BW::Pump();
    if (!Calls("RX")) return "a replay that holds again changed the running pump";
    BW::Pump();
    if (!Calls("RXS")) return "the hold made inside a replay did not run at the next pump";
    g_calls.clear();
    if (const char* b = Balanced(3)) return b;
    // 4.
    const uint32_t e4 = kBase + 4;
    BW::MarkDeferred(e4, "selftest: give up");
    HoldTag(e4, 30, 1, 'G', false);
    BW::GiveUp(e4, "selftest");
    BW::OnBound(e4);
    BW::Pump();
    if (BW::IsDeferred(e4) || !g_calls.empty()) return "a given-up eid's holds were replayed";
    if (const char* b = Balanced(4)) return b;
    // 5.
    const uint32_t e5 = kBase + 5, e6 = kBase + 6;
    BW::MarkDeferred(e5, "selftest: slot left");
    BW::MarkDeferred(e6, "selftest: slot left");
    HoldTag(e5, 40, 1, 'P', false);
    HoldTag(e5, 41, 2, 'Q', false);
    HoldTag(e6, 40, 1, 'U', false);
    BW::OnSlotLeft(1);
    BW::OnBound(e5);
    BW::OnBound(e6);
    BW::Pump();
    if (!Calls("Q") || g_calls[0].slot != 2) return "a slot that left took more than its own holds";
    g_calls.clear();
    if (const char* b = Balanced(5)) return b;
    // 6.
    const uint32_t e7 = kBase + 7;
    BW::MarkDeferred(e7, "selftest: bound");
    const uint32_t refused0 = BW::RefusedCount();
    for (uint16_t k = 0; k < 65; ++k) HoldTag(e7, 100 + k, 1, 'z', false);
    if (BW::RefusedCount() != refused0 + 1) return "the refusal at the per-eid bound was not counted";
    BW::OnBound(e7);
    BW::Pump();
    if (g_calls.size() != 64) return "the per-eid bound did not hold at 64";
    g_calls.clear();
    if (const char* b = Balanced(6)) return b;
    // 7. A give-up made by a replay reaches an eid already swapped out of the due list.
    const uint32_t e8 = kBase + 8, e9 = kBase + 9;
    g_giveUpEid = e9;
    BW::MarkDeferred(e8, "selftest: giver");
    BW::MarkDeferred(e9, "selftest: given up in flight");
    HoldTag(e8, 50, 1, 'K', false);
    HoldTag(e9, 51, 1, 'Y', false);
    BW::OnBound(e8);
    BW::OnBound(e9);
    BW::Pump();
    if (!Calls("K")) return "a give-up during a pump did not void the due eid it named";
    g_calls.clear();
    if (const char* b = Balanced(7)) return b;
    // 8. A slot that leaves during a pump has its remaining holds skipped, in every due eid.
    const uint32_t e10 = kBase + 10, e11 = kBase + 11;
    g_leaveSlot = 2;
    BW::MarkDeferred(e10, "selftest: leaver");
    BW::MarkDeferred(e11, "selftest: left");
    HoldTag(e10, 60, 1, 'L', false);
    HoldTag(e10, 61, 2, 'M', false);
    HoldTag(e11, 62, 2, 'N', false);
    HoldTag(e11, 63, 1, 'O', false);
    BW::OnBound(e10);
    BW::OnBound(e11);
    BW::Pump();
    if (!Calls("LO")) return "a slot that left during a pump still had its holds replayed";
    g_calls.clear();
    if (const char* b = Balanced(8)) return b;
    // 9. A row whose actor was purged is held without a mark, and its re-bind frees the hold.
    const char* purged = CheckPurgeWindow();
    if (purged) return purged;
    if (const char* b = Balanced(9)) return b;
    // 10. A replay that Drops a later hold of its own eid during the pass: that hold does not run.
    const uint32_t e12 = kBase + 12;
    g_dropEid = e12;
    const uint32_t dropped80 = BW::DroppedOf(80);
    BW::MarkDeferred(e12, "selftest: a replay drops");
    HoldTag(e12, 80, 1, 'D', false);
    HoldTag(e12, 81, 1, 'E', false);
    HoldTag(e12, 80, 1, 'F', false);
    BW::OnBound(e12);
    BW::Pump();
    if (!Calls("DE")) return "a Drop made by a replay did not void the later hold of its kind in the pass";
    if (BW::DroppedOf(80) != dropped80 + 1) return "the hold a replay's Drop voided was not counted as dropped";
    g_calls.clear();
    if (const char* b = Balanced(10)) return b;
    // 11. A Drop of a stored hold erases it, releases its bytes and leaves the eid's other kinds.
    const uint32_t e13 = kBase + 13;
    const uint32_t dropped90 = BW::DroppedOf(90);
    BW::MarkDeferred(e13, "selftest: drop stored");
    HoldTag(e13, 90, 1, 'S', false);
    HoldTag(e13, 91, 1, 'T', false);
    if (!BW::Holds(e13, 90) || !BW::Holds(e13, 91)) return "Holds did not see a stored hold";
    BW::Drop(e13, 90, "selftest");
    if (BW::Holds(e13, 90) || !BW::Holds(e13, 91)) return "a Drop of a stored hold took more or less than its kind";
    if (BW::DroppedOf(90) != dropped90 + 1) return "a Drop of a stored hold was not counted as dropped";
    BW::OnBound(e13);
    BW::Pump();
    if (!Calls("T")) return "a stored hold that was dropped still replayed";
    if (BW::HasHolds()) return "a Drop left bytes held";
    g_calls.clear();
    if (const char* b = Balanced(11)) return b;
    // 12. A Drop of a due hold (bound, not yet pumped) erases it.
    const uint32_t e14 = kBase + 14;
    BW::MarkDeferred(e14, "selftest: drop due");
    HoldTag(e14, 92, 1, 'U', false);
    HoldTag(e14, 93, 1, 'V', false);
    BW::OnBound(e14);
    if (!BW::Holds(e14, 92)) return "Holds did not see a due hold";
    BW::Drop(e14, 92, "selftest");
    if (BW::Holds(e14, 92) || !BW::Holds(e14, 93)) return "a Drop of a due hold took more or less than its kind";
    BW::Pump();
    if (!Calls("V")) return "a due hold that was dropped still replayed";
    g_calls.clear();
    if (const char* b = Balanced(12)) return b;
    // 13. Two holds of one kind: a coalesced one overwrites in place and counts as coalesced, not as held.
    const uint32_t e15 = kBase + 15;
    const uint32_t held94 = BW::HeldOf(94), coalesced94 = BW::CoalescedOf(94), replayed94 = BW::ReplayedOf(94);
    BW::MarkDeferred(e15, "selftest: coalesced tally");
    HoldTag(e15, 94, 1, 'c', true);
    HoldTag(e15, 94, 1, 'C', true);
    if (BW::HeldOf(94) != held94 + 1 || BW::CoalescedOf(94) != coalesced94 + 1)
        return "a coalesced hold was not counted apart from a distinct one";
    BW::OnBound(e15);
    BW::Pump();
    if (!Calls("C") || BW::ReplayedOf(94) != replayed94 + 1) return "a coalesced hold did not replay once, latest bytes";
    g_calls.clear();
    if (const char* b = Balanced(13)) return b;
    // 14. A GiveUp counts every stored hold into GivenUpOf: two unflagged holds of one kind, and a flagged one of
    // a kind of its own (a flagged hold coalesces latest-wins onto an earlier hold of its (kind, slot), so on
    // kind 95 it would replace one of the two and the count would be two). Then the coalesced case: two flagged
    // holds of one (kind, slot) are one stored hold, and the GiveUp counts one.
    const uint32_t e16 = kBase + 1;
    const uint32_t givenUp95 = BW::GivenUpOf(95), givenUp165 = BW::GivenUpOf(165), givenUp166 = BW::GivenUpOf(166);
    BW::MarkDeferred(e16, "selftest: given-up tally");
    HoldTag(e16, 95, 1, 'g', false);
    HoldTag(e16, 95, 1, 'h', false);
    HoldFlagged(e16, 165, 1, 'i');
    BW::GiveUp(e16, "selftest");
    if (BW::GivenUpOf(95) != givenUp95 + 2 || BW::GivenUpOf(165) != givenUp165 + 1)
        return "a GiveUp of three holds of distinct kinds was not counted into GivenUpOf";
    BW::MarkDeferred(e16, "selftest: coalesced given-up tally");
    HoldFlagged(e16, 166, 1, 'j');
    HoldFlagged(e16, 166, 1, 'k');
    BW::GiveUp(e16, "selftest");
    if (BW::GivenUpOf(166) != givenUp166 + 1) return "a GiveUp of two coalesced holds did not count one into GivenUpOf";
    if (const char* b = Balanced(14)) return b;
    // 15. A slot that left takes its holds into GivenUpOf, kind by kind; the other slot's hold replays.
    const uint32_t e17 = kBase + 16;
    const uint32_t givenUp96 = BW::GivenUpOf(96), givenUp97 = BW::GivenUpOf(97);
    BW::MarkDeferred(e17, "selftest: slot left tally");
    HoldTag(e17, 96, 1, 'a', false);
    HoldTag(e17, 96, 2, 'b', false);
    HoldFlagged(e17, 97, 1, 'c');
    BW::OnSlotLeft(1);
    if (BW::GivenUpOf(96) != givenUp96 + 1 || BW::GivenUpOf(97) != givenUp97 + 1)
        return "a slot that left did not count its holds into GivenUpOf";
    BW::OnBound(e17);
    BW::Pump();
    if (!Calls("b")) return "the hold of the slot that stayed did not replay alone";
    g_calls.clear();
    if (const char* b = Balanced(15)) return b;
    // 16. A pump skips the holds of an eid a replay gave up, and counts them into GivenUpOf. The flagged hold is
    // of a kind of its own: it coalesces latest-wins onto an earlier hold of its (kind, slot), so on kind 99 it
    // would replace the unflagged one and the count would be one.
    const uint32_t e18 = kBase + 17, e19 = kBase + 18;
    g_giveUpEid = e19;
    const uint32_t givenUp99 = BW::GivenUpOf(99), givenUp167 = BW::GivenUpOf(167);
    const uint32_t replayed99 = BW::ReplayedOf(99), replayed167 = BW::ReplayedOf(167);
    BW::MarkDeferred(e18, "selftest: giver tally");
    BW::MarkDeferred(e19, "selftest: given up in flight tally");
    HoldTag(e18, 98, 1, 'K', false);
    HoldTag(e19, 99, 1, 'Y', false);
    HoldFlagged(e19, 167, 1, 'y');
    BW::OnBound(e18);
    BW::OnBound(e19);
    BW::Pump();
    if (!Calls("K")) return "a pump replayed the holds of an eid a replay gave up";
    if (BW::GivenUpOf(99) != givenUp99 + 1 || BW::GivenUpOf(167) != givenUp167 + 1 ||
        BW::ReplayedOf(99) != replayed99 || BW::ReplayedOf(167) != replayed167)
        return "the holds a pump skipped by a give-up were not counted into GivenUpOf";
    g_calls.clear();
    if (const char* b = Balanced(16)) return b;
    // 17. A client slot keeps at most its share of the pool; slot 0 and another slot are not limited by it, and
    // a replay and a give-up release the bytes.
    const char* shared = CheckSlotShare(kBase + 19);
    if (shared) return shared;
    if (const char* b = Balanced(17)) return b;
    // 18. Two slots' coalescing holds of one kind are both kept; a slot's own latest replaces its earlier one.
    const uint32_t e21 = kBase + 20;
    const uint32_t held140 = BW::HeldOf(140), coalesced140 = BW::CoalescedOf(140);
    BW::MarkDeferred(e21, "selftest: coalesce across slots");
    HoldTag(e21, 140, 1, 'm', true);
    HoldTag(e21, 140, 2, 'n', true);
    HoldTag(e21, 140, 1, 'o', true);
    if (BW::HeldOf(140) != held140 + 2 || BW::CoalescedOf(140) != coalesced140 + 1)
        return "a coalesce across two slots replaced the other slot's hold";
    BW::OnBound(e21);
    BW::Pump();
    if (!Calls("on") || g_calls[0].slot != 1 || g_calls[1].slot != 2)
        return "the coalesced holds of two slots did not replay as the latest of each, in first-arrival order";
    g_calls.clear();
    if (const char* b = Balanced(18)) return b;
    // 19. A Clear made by a replay ends the pass; the holds it had not reached are counted into GivenUpOf. Last,
    // because Clear ends the marks of the whole session.
    const uint32_t e22 = kBase + 21, e23 = kBase + 22;
    const uint32_t replayed150 = BW::ReplayedOf(150), givenUp151 = BW::GivenUpOf(151), givenUp152 = BW::GivenUpOf(152);
    const uint32_t givenUpAll = BW::GivenUpCount();
    BW::MarkDeferred(e22, "selftest: clear in a pass");
    BW::MarkDeferred(e23, "selftest: cleared in a pass");
    HoldTag(e22, 150, 1, 'Z', false);
    HoldTag(e22, 151, 1, 'x', false);
    HoldTag(e23, 152, 1, 'y', false);
    BW::OnBound(e22);
    BW::OnBound(e23);
    BW::Pump();
    if (!Calls("Z")) return "a pump went on after a replay cleared the wait";
    if (BW::ReplayedOf(150) != replayed150 + 1) return "the replay that cleared the wait was not counted as replayed";
    if (BW::GivenUpOf(151) != givenUp151 + 1 || BW::GivenUpOf(152) != givenUp152 + 1 ||
        BW::GivenUpCount() != givenUpAll + 2)
        return "the rest of a pass a Clear voided was not counted into GivenUpOf";
    if (BW::HasHolds() || BW::IsDeferred(e22)) return "a Clear made by a replay left holds or marks";
    g_calls.clear();
    if (const char* b = Balanced(19)) return b;
    // 20. A twin's give-up queues a flagged hold for the next Pump, unmarked, and drops the unflagged one into
    // GivenUpOf; a GiveUp drops a flagged one; a slot that leaves before the Pump takes a queued flagged hold; a
    // replayed flagged hold counts into ReplayedOf and releases its bytes.
    const uint32_t e24 = kBase + 23, e25 = kBase + 24, e26 = kBase + 25;
    const uint32_t replayed160 = BW::ReplayedOf(160), givenUp160 = BW::GivenUpOf(160), givenUp161 = BW::GivenUpOf(161);
    BW::MarkDeferred(e24, "selftest: twin gave up");
    BW::MarkDeferred(e25, "selftest: destroyed");
    if (!HoldFlagged(e24, 160, 1, 'f')) return "a replay-on-give-up hold was refused";
    HoldTag(e24, 161, 1, 'u', false);
    HoldFlagged(e25, 160, 1, 'f');
    BW::GiveUpReplayHeld(e24, "selftest");
    BW::GiveUp(e25, "selftest");
    if (BW::IsDeferred(e24)) return "a given-up eid still reads as deferred";
    if (!g_calls.empty()) return "a flagged hold was replayed inside the give-up";
    if (BW::GivenUpOf(161) != givenUp161 + 1 || BW::GivenUpOf(160) != givenUp160 + 1)
        return "the holds a give-up dropped were not counted into GivenUpOf, or the flagged one was";
    if (!BW::Holds(e24, 160) || BW::Holds(e24, 161)) return "the give-up did not queue exactly its flagged hold";
    BW::Pump();
    if (!Calls("f")) return "the give-up did not replay exactly its flagged hold";
    if (BW::ReplayedOf(160) != replayed160 + 1) return "a replayed flagged hold was not counted into ReplayedOf";
    if (BW::GivenUpOf(160) != givenUp160 + 1) return "a replayed flagged hold was counted into GivenUpOf";
    if (BW::HasHolds()) return "a give-up left bytes held";
    g_calls.clear();
    if (const char* b = Balanced(20)) return b;
    // A queued flagged hold whose sender leaves before the Pump is dropped, not replayed.
    const uint32_t givenUp162 = BW::GivenUpOf(162);
    BW::MarkDeferred(e26, "selftest: sender left");
    HoldFlagged(e26, 162, 1, 'f');
    BW::GiveUpReplayHeld(e26, "selftest");
    BW::OnSlotLeft(1);
    BW::Pump();
    if (!g_calls.empty() || BW::GivenUpOf(162) != givenUp162 + 1)
        return "a queued flagged hold of a slot that left was replayed or not counted into GivenUpOf";
    if (const char* b = Balanced(20)) return b;
    // 21. Three flagged holds of one (kind, slot) for one eid coalesce to one, latest bytes, still flagged: the
    // give-up replays that one and the two replaced are counted into CoalescedOf, not HeldOf.
    const uint32_t e27 = kBase + 26;
    const uint32_t held163 = BW::HeldOf(163), coalesced163 = BW::CoalescedOf(163), replayed163 = BW::ReplayedOf(163);
    BW::MarkDeferred(e27, "selftest: coalesced flagged holds");
    HoldFlagged(e27, 163, 1, 'a');
    HoldFlagged(e27, 163, 1, 'b');
    HoldFlagged(e27, 163, 1, 'c');
    if (BW::HeldOf(163) != held163 + 1 || BW::CoalescedOf(163) != coalesced163 + 2)
        return "three flagged holds of one kind and slot did not coalesce into one";
    BW::GiveUpReplayHeld(e27, "selftest");
    BW::Pump();
    if (!Calls("c")) return "the give-up did not replay exactly the newest flagged hold";
    if (BW::ReplayedOf(163) != replayed163 + 1) return "the coalesced flagged hold was not counted into ReplayedOf";
    if (BW::HasHolds()) return "a coalesced flagged hold left bytes held";
    g_calls.clear();
    if (const char* b = Balanced(21)) return b;
    return nullptr;
}

// With the red row on a hold is refused whatever the eid: the eid is deferred, so only the row refuses it.
const char* CheckRed() {
    const uint32_t e = kBase + 0;
    BW::MarkDeferred(e, "selftest: red arm");
    const bool taken = HoldTag(e, 1, 1, 'x', false);
    BW::GiveUp(e, "selftest");
    return taken ? "a hold was taken with bind_wait_red=1" : nullptr;
}

// The synthetic eids only: the real marks and holds of the session stay.
void Cleanup() {
    for (uint32_t i = 0; i < kEids; ++i) BW::GiveUp(kBase + i, "selftest cleanup");
    g_calls.clear();
}

void Run() {
    if (RedOn()) {
        if (HoldTag(kBase + 0, 1, 1, 'x', false)) {
            UE_LOGE("[BIND-WAIT-SELFTEST] FAIL: a hold for an eid not deferred was taken");
            return;
        }
        const char* failed = CheckRed();
        Cleanup();
        if (failed) UE_LOGE("[BIND-WAIT-SELFTEST] FAIL: %s", failed);
        else UE_LOGI("[BIND-WAIT-SELFTEST] RED ARM: bind_wait_red=1 refuses every hold (2 checks); no PASS by design");
        return;
    }
    const char* failed = CheckGreen();
    Cleanup();
    if (failed) UE_LOGE("[BIND-WAIT-SELFTEST] FAIL: %s", failed);
    else UE_LOGI("[BIND-WAIT-SELFTEST] PASS (21 checks)");
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!Enabled() || g_fired || !session || session->role() == coop::net::Role::Host) return;
    if (!coop::net_pump::HasAnnouncedWorldReady()) return;
    // The checks pump the whole due list, so a real hold must not be in it: wait, bounded, then do not run.
    if (BW::HasHolds()) {
        if (++g_waitTicks < kMaxWaitTicks) return;
        g_fired = true;
        UE_LOGW("[BIND-WAIT-SELFTEST] SKIPPED: real holds present");
        return;
    }
    g_fired = true;
    Run();
}

void OnDisconnect() { g_fired = false; g_waitTicks = 0; }

}  // namespace coop::dev::bind_wait_selftest
