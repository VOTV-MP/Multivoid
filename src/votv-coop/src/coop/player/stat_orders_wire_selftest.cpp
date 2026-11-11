// coop/player/stat_orders_wire_selftest.cpp -- the un-gated boot selftest of the order / query
// wire's codec and of the host's token table, run once per process on both peers (shape:
// coop/atomic_file/atomic_file_selftest.cpp). Microseconds, no file, no engine: a codec that
// trusts a length or a float it was handed faults the host on a client's bytes, and a table that
// mints a zero token or answers the wrong slot answers an order nobody sent.
//
// Red arm: with the dev row selftest_break_stat_orders the first case expects the wrong token, so
// the run must fail.

#include "coop/player/stat_orders_wire.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"

#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace coop::stat_orders {
namespace {

using coop::net::StatOrderPayload;
using coop::net::StatOrderReplyPayload;
using coop::net::StatQueryReplyPayload;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

struct Tally {
    int pass = 0;
    int total = 0;
    void operator()(bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("stat_orders selftest FAIL: %s", what);
    }
};

Pending MakePending(uint8_t slot, uint32_t generation, Kind kind, uint64_t sentMs) {
    Pending e{};
    e.slot = slot;
    e.generation = generation;
    e.kind = kind;
    e.op = Op::SetStat;
    e.sentMs = sentMs;
    return e;
}

void RunTable(Tally& check, bool breakIt) {
    // The first token a table mints: the wrong one in the red arm.
    PendingTable t(0xFFFFFFFEu);
    uint32_t a = 0, b = 0, c = 0;
    const uint32_t expectFirst = breakIt ? 0xFFFFFFFDu : 0xFFFFFFFEu;
    check(t.Add(MakePending(1, 7, Kind::Order, 100), &a) && a == expectFirst,
          "table: the first token is the one the table was started with");
    check(t.Add(MakePending(1, 7, Kind::Order, 200), &b) && b == 0xFFFFFFFFu,
          "table: the next token follows");
    check(t.Add(MakePending(2, 7, Kind::Order, 300), &c) && c == 1u,
          "table: the token after the last value is 1, never 0");
    check(t.Count(1) == 2 && t.Count(2) == 1 && t.Count(9) == 0,
          "table: Count is per slot");
    check(t.OldestSentMs(1) == 100 && t.OldestSentMs(2) == 300 && t.OldestSentMs(9) == 0,
          "table: OldestSentMs is the slot's first-added entry, 0 for an empty slot");
    PendingTable zero(0);
    uint32_t z = 99;
    check(zero.Add(MakePending(0, 1, Kind::Query, 1), &z) && z == 1u,
          "table: a table started at 0 mints 1 first");

    // The allowances: 16 orders and one query per slot, each slot its own.
    uint32_t tok = 0;
    bool filled = true;
    for (int i = 0; i < PendingTable::kMaxOrders - 2; ++i)
        filled = t.Add(MakePending(1, 7, Kind::Order, 1000 + i), &tok) && filled;
    check(filled && t.Count(1) == PendingTable::kMaxOrders, "table: a slot takes 16 orders");
    check(!t.Add(MakePending(1, 7, Kind::Order, 2000), &tok), "table: the 17th order of a slot is refused");
    check(t.Add(MakePending(3, 7, Kind::Order, 2100), &tok), "table: another slot is unaffected by a full one");
    uint32_t q1 = 0, q2 = 0;
    int queryAnswers = 0;
    Pending query = MakePending(1, 7, Kind::Query, 3000);
    query.queryDone = [&queryAnswers](Result r, const TableView& v) {
        if (r == Result::Applied && v.effectTotal == 0) ++queryAnswers;
    };
    check(t.Add(std::move(query), &q1), "table: a slot with a full order allowance still takes a query");
    check(!t.Add(MakePending(1, 7, Kind::Query, 3100), &tok), "table: the 2nd query of a slot is refused");
    check(t.Add(MakePending(2, 7, Kind::Query, 3200), &q2), "table: another slot takes its own query");

    // Take: the right token, slot, kind and generation, else the entry stays.
    const int before = t.Count(1);
    Pending got;
    check(!t.Take(0x12345u, 1, 7, Kind::Order, &got), "table: Take refuses an unknown token");
    check(!t.Take(a, 2, 7, Kind::Order, &got), "table: Take refuses another slot's token");
    check(!t.Take(a, 1, 7, Kind::Query, &got), "table: Take refuses the other kind");
    check(!t.Take(a, 1, 8, Kind::Order, &got), "table: Take refuses a changed generation");
    check(t.Count(1) == before, "table: a refused Take leaves the entry");
    int orderAnswers = 0;
    Pending withCallback = MakePending(4, 3, Kind::Order, 4000);
    withCallback.orderDone = [&orderAnswers](const OrderAnswer& ans) {
        if (ans.result == Result::Left && !ans.storable && !ans.stored) ++orderAnswers;
    };
    uint32_t cb = 0;
    t.Add(std::move(withCallback), &cb);
    check(t.Take(cb, 4, 3, Kind::Order, &got) && got.token == cb && got.slot == 4 && got.kind == Kind::Order,
          "table: Take returns the entry for the right token, slot, kind and generation");
    if (got.orderDone) got.orderDone(OrderAnswer{Result::Left, 0.0f, false, false});
    check(orderAnswers == 1, "table: the entry carries its callback");
    check(!t.Take(cb, 4, 3, Kind::Order, &got) && t.Count(4) == 0, "table: a taken token is gone");
    check(t.Take(a, 1, 7, Kind::Order, &got) && got.token == a && t.Count(1) == before - 1,
          "table: Take removes one entry");

    // TakeOldest: the first-added of that slot and kind, with the generation Take checks.
    const int oldestCount = t.Count(1);
    const uint64_t oldestStamp = t.OldestSentMs(1);
    check(!t.TakeOldest(1, 8, Kind::Order, &got), "table: TakeOldest refuses a changed generation");
    check(t.Count(1) == oldestCount && t.OldestSentMs(1) == oldestStamp,
          "table: a refused TakeOldest leaves the entry");
    check(t.TakeOldest(1, 7, Kind::Order, &got) && got.token == b && got.sentMs == 200 && got.generation == 7,
          "table: TakeOldest returns the first-added order of the slot with the matching generation");
    check(t.TakeOldest(1, 7, Kind::Query, &got) && got.token == q1, "table: TakeOldest honours the kind");
    if (got.queryDone) got.queryDone(Result::Applied, TableView{});
    check(queryAnswers == 1, "table: a query entry carries its callback");
    check(!t.TakeOldest(9, 7, Kind::Order, &got), "table: TakeOldest is false for a slot that holds none");
    uint32_t g3 = 0, g4 = 0;
    t.Add(MakePending(5, 3, Kind::Order, 6000), &g3);
    t.Add(MakePending(5, 4, Kind::Order, 6100), &g4);
    check(!t.TakeOldest(5, 4, Kind::Order, &got) && t.Count(5) == 2 && t.OldestSentMs(5) == 6000,
          "table: TakeOldest judges the generation of the oldest entry, not of a later one");
    check(t.TakeOldest(5, 3, Kind::Order, &got) && got.token == g3 && t.TakeOldest(5, 4, Kind::Order, &got) &&
              got.token == g4 && t.Count(5) == 0,
          "table: TakeOldest takes each entry once its own generation is named");
    check(t.OldestSentMs(1) == 1000, "table: OldestSentMs follows the removals");

    // TakeSlot and TakeAll.
    const std::vector<Pending> slotOne = t.TakeSlot(1);
    bool onlyOne = true;
    for (const Pending& e : slotOne) onlyOne = onlyOne && e.slot == 1;
    check(slotOne.size() == static_cast<size_t>(PendingTable::kMaxOrders - 2) && onlyOne,
          "table: TakeSlot returns exactly that slot's entries");
    check(t.Count(1) == 0 && t.Count(2) == 2 && t.Count(3) == 1 && t.OldestSentMs(1) == 0,
          "table: TakeSlot leaves the other slots");
    uint32_t again = 0;
    check(t.Add(MakePending(1, 7, Kind::Order, 5000), &again), "table: a slot's allowance is free again after TakeSlot");
    const std::vector<Pending> all = t.TakeAll();
    check(all.size() == 4 && t.Count(1) == 0 && t.Count(2) == 0 && t.Count(3) == 0 && t.OldestSentMs(2) == 0,
          "table: TakeAll empties the table");
}

OrderWire SampleOrder(Op op) {
    OrderWire o;
    o.token = 0x01020304u;
    o.op = op;
    o.field = 3;
    o.value = 42.5f;
    o.strength = 1.5f;
    o.seconds = 30.0f;
    o.effect = (op == Op::SetStat) ? std::string() : std::string("foodPoison");
    return o;
}

bool SameOrder(const OrderWire& x, const OrderWire& y) {
    return x.token == y.token && x.op == y.op && x.field == y.field && x.value == y.value &&
           x.strength == y.strength && x.seconds == y.seconds && x.effect == y.effect;
}

void RunOrderCodec(Tally& check) {
    check(ValidEffectName("vaccine_a") && ValidEffectName("abcdefghijk"), "codec: a name of 1-11 printable characters is valid");
    check(!ValidEffectName("") && !ValidEffectName("abcdefghijkl") && !ValidEffectName("a\nb") &&
              !ValidEffectName(std::string("a\0b", 3)),
          "codec: an empty, a 12-character, a control-character and an embedded-NUL name are not");
    check(std::strcmp(ResultText(Result::Applied), "Applied") == 0 && std::strcmp(ResultText(Result::Left), "Left") == 0 &&
              std::strcmp(ResultText(Result::Malformed), "Malformed") == 0,
          "codec: ResultText names the result");
    check(std::strcmp(OpText(Op::SetStat), "set") == 0 && std::strcmp(OpText(Op::AddEffect), "add") == 0 &&
              std::strcmp(OpText(Op::RemoveEffect), "remove") == 0,
          "codec: OpText names the op");
    bool resultTexts = true;
    for (const Result r : {Result::Applied, Result::Refused, Result::NotFound, Result::NotReady, Result::Left,
                           Result::Malformed}) {
        const char* text = ResultText(r);
        resultTexts = resultTexts && text != nullptr && text[0] != '\0';
    }
    check(resultTexts, "codec: every Result value has a non-empty text");
    bool opTexts = true;
    for (const Op op : {Op::SetStat, Op::AddEffect, Op::RemoveEffect}) {
        const char* text = OpText(op);
        opTexts = opTexts && text != nullptr && text[0] != '\0';
    }
    check(opTexts, "codec: every Op value has a non-empty text");

    for (const Op op : {Op::SetStat, Op::AddEffect, Op::RemoveEffect}) {
        OrderWire in = SampleOrder(op);
        if (op != Op::SetStat) in.field = 0;   // an effect op's field travels as 0
        StatOrderPayload p;
        OrderWire out;
        check(PackOrder(SampleOrder(op), &p) && UnpackOrder(&p, sizeof p, &out) && SameOrder(in, out),
              "codec: an order round-trips");
    }
    OrderWire fieldy = SampleOrder(Op::AddEffect);
    StatOrderPayload p;
    OrderWire out;
    check(PackOrder(fieldy, &p) && p.field == 0, "codec: an effect op's field is packed as 0");
    OrderWire setty = SampleOrder(Op::SetStat);
    setty.effect = "ignored";
    check(PackOrder(setty, &p) && p.effect[0] == 0 && UnpackOrder(&p, sizeof p, &out) && out.effect.empty() && out.field == 3,
          "codec: a SetStat's effect is not packed");
    OrderWire bad = SampleOrder(Op::AddEffect);
    bad.effect = "abcdefghijkl";
    check(!PackOrder(bad, &p), "codec: PackOrder is false for a 12-character name");
    bad.effect.clear();
    check(!PackOrder(bad, &p) && !PackOrder([&] { bad.op = Op::RemoveEffect; return bad; }(), &p),
          "codec: PackOrder is false for an empty name on an add and on a remove");
    bad.effect = "a\x01" "b";
    check(!PackOrder(bad, &p), "codec: PackOrder is false for an unprintable name");

    StatOrderPayload good;
    PackOrder(SampleOrder(Op::AddEffect), &good);
    check(!UnpackOrder(&good, sizeof good - 1, &out) && !UnpackOrder(&good, sizeof good + 1, &out) && !UnpackOrder(&good, 0, &out),
          "codec: UnpackOrder is false for a wrong length");
    StatOrderPayload raw = good;
    std::memset(raw.effect, 0, sizeof raw.effect);
    raw.effect[0] = 'a';
    raw.effect[1] = 0x07;
    raw.effect[2] = 'b';
    check(UnpackOrder(&raw, sizeof raw, &out) && out.effect == "a?b", "codec: an unprintable name byte reads '?'");
    std::memset(raw.effect, 'x', sizeof raw.effect);
    check(UnpackOrder(&raw, sizeof raw, &out) && out.effect.size() == 12 && !ValidEffectName(out.effect),
          "codec: a name with no NUL is read to 12 bytes and is not valid");
}

void RunReplyCodec(Tally& check) {
    uint32_t token = 0;
    Result r = Result::Left;
    Op op = Op::SetStat;
    float now = -1.0f;
    StatOrderReplyPayload rp = PackOrderReply(0xABCDu, Result::Applied, Op::AddEffect, 2.0f);
    check(UnpackOrderReply(&rp, sizeof rp, &token, &r, &op, &now) && token == 0xABCDu && r == Result::Applied &&
              op == Op::AddEffect && now == 2.0f,
          "codec: an order reply round-trips");
    rp = PackOrderReply(5, Result::Refused, Op::SetStat, 7.0f);
    check(rp.valueNow == 0.0f && UnpackOrderReply(&rp, sizeof rp, &token, &r, &op, &now) && r == Result::Refused && now == 0.0f,
          "codec: valueNow travels only for Applied");
    rp = PackOrderReply(5, Result::NotReady, Op::SetStat, 0.0f);
    check(UnpackOrderReply(&rp, sizeof rp, &token, &r, &op, &now) && r == Result::NotReady,
          "codec: NotReady is a wire result");
    rp = PackOrderReply(5, Result::Left, Op::SetStat, 0.0f);
    check(!UnpackOrderReply(&rp, sizeof rp, &token, &r, &op, &now), "codec: UnpackOrderReply is false for result 4");
    rp = PackOrderReply(5, Result::Applied, Op::SetStat, kNaN);
    check(!UnpackOrderReply(&rp, sizeof rp, &token, &r, &op, &now), "codec: UnpackOrderReply is false for an Applied NaN");
    rp = PackOrderReply(5, Result::Refused, Op::SetStat, 0.0f);
    rp.valueNow = kNaN;
    now = 9.0f;
    check(UnpackOrderReply(&rp, sizeof rp, &token, &r, &op, &now) && now == 0.0f,
          "codec: a non-Applied valueNow reads 0");
    check(!UnpackOrderReply(&rp, sizeof rp - 1, &token, &r, &op, &now) && !UnpackOrderReply(&rp, sizeof rp + 1, &token, &r, &op, &now),
          "codec: UnpackOrderReply is false for a wrong length");

    const coop::net::StatQueryPayload qp = PackQuery(0xFEEDu);
    uint32_t qt = 0;
    check(UnpackQuery(&qp, sizeof qp, &qt) && qt == 0xFEEDu, "codec: a query round-trips");
    check(!UnpackQuery(&qp, sizeof qp - 1, &qt) && !UnpackQuery(&qp, sizeof qp + 1, &qt), "codec: UnpackQuery is false for a wrong length");
}

TableView SampleView(size_t effects) {
    TableView v{};
    v.validMask = (1u << coop::net::kStatRows) - 1u;
    for (int i = 0; i < coop::net::kStatRows; ++i) v.values[i] = 10.0f + static_cast<float>(i);
    const char* names[] = {"vaccine", "lsd", "foodPoison", "nausea", "sleepy", "vaccine_a", "poo"};
    for (size_t i = 0; i < effects; ++i)
        v.effects.push_back(EffectView{names[i % 7], (i % 2) == 0, 1.0f + static_cast<float>(i), 20.0f - static_cast<float>(i)});
    v.effectTotal = static_cast<uint8_t>(effects);
    return v;
}

void RunQueryReplyCodec(Tally& check) {
    uint32_t token = 0;
    Result r = Result::Left;
    TableView out;
    const TableView seven = SampleView(7);
    StatQueryReplyPayload p = PackQueryReply(77, Result::Applied, seven);
    check(p.effectCount == 5 && p.effectTotal == 7 && p.result == 0, "codec: a view of 7 effects packs 5 and total 7");
    check(UnpackQueryReply(&p, sizeof p, &token, &r, &out) && token == 77 && r == Result::Applied && out.effects.size() == 5 &&
              out.effectTotal == 7,
          "codec: a query reply unpacks to 5 entries and total 7");
    bool same = out.validMask == seven.validMask;
    for (int i = 0; i < coop::net::kStatRows; ++i) same = same && out.values[i] == seven.values[i];
    for (size_t i = 0; i < out.effects.size(); ++i)
        same = same && out.effects[i].name == seven.effects[i].name && out.effects[i].live == seven.effects[i].live &&
               out.effects[i].strength == seven.effects[i].strength && out.effects[i].time == seven.effects[i].time;
    check(same, "codec: a query reply round-trips its mask, values and entries");

    TableView longName = SampleView(1);
    longName.effects[0].name = "abcdefghijklmnop";
    p = PackQueryReply(1, Result::Applied, longName);
    check(UnpackQueryReply(&p, sizeof p, &token, &r, &out) && out.effects.size() == 1 && out.effects[0].name == "abcdefghijk",
          "codec: a name over 11 characters is cut");

    p = PackQueryReply(1, Result::Applied, seven);
    p.effectCount = 200;
    p.effectTotal = 3;
    check(UnpackQueryReply(&p, sizeof p, &token, &r, &out) && out.effects.size() == 3 && out.effectTotal == 3,
          "codec: effectCount 200 with total 3 yields 3 entries");
    p.effectTotal = 200;
    check(UnpackQueryReply(&p, sizeof p, &token, &r, &out) && out.effects.size() == 5,
          "codec: effectCount 200 with total 200 yields 5 entries");

    p = PackQueryReply(1, Result::Applied, seven);
    p.values[4] = kNaN;
    check(UnpackQueryReply(&p, sizeof p, &token, &r, &out) && (out.validMask & (1u << 4)) == 0 && out.values[4] == 0.0f &&
              (out.validMask & (1u << 5)) != 0,
          "codec: a NaN value clears its bit and reads 0");
    p = PackQueryReply(1, Result::Applied, seven);
    p.validMask = 0xFFFFFFFFu;
    check(UnpackQueryReply(&p, sizeof p, &token, &r, &out) && out.validMask == 0xFFFFFu, "codec: validMask is masked to 20 bits");
    p = PackQueryReply(1, Result::Applied, seven);
    p.effects[0].name[1] = 0x07;
    check(UnpackQueryReply(&p, sizeof p, &token, &r, &out) && out.effects.size() == 5 && out.effects[0].name == "v?ccine",
          "codec: an unprintable name byte reads '?'");
    p = PackQueryReply(1, Result::Applied, seven);
    p.effects[1].time = kInf;
    p.effects[2].strength = kNaN;
    check(UnpackQueryReply(&p, sizeof p, &token, &r, &out) && out.effects.size() == 3 && out.effectTotal == 7,
          "codec: an entry whose strength or time is not finite is dropped");

    p = PackQueryReply(9, Result::NotReady, seven);
    check(UnpackQueryReply(&p, sizeof p, &token, &r, &out) && token == 9 && r == Result::NotReady && out.validMask == 0 &&
              out.effectTotal == 0 && out.effects.empty() && out.values[0] == 0.0f,
          "codec: a non-Applied reply gives a zeroed view");
    p = PackQueryReply(9, Result::Left, seven);
    check(!UnpackQueryReply(&p, sizeof p, &token, &r, &out), "codec: UnpackQueryReply is false for result 4");
    p = PackQueryReply(9, Result::Applied, seven);
    check(!UnpackQueryReply(&p, sizeof p - 1, &token, &r, &out) && !UnpackQueryReply(&p, sizeof p + 1, &token, &r, &out) &&
              !UnpackQueryReply(&p, 0, &token, &r, &out),
          "codec: UnpackQueryReply is false for a wrong length");
}

bool RunSelftestBody() {
    Tally check;
    const bool breakIt = coop::config::ResolveFlag(coop::config_registry::rows::selftest_break_stat_orders);
    RunTable(check, breakIt);
    RunOrderCodec(check);
    RunReplyCodec(check);
    RunQueryReplyCodec(check);
    if (check.pass == check.total) {
        UE_LOGI("stat_orders selftest: ALL PASS (%d checks)", check.total);
        return true;
    }
    UE_LOGE("stat_orders selftest: %d/%d checks passed", check.pass, check.total);
    return false;
}

}  // namespace

// Once per process: the session-runtime call is once per session start, and a later call returns
// the first run's verdict without output.
bool RunSelftest() {
    static std::once_flag ran;
    static bool verdict = false;
    std::call_once(ran, [] { verdict = RunSelftestBody(); });
    return verdict;
}

}  // namespace coop::stat_orders
