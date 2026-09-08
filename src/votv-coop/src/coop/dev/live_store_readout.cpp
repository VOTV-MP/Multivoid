// coop/dev/live_store_readout.cpp -- see coop/dev/live_store_readout.h.

#include "coop/dev/live_store_readout.h"

#include "coop/config/config.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/core/log.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace coop::dev::live_store_readout {
namespace {

namespace INV = ue_wrap::inventory;
namespace SR  = ue_wrap::save_record;

constexpr int kPollTicks = 240;    // ~2 s at ~120 Hz -- this is an observer, not a hot path
constexpr int kMaxDiffLines = 12;  // a diff longer than this is summarised, never spammed

// The identity a HUMAN reads a gap by: class + save key. Not an authority key and not a wire
// identity -- deliberately, since what "the same item" means across peers is exactly the open
// question this readout exists to inform (BRIEF Q4), not something to quietly decide here.
// Explicit narrowing: these are engine class/FName leaves (ASCII in practice), and an implicit
// wstring->string iterator copy warns and silently mangles anything that is not. Non-ASCII becomes
// '?' so a surprising name is VISIBLE in the log rather than quietly truncated.
std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (const wchar_t c : w)
        s.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?');
    return s;
}

// The record's PAYLOAD SHAPE -- total elements per value group, non-empty groups only.
//
// Printed next to the identity because `Aprop_container_C::extract` captures the carrier actor
// through addObject -> getData BEFORE `loadData(takeObj_Output)` restores the saved state, which
// predicted that an item passing through a take would land holding a DEFAULT payload while its
// class and transform survive. The measurement REFUTED that: the taken record printed a
// non-empty shape. The control is built into the same line: this store's save-loaded records
// never went through a take, so they stand as the positive case beside a taken one.
//
// Read the result narrowly. Shapes differ BY CLASS -- crowbar b5,f1,nm2, food b6,f4,i1,nm2, a drive
// one signal more -- so a group count is a CLASS FINGERPRINT and says nothing about the VALUES
// inside. A familiar-looking shape here is NOT evidence that the saved state survived the take.
// That needs a value comparison against the source container record, and a prop_drive_C carrying a
// signal is the best candidate, since a fresh one should carry none. An empty shape on EVERY
// record would mean this instrument is blind, not that the payloads are empty.
std::string ShapeOf(const SR::SaveRecord& r) {
    auto n2 = [](const auto& vv) {
        size_t t = 0;
        for (const auto& v : vv) t += v.size();
        return t;
    };
    const std::pair<const char*, size_t> groups[] = {
        {"b", n2(r.bools)},    {"f", n2(r.floats)},     {"i", n2(r.ints)},
        {"s", n2(r.strings)},  {"sig", r.signals.size()}, {"cls", n2(r.classes)},
        {"v", n2(r.vectors)},  {"rot", n2(r.rotators)}, {"x", n2(r.transforms)},
        {"by", n2(r.bytes)},   {"nm", n2(r.names)},
    };
    std::string s;
    for (const auto& [tag, n] : groups) {
        if (!n) continue;
        if (!s.empty()) s += ",";
        s += std::string(tag) + std::to_string(n);
    }
    return s.empty() ? "EMPTY" : s;
}

std::string SigOf(const SR::SaveRecord& r) {
    std::string cls = Narrow(r.className);
    std::string key = Narrow(r.key);
    if (cls.empty()) cls = "<null-class>";
    if (key.empty()) key = "<no-key>";
    return cls + "|" + key + "{" + ShapeOf(r) + "}";
}

using Bag = std::map<std::string, int>;

Bag BagOf(const std::vector<SR::SaveRecord>& recs) {
    Bag b;
    for (const auto& r : recs) ++b[SigOf(r)];
    return b;
}

// Multiset difference a-minus-b, flattened to "sig xN" strings.
std::vector<std::string> MinusOf(const Bag& a, const Bag& b) {
    std::vector<std::string> out;
    for (const auto& [sig, n] : a) {
        auto it = b.find(sig);
        const int extra = n - (it == b.end() ? 0 : it->second);
        if (extra > 0)
            out.push_back(extra > 1 ? (sig + " x" + std::to_string(extra)) : sig);
    }
    return out;
}

std::string Join(const std::vector<std::string>& v) {
    if (v.empty()) return "(none)";
    std::string s;
    for (size_t i = 0; i < v.size() && i < kMaxDiffLines; ++i) {
        if (i) s += ", ";
        s += v[i];
    }
    if (v.size() > kMaxDiffLines)
        s += ", ... (+" + std::to_string(v.size() - kMaxDiffLines) + " more)";
    return s;
}

std::string StateKey(int32_t idx, const Bag& live, const Bag& proj) {
    std::string s = "i" + std::to_string(idx) + ";L";
    for (const auto& [sig, n] : live) s += sig + "*" + std::to_string(n) + ",";
    s += ";P";
    for (const auto& [sig, n] : proj) s += sig + "*" + std::to_string(n) + ",";
    return s;
}

}  // namespace

void Tick() {
    static const bool s_on = ::coop::config::ResolveFlag(::coop::config_registry::rows::live_store_readout);
    if (!s_on) return;
    static int s_ticks = 0;
    if (++s_ticks < kPollTicks) return;
    s_ticks = 0;

    INV::LivePersonalStore live;
    if (!INV::ReadLivePersonalStore(live)) {
        // Not an error: pre-world / pre-saveSlot this is simply not resolvable yet. Log ONCE so a
        // permanently-inert reader is visible, then stay quiet.
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            UE_LOGI("live_store: not resolvable yet (no gamemode/playerContainer/GObjStack slot) "
                    "-- will keep polling silently until it is");
        }
        return;
    }

    INV::PlayerInventory proj;
    const bool projOk = INV::ReadAll(proj);

    const Bag liveBag = BagOf(live.records);
    const Bag projBag = projOk ? BagOf(proj.inventory) : Bag{};

    // Log only when something CHANGED -- a per-2s heartbeat of an unchanged state is log spam, and
    // the pre-deploy checklist treats a line repeating at a rate the design did not intend as a bug.
    static std::string s_last;
    const std::string now = StateKey(live.slotIndex, liveBag, projBag);
    if (now == s_last) return;
    const bool first = s_last.empty();
    s_last = now;

    const std::vector<std::string> liveOnly = MinusOf(liveBag, projBag);
    const std::vector<std::string> projOnly = MinusOf(projBag, liveBag);
    const int gap = static_cast<int>(live.records.size()) -
                    static_cast<int>(projOk ? proj.inventory.size() : 0);

    UE_LOGI("live_store: %s GObjStack[%d] live=%zu proj=%s gap=%+d | equip=%zu hold=%zu",
            first ? "FIRST" : "CHANGE", live.slotIndex, live.records.size(),
            projOk ? std::to_string(proj.inventory.size()).c_str() : "READ-FAILED",
            gap, proj.equipment.size(), proj.hold.size());
    UE_LOGI("live_store:   live-only (carried, absent from the projection): %s",
            Join(liveOnly).c_str());
    UE_LOGI("live_store:   proj-only (in the projection, not carried):      %s",
            Join(projOnly).c_str());
}

}  // namespace coop::dev::live_store_readout
