// coop/dev/desk_panel_drill.h -- [dev] the desk's back panels, driven through the desk's own verbs on
// one peer and judged against the other's desk.
//
// Arms (`desk_panel_drill`): host and client -- the acting peer unbolts the first panel the desk
// still carries (unscrewPanel, the wrench's last step; skipped for a panel the save holds unbolted),
// takes it off (the E press, which hands it the panel prop), snaps the prop back into its lid so the
// desk's overlap puts it on, and bolts it again. Each peer
// says its desk's two masks and its count of live panel props in the lobby chat whenever they
// change, and after every step the acting peer waits for both peers to hold the state the step
// leaves. race -- the host unbolts, then both peers press on one instant counted from the go line,
// and each judges that the panel is off and one panel prop is left on both. Every verdict is one
// line, `[DESK-PANEL-DRILL]` with the peer's tag and `PASS` or `FAIL`.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::dev::desk_panel_drill {

bool IsEnabled();

// The drill's pass on either peer. A single read when off. Game thread.
void Tick(coop::net::Session* session);

// Back to the first step. Net disconnect.
void OnDisconnect();

}  // namespace coop::dev::desk_panel_drill
