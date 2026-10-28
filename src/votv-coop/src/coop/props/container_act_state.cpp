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

bool State::WaitsOnHolds(bool isPut, uint32_t eid, uint8_t slot) const {
    if (SlotHolds(slot)) return true;
    if (ReservedByOther(eid, slot)) return true;
    return isPut && EscrowOn(eid);
}

}  // namespace coop::props::container_act_state
