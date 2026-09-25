# Will my hook fire? The dispatch map

The answer to "will our hook fire for function X?", kept in one place because the fact used to be
scattered across comments and cost a three-iteration rework. Read it before adding any observer,
interceptor or poll. Companion: [coop-entity-expression-map.md](coop-entity-expression-map.md).
Evidence tags: `[V]` verified from code or a run, `[RD]` from a comment or a reverse-engineering
record, `[?]` needs a probe.

## The one rule

The mod's `ProcessEvent` detour (`ue_wrap/core/pe_detour`) sees a call if and only if the engine
dispatches it through `ProcessEvent`. A call the Blueprint VM routes through `CallFunction` and
`ProcessInternal` bypasses that hook: `ProcessEvent` itself calls `ProcessInternal` one layer
below, so anything that enters there is beneath the detour. `[RD]` The second detour sits one
layer lower still, on the VM's script loop, the function every one of those routes ends in
(`ue_wrap/core/script_gate`): it sees every Blueprint function body with the frame built, and
can refuse to run it, measured on four routes: an engine dispatch through `ProcessEvent`, a
Blueprint's local virtual call on itself, the same call through a context switch, and an
ubergraph entry through a local final call `[V]`; a virtual or final call to a script function
through `CallFunction` ends in the same loop by construction `[RD]`. The table below keeps the
`ProcessEvent` reading of "visible", since it is the one the lanes were built on.

Visibility is a property of the dispatch path, not of the function. The same function
(`K2_DestroyActor`) is visible when the engine dispatches it and invisible when Blueprint code calls
it on itself.

## Visible and invisible

**Visible**, reaching `ProcessEvent`: native engine code entering a function. Input action events,
the engine lifecycle of dispatched actors (`ReceiveBeginPlay`, `ReceiveTick`), RPC-style and
native-event entry points, the `GameplayStatics` calls when a native, engine or spawner caller
issues them, multicast delegate broadcasts (a component hit, a widget click), an engine-initiated
destroy, and the mod's own reflected calls, which re-enter the detour nested in the dispatch that
made them, where the pump does not drain. `[V]`

**Invisible**, routed through the Blueprint VM below the hook: the `EX_LocalVirtualFunction`,
`EX_VirtualFunction`, `EX_FinalFunction`, `EX_LocalFinalFunction` and `EX_CallMath` opcodes, so
every Blueprint-to-Blueprint call, a Blueprint self-destroy, and native C++ internal calls. `[RD]`

The same `GameplayStatics` function is in both lists. `BeginDeferredActorSpawnFromClass` reaches
`ProcessEvent` from a spawner's native caller and is `EX_CallMath` from a Blueprint graph; the pile
grab's clump spawn logged zero fires on an observer that fires for every pinecone. "Catches it
for the spawner, so catches it for the clump" is a category error. `[V]`

## The seams

Per dispatch, in order: drain the task pump, then the interceptors (before the call; a `true`
return skips it), then the pre observers, then the engine's function through the trampoline, then
the post observers (`ue_wrap/core/game_thread.h` declares them). `[V]`

| Seam | Fires | Sees | Blind to |
|---|---|---|---|
| a post observer | after the original; every registered pair fires | state the Blueprint just wrote; a spawned actor to bind | every invisible call |
| a pre observer | before the original | state about to be cleared or destroyed | the same |
| an interceptor | before the original; `true` cancels | a dispatch to suppress or replace | a Blueprint-internal call |
| the detour itself | the anchor of the three above | a transparent bypass darkens the whole layer | |
| a native detour (`ue_wrap/core/hook`) | on a raw native address | non-function natives: the save write, the swap chain's present, the level open | anything a function observer would want |
| the native function seam (`ue_wrap/core/ufunction_hook`) | after a call whose target is native, on every route | `EX_CallMath`, final and virtual calls into natives, and dispatched events; the caller's object, the calling frame's function and locals, and the result, which the callback may overwrite | a script function called locally |
| the script-body gate (`ue_wrap/core/script_gate`) | at the entry of a Blueprint function's body, on every route; a pre callback answers run or cancel, a post callback reads the result | the instance, the evaluated parameters, the caller's frame; a `ProcessEvent` call, an `EX_Local*` call, a call through `EX_Context` and an ubergraph entry alike | a native function; a watched body on a worker thread is counted and passed through |
| polling | a throttled tick | the observable result of anything | the moment of the verb |

**The thread rule.** An observer or interceptor can fire on a parallel animation worker. A
callback that dereferences an actor or calls an engine function either posts itself to the game
thread and re-validates liveness there, or reads only its parameters. `[V]`

**The pump rule.** A posted task runs at the next outermost dispatch on the game thread -- one
nested in no other, so never in the middle of a Blueprint body or inside an observer, seam or gate
callback -- and the pump defers while the world refuses `SpawnActor`, which silently returns null
in a shipping build: an actor's construction, which can itself be an outermost dispatch, and the
world's teardown. So a task may legally spawn, but it runs only once the body that was running when
it was posted has returned: a frame later in play, seconds later in a world load, whose tail is one
body (5.6 to 6.8 s with no drain on a joining client). A queue that a callback fills and
a tick judges must hold everything such a body produces, and a window a callback opens for a tick to
close counts session ticks (`TickSerial` in `coop/session/net_pump`), not wall time, since such a
body costs one tick. Every task posted while a drain runs, from any thread, runs in that drain.
Never spawn from an observer directly; post it. `[V]`

## The function table

| Edge | Dispatch | Visible? | Caught with |
|---|---|---|---|
| the player's use input (E) | native input | yes, pre and post | the desk and pile lanes read what the press is about to act on `[V]` |
| a light group's `runTrigger` (`trigger_lightRoot_C`: 0 the toggle its breaker gates, 1 on, 2 off), the route of every live writer of its `isActive` -- a switch's use, powerControl, the gamemode, an eventer, a keyhole, a generator | Blueprint calls | no, to the detour | the script-body gate watches it BY NAME: after the body on the host, the group lane sends the state the call left; before the body on a client, every call on a group the lane indexes is refused but the lane's own apply to that group `[RD]` |
| the player's drop input (R) | native input | yes | not hooked; the pickup and drop are seam-driven below `[V]` |
| the ATV's seven component-hit delegates | delegate broadcast | yes, and interceptable | the hit guard zeroes a non-owner's impulse `[V]` |
| the inventory widget's buttons | widget delegate | yes | `[V]` |
| the hotbar switch (`updateHold`) | Blueprint-internal | no, but one synchronous call does the destroy, the spawn and the name | poll the holding actor `[V]` |
| the quick-slot bar's rebuild (`updateSlotInv`) | Blueprint-internal, from ten sites -- the game mode, the player and the inventory screen | no, to the detour | the script-body gate watches it BY NAME, which is the only seam that fires inside a world load, where the pump does not drain and no tick of ours gets a sample; the mod's own re-issue of it is a reflected call and so reaches the detour like any other `[V]` |
| the pile's grab and re-pile verbs | `EX_LocalVirtualFunction`, the grab one through an interface context | no, to the detour | the script-body gate watches `toClump` and `playerGrabbed` and refuses each per call on a client, which covers every caller of them -- the collision component's overlap handler and the arir follower alike; the use input's pre observer still reads the aimed pile while it is alive `[V]` |
| a door's entry verbs (`actionOptionIndex` the press, `addDamage` a hit, `door_pryable_C::crowbarOpen` a pry) | Blueprint calls from the player's graph (the use press, the melee trace) and the crowbar's | no, to the detour | the script-body gate watches them BY NAME and refuses each per call on a client for a door the door lane indexes, sending it to the host, which runs the same verb on its own copy; a hit whose `actor` is not the local player is refused without a send. `[V]` for a press and a hit dispatched the way the door drill dispatches them; a player's own E and swing, and a pry, `[RD]` |
| a door's state verbs (`doorOpen`, `doorClose`), the one route of every writer of its open state -- the door's own graph (the press, the pry, the autoclose, a jam, a trigger) and other Blueprints (a keypad, a creature) | Blueprint calls | no, to the detour | the script-body gate watches both BY NAME: after the body on the host, the door lane sends the state the verb left; before the body on a client, every call on a door the lane indexes is refused but the lane's own apply to that door. `[V]` for the host's sends and a client's own autoclose close, in the door drill; a creature's or a trigger's call on a client `[RD]` |
| a door's sensor events (`BndEvt__door_sensor_ComponentBoundEvent_2_...` the begin, `..._3_...` the end, which build the list the autoclose counts) | the sensor's overlap delegate, from the engine | the stubs, no: 0 calls on any door over a drill run whose lists changed, both name watches live; the handlers, yes, as `ExecuteUbergraph_door` at the stubs' entry points (7956 the begin, 8242 the end), with the event's arguments in the ubergraph's frame | read there by the door drill, a probe; no lane stands on them. The door's own `coll`, `door_L` and `door_R` begin and end on its sensor as the door moves; the begin handler adds only a prop or a pawn, so they never enter the list. `[V]` on a client |
| the broom's stroke, the swing montage's notify | a delegate broadcast | yes | the script-body gate refuses it per call on a client, which sends what the stroke read of its holder, and the host runs the notify on its mirror of that client's broom `[V]` |
| `mainPlayer_C::arm`, the reach a held tool aims with | `EX_LocalVirtualFunction` through a context switch, from the tool's graph | no, to the detour | while the host runs a client's stroke, the gate's pre writes the client's segment into the out parameters and cancels the body, matched to the holder and the broom that called it; every other call runs `[V]` |
| the push's reads of its holder (`GetActorForwardVector`, `GetVelocity`) | `EX_FinalFunction` and `EX_VirtualFunction` into natives, through a context switch | not to the detour; yes to the native seam | while the host runs a client's stroke, seams armed for that call alone write the client's heading and velocity into the results, matched to the holder and the broom whose bytecode reads them `[V]` |
| the dispenser pile's broom verb (`broomed`) | `EX_LocalVirtualFunction` from the broom's own ubergraph | invisible to a ProcessEvent hook | a client never reaches it, its stroke being refused above; the host's gate reads the pile's key at its entry so the depletion watch can believe a death far from the host's camera, and arms the native seam on `FinishSpawningActor` for that pile's own bytecode until the body returns, so each prop the pile finishes there is trash the stroke knocked out of it `[V]` |
| the broom's own chip-pile morph (a copy of the pile's morph inside the broom's ubergraph) | none: a `BeginDeferredActorSpawnFromClass` / `FinishSpawningActor` / `K2_DestroyActor` sequence in the broom's graph, no verb of its own | not to the detour; yes to the native seam, with the broom as the source | on the host the deferred-spawn seam reads the pile the stroke loop is on out of the broom's frame and moves its id onto the clump at birth; a client never runs it, its stroke being refused `[V]` |
| a Blueprint's physics velocity set (`SetPhysicsLinearVelocity`) | `EX_VirtualFunction` into a native, through a context switch | not to the detour; yes to the native seam | a broom's push, told from every other caller by the object whose bytecode made the call: a prop coasts on the driven-prop channel and a clump rolls on the clump stream `[V]` |
| the weather-event rolls (red sky, black fog, rolling fog) | `EX_LocalVirtualFunction` | no | a field poll on the host; a birth catch at the finish-spawning seam on clients `[V]` |
| the impact damage entries | native impact system into a Blueprint event | yes, and interceptable | cancelled on any body that is not the local player `[V]` |
| the player's damage verb (`Add Player Damage`) | `EX_LocalVirtualFunction` on itself, and the same opcode through a context switch from each attacker | no, on every one of its call sites | the script-body gate, refusing per call by the verb's own `source` argument -- the attacker the Blueprint passes `[V]` |
| a base cleaner's begin-play (it box-overlaps and destroys) | engine | yes | a client cancels it; the verb is declared on the base class, so a leaf variant that declares nothing resolves only by climbing `[V]` |
| the lethal chain (damage, kill, ragdoll, fallen) | `EX_LocalVirtualFunction` | no | the death lane cuts at the native level open below it `[V]` |
| the level travel (`loadLevel`, `transition`) | `EX_LocalVirtualFunction` | no, to both the detour and the native seam | the script-body gate watches `lib_C::loadLevel` BY NAME and cancels per call, judging the author the call still carries -- the only place it is still there (`coop/player/run_end_travel`) `[V]` |
| `UGameplayStatics::OpenLevel` | a final call into a native | not to the detour; yes to a plain function detour | nothing of ours sits here any more: the author is a parameter of `loadLevel` and is gone by this hop, so the veto moved UP to the gate and this detour was deleted with its AOB `[V]` |
| an engine-initiated destroy of a tracked actor | engine | yes | the creature and world-actor pre observers `[V]` |
| any Blueprint destroy (a pickup, a morph) | `EX_CallMath` or a final call into the native | not to the detour; yes to the native seam | the prop destroy seam `[V]` |
| a finish-spawning from a graph (a container extract, a drop, a place) | `EX_CallMath` | not to the detour; yes to the native seam | the host spawn watcher with a one-tick drain `[V]` |
| a script function called locally (a container take) | inline in the VM | no, to both | the script-body gate at the body's entry; the effect polled or reconciled `[RD]` |
| a disc-holding device's SLOT ENTRY -- the hitbox delegate that takes a disc nobody pressed anything for (one on the signal server, two on the laptop) | delegate broadcast | yes, and interceptable | a disc is marked in transit at its own spawn and the entry is cancelled for one; the mark has to be set at the DEFERRED spawn, since a collider reports its overlaps as it registers, inside the finish `[V]` |
| a disc-holding device's insert and eject | `EX_LocalVirtualFunction` | no | the slot is mirrored as state, and the device's LOOK is driven by us: the laptop's widget has a notify-free refresh, the signal server has none at all -- its mesh swap is inline in the two verbs, so a receiver sets the mesh itself from `lib_C::floppyFromType` `[V]` |
| a wall-attach component's stick commit | a latent resume of its ubergraph at the commit entry | yes, to a post observer on the ubergraph | the stick lane records it and broadcasts it on the next pass `[V]` |
| a wall-attach component's unstick (a grab's prelude, a crowbar's `crowbarOpen`, a fridge glow's knock) | `EX_LocalVirtualFunction` from the caller's graph | no, to the detour | the script-body gate at the unstick's body; the next pass broadcasts the prop if the body freed it `[V]` |
| every credit and debit of points | `EX_LocalVirtualFunction` at all nineteen sites | no, to both | the economy is host-authored: the balance is polled, intents name artifacts `[V]` |
| a deferred-spawned actor's own initialisation | `EX_LocalVirtualFunction` from its construction script | no; its post observer never fires | the finish-spawning post (keyed) or the deferred-spawn post (keyless) `[V]` |
| a deferred spawn from a native or spawner caller | reaches the detour | yes | the creature and world-actor interceptors; the actor is not positioned yet, so read the transform parameter `[V]` |
| a deferred spawn from a Blueprint graph (the pile morph, the wisp swarm, the pyramid's spawner) | `EX_CallMath` | not to the detour | the native seam, gated by the calling object's class `[V]` |
| an event actor's self-destroy at its end | a self-call | no | the host's pose walk retires the dead actor `[V]` |
| a finish-spawning from a native caller | reaches the detour | yes | the keyed sandbox-spawn seam `[V]` |
| the spawn menu's own `ui_spawnmenu_C::spawn`, and the gamemode verb it calls | a widget delegate into `EX_Context` on the menu's `gamemode` variable, whose inner expression is `EX_LocalVirtualFunction spawnPropThroughGamemode` (measured on the shipped pak, `ui_spawnmenu.json` expr [13]) | the widget's yes, the gamemode verb's no | the script-body gate on `spawnPropThroughGamemode`. The verb alone does not name a player -- `lib_C::replaceProp` and `comp_physicsImpact` reach it too -- so the gate's PRE reads the CALLING frame and only the menu's own `spawn` counts, and the seam under the birth asks `IsBodyActive(verb, menuSpawn)` rather than keeping a counter of its own `[V]` |
| the toolgun's spawn (`tool_spawn_C`) | `EX_CallMath` from `ExecuteUbergraph_tool_spawn`, which carries its own copy of the three catalog branches and never calls the gamemode verb | not to the detour; yes to the native seam | the caller frame the native seam already holds names the ubergraph, and it finishes nothing else `[V]` for the bytecode, `[?]` for a run |
| a hook's constraint build (`SetConstrainedComponents`, from `attach_a` or `makeAttachments`) | a final call into a native, from the graph | not to the detour; yes to the native seam, on every route | a client breaks every hook tie there; the host keeps its own and builds a client's on its mirror `[V]` |
| begin-play of a save-loaded actor | no dispatch the session sees | caught by the object scan at world start `[V]` |
| begin-play of a runtime-spawned actor | maybe | unverified | probe before relying on it `[?]` |
| a cosmetic emitter spawn | `EX_CallMath` | no | poll the result; the cue lane diffs the particle components `[V]` |
| the save write | native C++ | not to the detour | the native detour that blocks a client's world save `[V]` |
| the pause (menu and console paths) | `EX_CallMath`, and the console bypasses the statics entirely | no, on two paths | enforce the state every tick `[V]` |
| the kerfur's conversion verbs | `EX_LocalVirtualFunction` self-calls, with `EX_CallMath` spawns inside | the verb no; its inner spawns yes, to the native seam | the script-body gate brackets the verb, the native seam captures the successor `[RD]` |
| the scheduler's event fire | a cross-object virtual call | no, to both | poll the save's passed-events list `[V]` |
| every screen and panel verb | `EX_LocalVirtualFunction` | no | poll the state field `[V]` |
| the desk keyboard's key router | widget input | yes, on the occupant's machine only | the desk input lane `[V]` |
| the desk ping | not a verb: a latent tick machine gated on a flag | | never write the flag into a mirror `[V]` |
| the laptop's interaction verbs | `EX_LocalVirtualFunction` | no | poll the power flag; the host authors the content `[V]` |
| the base alarm's trigger | a virtual call after a key lookup | no | poll the active flag on both peers `[V]` |
| the timer, delay and tick-interval drivers | `EX_CallMath` | not to the interceptor | park the instance's tick, or cancel the spawner's entry function `[V]` |
| the container contents verbs | `EX_LocalVirtualFunction` | no | the peer whose verb fired authors the contents and the host arbitrates; the script-body gate marks the owning actor's eid dirty `[V]` -- both peers log `the verb watch ENTERED` once per session, and a client's slice is judged on the author's reach, its rate, and the base it edited from |
| the desk's audio components' play and activate | virtual calls on native targets | yes, to the native seam | the effect forward `[V]` |
| deck playback | stubs into the graph; the sound component's activate and deactivate | yes, to the native seam | the play and stop edges `[V]` |
| the drive-chain, database and module verbs | `EX_LocalVirtualFunction` | no | the script-body gate brackets, then a poll `[RD]` |
| the UI input-mode verbs (`SetInputMode_UIOnlyEx`, `_GameAndUIEx`, `_GameOnly`) | `EX_FinalFunction` into a UMG native -- all 46 call sites in the cook, no other route exists | not to a ProcessEvent observer; YES to the native seam | the post-hook tells the mod when a UI surface can hold focus, which is what lets `input_owner` stop sweeping every UObject once a second `[V]` |
| the widget focus verbs (`SetKeyboardFocus`, `SetUserFocus`, `SetFocus` on `UWidget`) | `EX_FinalFunction` into a native -- 20 sites in 12 classes, 9 of which announce no input mode | the same | watched beside the input-mode trio, because the mode alone is a census of this cook and not an engine guarantee. NOTE a mouse click that moves Slate focus calls NONE of them: Slate handles it internally, and only the surface's input-mode announcement covers that case `[V]` |

## How to pick a seam

1. A native-engine entry into a function (input, lifecycle, an RPC-style event, a statics call
   from a native caller)? Visible. Use a pre observer to read state about to be cleared, a post
   observer to read state just written, an interceptor to cancel or replace.
2. A Blueprint-to-Blueprint call, a Blueprint self-destroy, or a Blueprint's own initialisation?
   Invisible. Do not hook it: the observer registers fine and never fires. Instead:
   - a deferred-spawned actor: the deferred-spawn post (keyless; the actor is at the origin, read
     the parameter) or the finish-spawning post (keyed);
   - an actor already in the world at connect: the object scan at world start and the connect
     snapshot;
   - a cosmetic effect or an unobservable self-destroy: poll the result on a throttled tick, with a
     proximity or identity gate so a stream-out is not read as the event;
   - an `EX_CallMath` call whose source and product you need deterministically: the native
     function seam on the callee, which reads the calling object and the result; a `ProcessEvent`
     observer can never fire on it;
   - a script function called locally from Blueprint: neither the detour nor the native seam
     fires; the script-body gate does, with the arguments, and can refuse the body on the peer
     that must not author it; where only the effect matters, observe it (the join's pose gate
     observes the loader's end by quiescence);
   - an input that triggers a Blueprint-internal action: hook the input function's pre observer
     and read what the Blueprint is about to act on.
3. A native C++ function that is not a reflected function? A function observer can never fire.
   Use a native detour.
4. Always handle the thread context.

## Observing an input is not driving it

A reflected call of an input event function fires the observers, because it re-enters
`ProcessEvent`, but it does not run the input-gated gameplay body: the input stub is driven by the
engine's input system threading the graph to the right node on a live pressed edge, and a
synthetic call runs the stub without that path. `[V]` To observe an input, hook it. To drive the
gameplay from code, call the gameplay function the input would invoke, which routes through
`ProcessEvent` into the real body; for the pile grab that is the grab function itself, and the
clump then lands in the physics-handle slot rather than the hand slot.

## Overriding an animation variable the update recomputes

A game-thread write to an animation instance variable that the Blueprint's update event recomputes
always loses: per update, the event graph recomputes the variable, then the fast-path copies sample
class variables into node pins, then the graph evaluates. The seam is the native function seam on
the animation Blueprint's own update override, which runs after the recompute and before the
copies; filter the instances inside the callback, and verify the resolved function is declared on
the Blueprint class, since resolving a superclass's declaration would hook every animation
instance. `[V]` Two corollaries: the native-seam table's capacity is per peer, so an install must be
verified in every peer's log, not one; and a node's alpha field is not its effective weight, since
a node inside a state-machine state contributes nothing when the machine has left that state.

## The ambient verb window

The script-body gate publishes the innermost watched body for the calling thread, so a consumer's
own native-seam hooks firing inside a verb body can attribute a spawn or destroy to it. It is a
project-wide namespace, and two readings of it are wrong: the "active" flag alone means any
watched body on this thread, and the tag is a caller-chosen number unique only within its own
consumer. The verb name is the identity.

**A consumer asking "is MY verb running?" asks `IsBodyActive(function, callerFunction)`, never the
innermost window and never a counter of its own.** The window names only the innermost watched
body, so another consumer's watch firing in between hides the outer one; and a counter a consumer
raises in its pre and lowers in its post is exact only while every pre gets a post, which is not
guaranteed on two paths -- a fault the ProcessEvent firewall absorbs unwinds past the post-callback
statements, and any consumer returning Cancel skips the posts for every watch on that call. Either
leaves the counter raised for the life of the process, with the consumer believing its verb is
running forever; for the spawn-menu lane that would have meant every ambient prop birth on that
client crossing to the host as an intent. `IsBodyActive` walks the gate's own RAII scope chain
instead, which unwinds on all three paths, and can match on the CALLER too, which is what tells a
verb's player route from the same verb's ambient callers. `[V]` A consumer reading the call handed to its own callback
is already scoped. And a context gate belongs in a hot ambient callback while a context resolve
does not: a class resolve on a miss walks the whole object array on every click. `[V]`

## Six traps

A call that returns true has not necessarily done anything visible: a static-mobility component
silently ignores a mesh swap and a move while the call still returns true, which is how the trash
proxies were once invisible for a whole session. A dispatch verdict says nothing about the GPU.
`[V]`

Driving an entity through a non-representative slot gives a false result: grabbing a pile by
calling the grab function puts the clump in the physics-handle slot, where the native re-pile gate
aborts, while a real press carries it in the hand slot, where the gate never fires and the held
clump re-piles on contact. Drive the entity through the seam the player uses. `[V]`

A hook resolves the function an INSTANCE would run, which is not the function a class declares.
`reflection::FindFunction` matches the exact owner and never climbs, so naming a class that
inherits the verb returns null and the seam never installs: it logs nothing after its one warning
and reads exactly like a verb that never fires. The mirror of the same fact bites a reflected
call, because `ProcessEvent` runs the UFunction it is handed and does not re-resolve by name --
a verb resolved on a base and called on an overriding subclass runs the BASE body. Ask both
questions with `reflection::FindDispatchFunction`, which climbs and names the declaring class: if
the declarer is not the class you asked for, that one object serves the whole family, so either
cover it deliberately or filter on the instance. `[V]` The measured cost of not asking: a client
cleaner deleting props locally for a year, and the wrong material on six grime types.

A Blueprint path that the decompiled listing renders as an unreachable cycle is the VM's own
execution-flow STACK, and only the control-flow graph shows where it goes. `EX_PushExecutionFlow`
pushes a target and falls through; a later `EX_PopExecutionFlow` jumps to the most recent one, so
a body can schedule a block, run several others, and arrive there from whichever of them finishes
first. A linearising decompile has no way to draw that and emits two blocks that `goto` each other
with nothing entering either, which reads as dead code and is the hot path. `AmainPlayer_C::
LookAtFunction` is the worked example: its action-row rebuild is pushed at one block, its field
store at another, and they run in the reverse of the order they appear. Read the function's
control-flow graph, built from the bytecode offsets, before believing a listing's control flow, and
note what this costs a seam rather than a reader -- nothing, because the script gate sees a body
entered by a pop exactly as it sees one entered by a call. `[V]`

A tick park is a claim about ONE dispatch, never about the actor. The hook lane parks a mirror's
brain with a PRE interceptor that cancels the Blueprint body of `hook_C::ReceiveTick`, per actor
(`coop/items/hook_sync.h:32-38`), and that is correct for what it covers. It does not cover the
ubergraph: a mirror adopting an anchored hook runs `loadData` then `processKeys`, which tails into
`frameDelay` -> `makeAttachments()` and builds a real PhysX constraint against that peer's own copy
of whatever the hook was tied to -- no tick involved, and no `attach_a` either. So a parked mirror
can still mutate the receiving peer's world through an event the park never saw. Ask which dispatch
the park intercepts, then ask what else can reach the same write. `[V]` The write itself has a
seam: every route into a hook's constraint ends in the native `SetConstrainedComponents`, and the
native function seam on it fires for the mirror's `makeAttachments` as it does for the owner's
`attach_a`; the hook lane breaks a client's tie there (`coop/items/hook_constraint`). `[V]`

A verb you dispatch to repaint a device can write PLAYER state, and the rate you call it at is
part of its meaning. `analogDScreenTest_C::updToggles` repaints the console's toggle LEDs and ends
by nulling the local pawn's `lookAtComponent`, the last statement of its decompiled body. `[V]` A
mirror lane pulsed that verb on a 333 ms clock, and each pulse left the pawn with a null
`lookAtComponent` beside a live `lookAtActor`, so `AmainPlayer_C::LookAtFunction`'s five-field
compare failed on that one pair the next tick and every action button was destroyed and re-created
-- measured at 284 of 284 rebuilds under a held aim, at 2.906/s against a 3.00 Hz clock, on every
prop. `[RD]` The write reads as the game invalidating its own look-at cache so the tooltip of the
toggle just flipped is re-resolved, which makes the rebuild the INTENDED effect at the rate the game
produces it -- a human flipping a switch -- and a metronome on a clock; that is an intent read of
the body, not an observation. `[RD]` The lane is client-side because the host returns before the
mirror, which is a read of the code, not a measured host.
Two questions before dispatching any game verb periodically: what does its whole body write
besides the thing you want, and does the game itself ever call it on a clock? A parameterless name
like `upd*` or `refresh*` promises nothing, and a list of such verbs assembled for one write path
does not transfer to another -- here none of the nine painted any field the periodic caller wrote,
so the pulse bought nothing and cost a visible defect. Full trail:
`src/votv-coop/src/ue_wrap/desk/console_desk.cpp` (the chain, with what each verb is for).

## Needs a probe

- Whether begin-play of a normal runtime-spawned actor reaches `ProcessEvent`. `[?]`
