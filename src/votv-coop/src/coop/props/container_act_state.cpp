// coop/props/container_act_state.cpp -- see coop/props/container_act_state.h.

#include "coop/props/container_act_state.h"

#include <algorithm>
#include <utility>

namespace coop::props::container_act_state {

State& Live() {
    static State s;
    return s;
}

std::vector<Hold> State::TakeIf(bool (*pred)(const Hold&, uint64_t), uint64_t arg) {
    std::vector<Hold> out;
    for (auto it = holds_.begin(); it != holds_.end();) {
        if (pred(*it, arg)) {
            out.push_back(std::move(*it));
            it = holds_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

bool State::Open(Hold h) {
    if (SlotHolds(h.slot)) return false;
    holds_.push_back(std::move(h));
    return true;
}

const Hold* State::Match(uint8_t slot, uint16_t id) const {
    for (const Hold& h : holds_)
        if (h.slot == slot && h.id == id) return &h;
    return nullptr;
}

std::optional<Hold> State::Close(uint8_t slot, uint16_t id) {
    for (auto it = holds_.begin(); it != holds_.end(); ++it) {
        if (it->slot != slot || it->id != id) continue;
        Hold h = std::move(*it);
        holds_.erase(it);
        return h;
    }
    return std::nullopt;
}

bool State::SlotHolds(uint8_t slot) const {
    for (const Hold& h : holds_)
        if (h.slot == slot) return true;
    return false;
}

bool State::ReservedByOther(uint32_t eid, uint8_t slot) const {
    for (const Hold& h : holds_)
        if (h.kind == HoldKind::Reservation && h.eid == eid && h.slot != slot) return true;
    return false;
}

bool State::EscrowOn(uint32_t eid) const {
    for (const Hold& h : holds_)
        if (h.kind == HoldKind::Escrow && h.eid == eid) return true;
    return false;
}

std::vector<uint32_t> State::HeldEids() const {
    std::vector<uint32_t> out;
    for (const Hold& h : holds_)
        if (std::find(out.begin(), out.end(), h.eid) == out.end()) out.push_back(h.eid);
    return out;
}

std::vector<Hold> State::TakeExpired(uint64_t nowMs) {
    return TakeIf([](const Hold& h, uint64_t now) { return h.grantedMs + kHoldDeadlineMs <= now; }, nowMs);
}

std::vector<Hold> State::TakeForSlot(uint8_t slot) {
    return TakeIf([](const Hold& h, uint64_t s) { return h.slot == s; }, slot);
}

std::vector<Hold> State::TakeForEid(uint32_t eid) {
    return TakeIf([](const Hold& h, uint64_t e) { return h.eid == e; }, eid);
}

std::vector<Hold> State::TakeAll() { return std::exchange(holds_, {}); }

size_t State::Count() const { return holds_.size(); }

bool State::HoldOn(uint32_t eid) const {
    for (const Hold& h : holds_)
        if (h.eid == eid) return true;
    return false;
}

void State::Lapse(const Hold& h, uint64_t nowMs) {
    Tombstone t;
    t.slot = h.slot;
    t.id = h.id;
    t.eid = h.eid;
    t.className = h.record.className;
    ue_wrap::save_record::RecordName(h.record, t.name);
    t.atMs = nowMs;
    size_t ofSlot = 0;
    for (const Tombstone& o : tombstones_) ofSlot += o.slot == h.slot ? 1 : 0;
    if (ofSlot >= kTombstonesPerSlot) {   // the oldest of the slot goes: they are kept in the order they lapsed
        const auto oldest = std::find_if(tombstones_.begin(), tombstones_.end(),
                                         [&](const Tombstone& o) { return o.slot == h.slot; });
        tombstones_.erase(oldest);
    }
    tombstones_.push_back(std::move(t));
    const auto mark = std::find_if(slowed_.begin(), slowed_.end(), [&](const Slowing& s) { return s.slot == h.slot; });
    if (mark != slowed_.end()) mark->untilMs = nowMs + kSlowedMs;
    else slowed_.push_back(Slowing{h.slot, nowMs + kSlowedMs});
}

std::optional<Tombstone> State::ConsumeTombstone(uint8_t slot, uint16_t id, uint32_t eid) {
    for (auto it = tombstones_.begin(); it != tombstones_.end(); ++it) {
        if (it->slot != slot || it->id != id || it->eid != eid) continue;
        Tombstone t = std::move(*it);
        tombstones_.erase(it);
        slowed_.erase(std::remove_if(slowed_.begin(), slowed_.end(), [&](const Slowing& s) { return s.slot == slot; }),
                      slowed_.end());
        return t;
    }
    return std::nullopt;
}

bool State::Slowed(uint8_t slot, uint64_t nowMs) const {
    for (const Slowing& s : slowed_)
        if (s.slot == slot && nowMs < s.untilMs) return true;
    return false;
}

void State::SweepLapsed(uint64_t nowMs) {
    tombstones_.erase(std::remove_if(tombstones_.begin(), tombstones_.end(),
                                     [&](const Tombstone& t) { return t.atMs + kTombstoneMs <= nowMs; }),
                      tombstones_.end());
    slowed_.erase(std::remove_if(slowed_.begin(), slowed_.end(), [&](const Slowing& s) { return s.untilMs <= nowMs; }),
                  slowed_.end());
}

bool State::HasLapsed() const { return !tombstones_.empty() || !slowed_.empty(); }

size_t State::TombstoneCount() const { return tombstones_.size(); }

void State::DropLapsedOf(uint8_t slot) {
    tombstones_.erase(std::remove_if(tombstones_.begin(), tombstones_.end(),
                                     [&](const Tombstone& t) { return t.slot == slot; }),
                      tombstones_.end());
    slowed_.erase(std::remove_if(slowed_.begin(), slowed_.end(), [&](const Slowing& s) { return s.slot == slot; }),
                  slowed_.end());
}

void State::DropLapsed() {
    tombstones_.clear();
    slowed_.clear();
}

bool State::WaitsOnHolds(bool isPut, uint32_t eid, uint8_t slot) const {
    if (SlotHolds(slot)) return true;
    if (ReservedByOther(eid, slot)) return true;
    return isPut && EscrowOn(eid);
}

}  // namespace coop::props::container_act_state
