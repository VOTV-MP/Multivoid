// coop/props/prop_tool_park.cpp -- see coop/props/prop_tool_park.h.

#include "coop/props/prop_tool_park.h"

#include "ue_wrap/core/log.h"

namespace coop::prop_tool_park {

void Ledger::Park(const coop::net::PropPropsStatePayload& st, uint64_t nowMs) {
    m_parked[st.eid] = Entry{st, nowMs, false};
}

void Ledger::Supersede(uint32_t eid) { m_parked.erase(eid); }

void Ledger::Retry(uint64_t nowMs, const Env& env) {
    if (m_parked.empty()) return;
    for (auto it = m_parked.begin(); it != m_parked.end();) {
        void* const actor = env.copy(it->first);
        if (!actor) {
            it = m_parked.erase(it);
        } else if (!env.held(actor)) {
            env.apply(it->second.state, actor);
            it = m_parked.erase(it);
        } else {
            if (!it->second.said && nowMs - it->second.since > kWarnMs) {
                it->second.said = true;
                UE_LOGW("prop_tool_park: element %u is still held or driven after %llu ms; its flags are applied when the "
                        "hold ends", it->first, static_cast<unsigned long long>(kWarnMs));
            }
            ++it;
        }
    }
}

}  // namespace coop::prop_tool_park
