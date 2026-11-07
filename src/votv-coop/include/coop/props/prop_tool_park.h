// coop/props/prop_tool_park.h -- the states of the prop-properties tool that wait for a held copy, one per element.
//
// A received PropPropsState for a copy this player holds or a drive moves cannot be replayed at once (the setter's init()
// switches physics and collision under a grab). It waits here until the copy is let go. The newest state of an element
// wins: a state that reaches a copy directly, or a resync from a refusal, supersedes the older one parked for it, so a
// replay never puts an older press over a newer one. Game thread.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <unordered_map>

namespace coop::prop_tool_park {

// What the ledger asks of the engine side: the live prop copy of an element (null when it is none), whether that copy is
// held or driven, and the replay of one state on it.
struct Env {
    void* (*copy)(uint32_t eid);
    bool  (*held)(void* actor);
    void  (*apply)(const coop::net::PropPropsStatePayload& st, void* actor);
};

class Ledger {
public:
    // A state of `st.eid` waits for the copy's release, replacing any state parked for it before.
    void Park(const coop::net::PropPropsStatePayload& st, uint64_t nowMs);

    // A newer state reached the copy of `eid` directly: the one parked for it is older and is never replayed.
    void Supersede(uint32_t eid);

    // Replays each parked state whose copy is let go and forgets the ones whose copy is gone. A copy still held after
    // kWarnMs is said once; its state is kept, for the hold's end is where it is applied.
    void Retry(uint64_t nowMs, const Env& env);

    bool Has(uint32_t eid) const { return m_parked.find(eid) != m_parked.end(); }
    bool Empty() const { return m_parked.empty(); }
    void Clear() { m_parked.clear(); }

    static constexpr uint64_t kWarnMs = 10000;

private:
    struct Entry {
        coop::net::PropPropsStatePayload state;
        uint64_t since;
        bool said;
    };
    std::unordered_map<uint32_t, Entry> m_parked;
};

}  // namespace coop::prop_tool_park
