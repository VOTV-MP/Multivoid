// coop/props/container_overlap_pure.h -- the decisions and the tables of the host's judge of a put at its own overlap
// (container_act_host_overlap.cpp), with no engine call, so the selftest drives them. The judge's ORDER is Decide, which
// names the next input it needs until it can answer; the wait table and the refusal latch are templates over the reference
// type they hold, so the selftest holds a fake and the host holds a ue_wrap::CachedObjRef. A Ref has Set(void*), Raw(),
// Alive() and Is(void*). The values are design values (the sheet's v4.1): 8 rows in each table, the oldest dropped, and
// an unread put waits for its readiness edge at most kUnreadWaitMs.

#pragma once

#include "coop/props/container_act_host.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace coop::props::container_act_host::overlap::pure {

inline constexpr size_t kTableRows = 8;
// A birth's reliable round trip with margin: an unread put waits this long for its eid's bind or its container's change.
inline constexpr uint64_t kUnreadWaitMs = 2000;

// ---- the judge's order ----

enum class Judged : uint8_t { Pass, Wait, Refuse };
enum class Need : uint8_t { Nothing, Armed, Reach, Hold, Gates, Fit };
enum class ReachRead : uint8_t { Ok, OutOfReach, Unread };
enum class GateRead : uint8_t { Pass, NoMesh, IgnoreSaveUnread, Ignored, CanPickupUnread, CannotPickup };
enum class FitRead : uint8_t { Fits, Unread, Refused };

// What has been read so far; a flag says whether its input is.
struct Inputs {
    bool needsReach = false;   // a held or claimed driver: an elected throw has none
    bool armedKnown = false, armed = false;
    bool reachKnown = false;
    ReachRead reach = ReachRead::Ok;
    bool holdKnown = false, held = false;
    bool gatesKnown = false;
    GateRead gate = GateRead::Pass;
    bool fitKnown = false;
    FitRead fit = FitRead::Fits;
};

struct Decision {
    Judged judged = Judged::Pass;
    Tally tally = Tally::kCount;   // the counter of a refusal or a wait
    const char* why = "";
    bool transient = false;        // a Wait on a read that did not come, not on a hold
    Need need = Need::Nothing;     // the input to read before the judge can answer
};

// Order: armed, then (a held or claimed driver only) reach, then a client's hold, then the body's silent gates, then the
// fit, which reads the whole container and so comes last. The first term that decides answers; an input not read yet is
// asked for.
inline Decision Decide(const Inputs& in) {
    Decision d;
    const auto ask = [&d](Need n) { d.need = n; return d; };
    const auto refuse = [&d](Tally t, const char* why) { d.judged = Judged::Refuse; d.tally = t; d.why = why; return d; };
    if (!in.armedKnown) return ask(Need::Armed);
    if (!in.armed) return refuse(Tally::PutOverlapUnarmed, "the container's box is not armed");
    if (in.needsReach) {
        if (!in.reachKnown) return ask(Need::Reach);
        if (in.reach == ReachRead::OutOfReach) return refuse(Tally::PutOverlapReach, "the driver is out of reach");
        if (in.reach == ReachRead::Unread) return refuse(Tally::PutOverlapUnread, "the driver's reach did not read");
    }
    if (!in.holdKnown) return ask(Need::Hold);
    if (in.held) {
        d.judged = Judged::Wait;
        d.tally = Tally::PutOverlapHeld;
        d.why = "a client holds the container";
        return d;
    }
    if (!in.gatesKnown) return ask(Need::Gates);
    switch (in.gate) {
        case GateRead::Pass:             break;
        case GateRead::NoMesh:           return refuse(Tally::PutOverlapGate, "the prop has no mesh");
        case GateRead::IgnoreSaveUnread: return refuse(Tally::PutOverlapGate, "ignoreSave does not read");
        case GateRead::Ignored:          return refuse(Tally::PutOverlapGate, "the prop is not saved");
        case GateRead::CanPickupUnread:  return refuse(Tally::PutOverlapGate, "canPickup does not read");
        case GateRead::CannotPickup:     return refuse(Tally::PutOverlapGate, "the prop cannot be picked up");
    }
    if (!in.fitKnown) return ask(Need::Fit);
    if (in.fit == FitRead::Unread) {
        d.judged = Judged::Wait;
        d.tally = Tally::PutOverlapUnread;
        d.why = "a read the fit needs did not come";
        d.transient = true;
        return d;
    }
    if (in.fit == FitRead::Refused) return refuse(Tally::PutOverlapFit, "the put terms refuse it");
    return d;
}

// ---- the wait table ----

// Hold: waits on a client's hold on the container, released when it ends. Bind: the prop has no eid yet, released when it
// binds. Read: a read the fit needs did not come, released when the container changes. A Bind or Read row that is not
// released by untilMs is refused as unread.
enum class WaitKind : uint8_t { Hold, Bind, Read };

template <class Ref>
struct WaitRow {
    Ref prop;
    Ref container;            // the container's actor, for its end of play
    uint32_t containerEid = 0;
    WaitKind kind = WaitKind::Hold;
    uint64_t untilMs = 0;     // Hold rows have none
    bool ready = false;       // a Bind or Read row whose edge came: judged again at the next tick
};

template <class Ref>
class WaitTable {
public:
    using Row = WaitRow<Ref>;
    enum class Added : uint8_t { New, Merged, NewDroppedOldest };

    // A put already waiting for the pair is kept as it is, except that a Hold row stays a Hold row.
    Added Add(void* prop, void* container, uint32_t containerEid, WaitKind kind, uint64_t untilMs) {
        for (Row& r : rows_) {
            if (r.containerEid != containerEid || r.prop.Raw() != prop) continue;
            if (r.kind != WaitKind::Hold && kind == WaitKind::Hold) {
                r.kind = WaitKind::Hold;
                r.untilMs = 0;
                r.ready = false;
            }
            return Added::Merged;
        }
        Added a = Added::New;
        if (rows_.size() >= kTableRows) {
            rows_.erase(rows_.begin());
            a = Added::NewDroppedOldest;
        }
        rows_.emplace_back();
        Row& r = rows_.back();
        r.prop.Set(prop);
        r.container.Set(container);
        r.containerEid = containerEid;
        r.kind = kind;
        r.untilMs = untilMs;
        return a;
    }

    // Puts back a row that was taken and still waits (no cap: it just left a place).
    void Readd(Row&& r) { rows_.push_back(std::move(r)); }

    // The Hold rows of a container whose hold ended.
    std::vector<Row> TakeHold(uint32_t containerEid) {
        return TakeIf([containerEid](const Row& r) { return r.kind == WaitKind::Hold && r.containerEid == containerEid; });
    }
    // The rows of a container that ended its play, of any kind.
    std::vector<Row> TakeContainerActor(void* container) {
        return TakeIf([container](const Row& r) { return r.container.Raw() == container; });
    }
    // Rows whose edge came: a Read row's container changed, a Bind row's prop bound. They are judged at the next tick.
    void MarkChanged(uint32_t containerEid) {
        for (Row& r : rows_) r.ready = r.ready || (r.kind == WaitKind::Read && r.containerEid == containerEid);
    }
    void MarkBound(void* prop) {
        for (Row& r : rows_) r.ready = r.ready || (r.kind == WaitKind::Bind && r.prop.Raw() == prop);
    }
    std::vector<Row> TakeReady() { return TakeIf([](const Row& r) { return r.ready; }); }
    // Rows past their time that no edge released.
    std::vector<Row> TakeExpired(uint64_t nowMs) {
        return TakeIf([nowMs](const Row& r) { return r.kind != WaitKind::Hold && !r.ready && r.untilMs <= nowMs; });
    }

    bool HasEdgeRows() const {
        for (const Row& r : rows_)
            if (r.kind != WaitKind::Hold) return true;
        return false;
    }
    size_t Size() const { return rows_.size(); }
    bool Empty() const { return rows_.empty(); }
    void Clear() { rows_.clear(); }

private:
    template <class Pred>
    std::vector<Row> TakeIf(Pred pred) {
        std::vector<Row> out;
        for (size_t i = 0; i < rows_.size();) {
            if (!pred(rows_[i])) {
                ++i;
                continue;
            }
            out.push_back(std::move(rows_[i]));
            rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        return out;
    }
    std::vector<Row> rows_;
};

// ---- the refusal latch ----

// A refusal that stands for as long as the drive does: the key is the prop, the container and the driver's drive key (its
// slot and the generation of its row or hold, so a new drive is a new key). It is cleared by any change of the container's
// records or arming (its dirty mark) and by a hold's end. Reach and unread refusals are not kept: they depend on where the
// prop is and on a read.
template <class Ref>
class LatchTable {
public:
    struct Row {
        Ref prop;
        uint32_t containerEid = 0;
        uint32_t driveKey = 0;
        Tally tally = Tally::kCount;
    };

    bool Hit(void* prop, uint32_t containerEid, uint32_t driveKey, Tally& tally) const {
        for (const Row& r : rows_) {
            if (r.containerEid != containerEid || r.driveKey != driveKey || !r.prop.Is(prop)) continue;
            tally = r.tally;
            return true;
        }
        return false;
    }

    // True when a live row was evicted for room. A refusal of a kind that is not kept adds nothing.
    bool Add(void* prop, uint32_t containerEid, uint32_t driveKey, Tally tally) {
        if (tally == Tally::PutOverlapReach || tally == Tally::PutOverlapUnread) return false;
        for (size_t i = 0; i < rows_.size();) {   // an older row of the pair, or of a prop that is gone, goes
            if ((rows_[i].containerEid == containerEid && rows_[i].prop.Is(prop)) || !rows_[i].prop.Alive())
                rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(i));
            else
                ++i;
        }
        bool evicted = false;
        if (rows_.size() >= kTableRows) {
            rows_.erase(rows_.begin());
            evicted = true;
        }
        rows_.emplace_back();
        Row& r = rows_.back();
        r.prop.Set(prop);
        r.containerEid = containerEid;
        r.driveKey = driveKey;
        r.tally = tally;
        return evicted;
    }

    void ClearContainer(uint32_t containerEid) {
        for (size_t i = 0; i < rows_.size();) {
            if (rows_[i].containerEid == containerEid) rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(i));
            else ++i;
        }
    }
    size_t Size() const { return rows_.size(); }
    void Clear() { rows_.clear(); }

private:
    std::vector<Row> rows_;
};

// The driver's drive key: its slot in the top byte, the generation of its client row in the low byte and of its hold above.
inline uint32_t DriveKey(uint8_t slot, uint8_t rowGen, uint16_t holdGen) {
    return (static_cast<uint32_t>(slot) << 24) | (static_cast<uint32_t>(holdGen) << 8) | rowGen;
}

}  // namespace coop::props::container_act_host::overlap::pure
