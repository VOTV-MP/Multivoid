// coop/session/session_gate_hold.cpp -- the session's hold on the script-body gate and the death
// seam; declared in coop/session/subsystems.h, called by the pump at a connection's start and end.

#include "coop/session/subsystems.h"

#include "coop/element/death_seam.h"
#include "coop/props/prop_key_index.h"  // Begin/End at the gate hold
#include "ue_wrap/core/script_gate.h"   // the session's hold on the script-body gate

namespace coop::subsystems {

namespace {
// Whether the session holds the script gate and the death seam: taken by HoldSessionGate, released
// by ReleaseSessionGateHold. Game thread.
bool g_sessionHoldsGate = false;
}  // namespace

void HoldSessionGate() {
    if (g_sessionHoldsGate) return;
    ue_wrap::script_gate::Acquire("the coop session");
    coop::element::death_seam::Acquire("the coop session");
    g_sessionHoldsGate = true;
    // The key index sees every key write from here on, so it seeds now (or on a later tick, once its watches
    // are live).
    coop::prop_key_index::Begin();
}

void ReleaseSessionGateHold() {
    if (!g_sessionHoldsGate) return;
    coop::prop_key_index::End();  // a write with the gate let go goes unobserved: the next hold seeds again
    ue_wrap::script_gate::Release("the coop session");
    coop::element::death_seam::Release("the coop session");
    g_sessionHoldsGate = false;
}

}  // namespace coop::subsystems
