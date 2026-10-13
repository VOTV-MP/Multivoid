// coop/dev/hostile_drill.h -- the receive boundary's drill: a hostile peer, judged by the receiver.
//
// One peer sends malformed coop packets of a chosen kind and the RECEIVING peer's mod judges whether the
// boundary refused them without stalling the session, the verdict PRINTED for the rig to read. Each arm
// proves its one attack and nothing more. The same judge reads green on the gated bytes and red on bytes
// without the gate (the commit before the fix), so a PASS is one the judge could have refused. Some arms
// (reliable_flood, repeat_world_ready, repeat_save_request) are judged by the SENDER: the host's answer is
// its session ending, with a code the sender reads.

// This is defensive testing of OUR OWN mod, in OUR OWN rig. Nothing here ships enabled: the enum row
// (hostile_drill) defaults off and gates every path -- a single read when off -- and the forged sends
// are the same shape as the dev-gated malformed sends the tree already carries (midnight_drill's NaN
// clock, drive_drill's forged row). Set on BOTH peers: the client sends, the host judges (the
// sender-judged arms above excepted); the host's gates read nothing of it. Protocol waits on readiness
// (rig_ready milestones), never a clock.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hostile_drill {

// Any arm set? A single enum read; every path below returns early when off.
bool IsEnabled();

void Install(coop::net::Session* session);
void Tick();
void OnDisconnect();

}  // namespace coop::dev::hostile_drill
