// coop/dev/cheat_drill_raw.cpp -- see coop/dev/cheat_drill_raw.h.

#include "coop/dev/cheat_drill_raw.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"

#include <cstring>
#include <vector>

namespace coop::dev::cheat_drill {
namespace {

using coop::net::CheatOp;
using coop::net::CheatReason;
using coop::net::CheatRequestPayload;

constexpr uint32_t kRawIdBase = 0x80000000u;   // above any id the panel path hands out

CheatRequestPayload Make(size_t index, CheatOp op, const char* name, int count, int amount, int target) {
    CheatRequestPayload p{};
    p.id = kRawIdBase + static_cast<uint32_t>(index);
    p.op = static_cast<uint8_t>(op);
    p.count = static_cast<uint8_t>(count);
    p.amount = amount;
    p.target = static_cast<uint8_t>(target);
    if (name) {
        const size_t n = std::strlen(name);
        std::memcpy(p.name, name, n < sizeof(p.name) - 1 ? n : sizeof(p.name) - 1);
    }
    return p;
}

std::vector<RawCase> Build() {
    std::vector<RawCase> v;
    auto add = [&](const char* what, CheatRequestPayload p, CheatReason want) {
        p.id = kRawIdBase + static_cast<uint32_t>(v.size());
        v.push_back({what, p, want});
    };
    constexpr int kFront = coop::net::kCheatInFront;
    add("an unknown op", Make(0, static_cast<CheatOp>(9), "x", 1, 0, kFront), CheatReason::BadRequest);
    CheatRequestPayload open = Make(0, CheatOp::Event, nullptr, 1, 0, kFront);
    std::memset(open.name, 'A', sizeof(open.name));   // no terminator inside the field
    add("a name with no end", open, CheatReason::BadRequest);
    CheatRequestPayload wide = Make(0, CheatOp::Event, nullptr, 1, 0, kFront);
    wide.name[0] = static_cast<char>(0xC3);
    wide.name[1] = static_cast<char>(0xA9);
    add("a name that is not ASCII", wide, CheatReason::BadRequest);
    add("a spawn of count 0", Make(0, CheatOp::SpawnItem, "crowbar", 0, 0, kFront), CheatReason::BadRequest);
    add("a spawn of count 11", Make(0, CheatOp::SpawnItem, "crowbar", 11, 0, kFront), CheatReason::BadRequest);
    add("a spawn of count 255", Make(0, CheatOp::SpawnItem, "crowbar", 255, 0, kFront), CheatReason::BadRequest);
    add("points of 0", Make(0, CheatOp::Points, nullptr, 1, 0, kFront), CheatReason::BadRequest);
    add("points of +100001", Make(0, CheatOp::Points, nullptr, 1, 100001, kFront), CheatReason::BadRequest);
    add("points of -100001", Make(0, CheatOp::Points, nullptr, 1, -100001, kFront), CheatReason::BadRequest);
    add("a spawn at a slot nobody holds", Make(0, CheatOp::SpawnItem, "crowbar", 1, 0, 200), CheatReason::NoTarget);
    add("an event that is not safe", Make(0, CheatOp::Event, "treehouseSleep", 1, 0, kFront), CheatReason::Refused);
    add("an explosive prop", Make(0, CheatOp::SpawnItem, "pipebomb", 1, 0, kFront), CheatReason::Refused);
    add("an event the game lacks", Make(0, CheatOp::Event, "noSuchEvent_xyz", 1, 0, kFront), CheatReason::UnknownEvent);
    return v;
}

const std::vector<RawCase>& Cases() {
    static const std::vector<RawCase> v = Build();
    return v;
}

}  // namespace

size_t RawCaseCount() { return Cases().size(); }
const RawCase& RawCaseAt(size_t i) { return Cases()[i]; }

CheatRequestPayload RawSpawn(uint32_t id, const char* name, int count) {
    CheatRequestPayload p = Make(0, CheatOp::SpawnItem, name, count, 0, coop::net::kCheatInFront);
    p.id = id;
    return p;
}

bool SendRawForDrill(coop::net::Session& s, const CheatRequestPayload& p) {
    static const bool on = coop::config::ResolveEnum(::coop::config_registry::rows::cheat_drill) == "on";
    if (!on || s.role() != coop::net::Role::Client) return false;
    return s.SendReliableToSlot(0, coop::net::ReliableKind::CheatRequest, &p, sizeof(p));
}

}  // namespace coop::dev::cheat_drill
