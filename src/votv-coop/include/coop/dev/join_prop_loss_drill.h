// coop/dev/join_prop_loss_drill.h -- [dev] the join prop-loss drill: proves a joining client's own physics, and the join's
// snapshot bracket, cannot cost the host a prop. Inert unless the join_prop_loss_drill row is set; the arm is its value.
//   break / fire -- the client calls a resting mirror's impactDamage / fireDamage; its copy and the host's can stand.
//   swing        -- the client axes a mirror of a host can; its copy breaks as the elected syncer and the host's can goes by
//                   that destroy.
//   heldbreak    -- the client holds a host can and calls its impactDamage; a prop it holds is its own to break.
//   rekey        -- the client destroys one of two cans 13.7 cm apart before the bracket; each key keeps its own actor.
//   twin         -- the client replaces its copy of a can with a can of its own before the bracket; the key follows it.
//   stack        -- as rekey with three cans, the middle one destroyed; the outer two keep their keys.
//   late         -- the host places a can after the client's bracket closed; it shows with no spawn waiting.
//   throwat      -- a measure: the client throws a can at a mirror; the host prints whether its can lives.
// The peers sequence each other with drill_chat lines; nothing crosses by file. Lines are tagged [JPL-DRILL]; each
// peer prints `<role> <arm> PASS|FAIL|INCONCLUSIVE` and its last line is `<role> DONE`.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::join_prop_loss_drill {

// Game thread, once per pump tick, after the world is up; a latched enum read when off.
void Tick(coop::net::Session* session);

// Game thread, every pump tick including the world load; a latched enum read when off. The bracket arms' client half
// runs here, since their destroy must come before the join's snapshot bracket.
void TickLoad(coop::net::Session* session);

// The cans, the pair and the steps belong to one session.
void OnDisconnect();

}  // namespace coop::dev::join_prop_loss_drill
