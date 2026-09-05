# Will my hook fire? The dispatch map

The answer to "will our hook fire for function X?", kept in one place because the fact used to be
scattered across comments and cost a three-iteration rework. Read it before adding any observer,
interceptor or poll. Companion: [COOP_ENTITY_EXPRESSION_MAP.md](COOP_ENTITY_EXPRESSION_MAP.md).
Evidence tags: `[V]` verified from code or a run, `[RD]` from a comment or a reverse-engineering
record, `[?]` needs a probe.

## The one rule

The mod's detour sits on `UObject::ProcessEvent` and nowhere else (`ue_wrap/core/pe_detour`). A
call is visible if and only if the engine dispatches it through `ProcessEvent`. A call the
Blueprint VM routes through `CallFunction` and `ProcessInternal` bypasses the hook: `ProcessEvent`
itself calls `ProcessInternal` one layer below, so anything that enters there is beneath the
detour. `[RD]`

Visibility is a property of the dispatch path, not of the function. The same function
(`K2_DestroyActor`) is visible when the engine dispatches it and invisible when Blueprint code calls
it on itself.

## Visible and invisible

**Visible**, reaching `ProcessEvent`: native engine code entering a function. Input action events,
the engine lifecycle of dispatched actors (`ReceiveBeginPlay`, `ReceiveTick`), RPC-style and
native-event entry points, the `GameplayStatics` calls when a native, engine or spawner caller
issues them, multicast delegate broadcasts (a component hit, a widget click), an engine-initiated
destroy, and the mod's own reflected calls, which re-enter the detour behind a pump guard. `[V]`

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
| the native function seam (`ue_wrap/core/ufunction_hook`) | after a call whose target is native, on every route | `EX_CallMath` and final calls into natives, and dispatched events; the caller's object and the result | a script function called locally |
| the bytecode seam (`ue_wrap/core/vm_dispatch`) | at a virtual call site in the VM, observe only | every `EX_LocalVirtualFunction` dispatch, calls from inside a graph included | the arguments; it cannot cancel |
| polling | a throttled tick | the observable result of anything | the moment of the verb |

**The thread rule.** An observer or interceptor can fire on a parallel animation worker. A
callback that dereferences an actor or calls an engine function either posts itself to the game
thread and re-validates liveness there, or reads only its parameters. `[V]`

**The pump rule.** A posted task runs at top-level game-thread context, and the pump defers while
the world refuses `SpawnActor`: the detour also fires on dispatches nested inside another actor's
construction script, where `SpawnActor` silently returns null in a shipping build, so a task may
legally spawn but may run milliseconds after it was posted. Never spawn from an observer directly;
post it. `[V]`

## The function table

| Edge | Dispatch | Visible? | Caught with |
|---|---|---|---|
| the player's use input (E) | native input | yes, pre and post | the door, desk and pile lanes read what the press is about to act on `[V]` |
| the player's drop input (R) | native input | yes | not hooked; the pickup and drop are seam-driven below `[V]` |
| the ATV's seven component-hit delegates | delegate broadcast | yes, and interceptable | the hit guard zeroes a non-owner's impulse `[V]` |
| the inventory widget's buttons | widget delegate | yes | `[V]` |
| the hotbar switch (`updateHold`) | Blueprint-internal | no, but one synchronous call does the destroy, the spawn and the name | poll the holding actor `[V]` |
| the pile's grab and re-pile verbs | `EX_LocalVirtualFunction` | no | the use input's pre observer reads the aimed pile while it is alive `[V]` |
| the new-day weather rolls (red sky, black fog, rolling fog) | `EX_LocalVirtualFunction` | no | a field poll on the host; a birth catch at the finish-spawning seam on clients `[V]` |
| the impact damage entries | native impact system into a Blueprint event | yes, and interceptable | cancelled on any body that is not the local player `[V]` |
| the lethal chain (damage, kill, ragdoll, fallen) | `EX_LocalVirtualFunction` | no | the death lane cuts at the native level open below it `[V]` |
| the level travel (`loadLevel`, `transition`) | `EX_LocalVirtualFunction` | no, to both the detour and the native seam | the bytecode seam sees it `[V]` |
| `UGameplayStatics::OpenLevel` | a final call into a native | not to the detour; yes to a plain function detour | the death lane's veto `[V]` |
| an engine-initiated destroy of a tracked actor | engine | yes | the creature and world-actor pre observers `[V]` |
| any Blueprint destroy (a pickup, a morph) | `EX_CallMath` or a final call into the native | not to the detour; yes to the native seam | the prop destroy seam `[V]` |
| a finish-spawning from a graph (a container extract, a drop, a place) | `EX_CallMath` | not to the detour; yes to the native seam | the host spawn watcher with a one-tick drain `[V]` |
| a script function called locally (a container take) | inline in the VM | no, to both | the bytecode seam at the call site; the effect polled or reconciled `[V]` |
| every credit and debit of points | `EX_LocalVirtualFunction` at all nineteen sites | no, to both | the economy is host-authored: the balance is polled, intents name artifacts `[V]` |
| a deferred-spawned actor's own initialisation | `EX_LocalVirtualFunction` from its construction script | no; its post observer never fires | the finish-spawning post (keyed) or the deferred-spawn post (keyless) `[V]` |
| a deferred spawn from a native or spawner caller | reaches the detour | yes | the creature and world-actor interceptors; the actor is not positioned yet, so read the transform parameter `[V]` |
| a deferred spawn from a Blueprint graph (the pile morph, the wisp swarm, the pyramid's spawner) | `EX_CallMath` | not to the detour | the native seam, gated by the calling object's class `[V]` |
| an event actor's self-destroy at its end | a self-call | no | the host's pose walk retires the dead actor `[V]` |
| a finish-spawning from a native caller | reaches the detour | yes | the keyed sandbox-spawn seam `[V]` |
| begin-play of a save-loaded actor | no dispatch the session sees | caught by the object scan at world start `[V]` |
| begin-play of a runtime-spawned actor | maybe | unverified | probe before relying on it `[?]` |
| a cosmetic emitter spawn | `EX_CallMath` | no | poll the result; the cue lane diffs the particle components `[V]` |
| the save write | native C++ | not to the detour | the native detour that blocks a client's world save `[V]` |
| the pause (menu and console paths) | `EX_CallMath`, and the console bypasses the statics entirely | no, on two paths | enforce the state every tick `[V]` |
| the kerfur's conversion verbs | `EX_LocalVirtualFunction` self-calls, with `EX_CallMath` spawns inside | the verb no; its inner spawns yes, to the native seam | the bytecode seam brackets the verb, the native seam captures the successor `[V]` |
| the scheduler's event fire | a cross-object virtual call | no, to both | poll the save's passed-events list `[V]` |
| every screen and panel verb | `EX_LocalVirtualFunction` | no | poll the state field `[V]` |
| the desk keyboard's key router | widget input | yes, on the occupant's machine only | the desk input lane `[V]` |
| the desk ping | not a verb: a latent tick machine gated on a flag | | never write the flag into a mirror `[V]` |
| the laptop's interaction verbs | `EX_LocalVirtualFunction` | no | poll the power flag; the host authors the content `[V]` |
| the base alarm's trigger | a virtual call after a key lookup | no | poll the active flag on both peers `[V]` |
| the timer, delay and tick-interval drivers | `EX_CallMath` | not to the interceptor | park the instance's tick, or cancel the spawner's entry function `[V]` |
| the container contents verbs | `EX_LocalVirtualFunction` | no | the host authors the contents as state; the bytecode seam marks dirty `[V]` |
| the desk's audio components' play and activate | virtual calls on native targets | yes, to the native seam | the effect forward `[V]` |
| deck playback | stubs into the graph; the sound component's activate and deactivate | yes, to the native seam | the play and stop edges `[V]` |
| the drive-chain, database and module verbs | `EX_LocalVirtualFunction` | no | the bytecode seam brackets, then a poll `[V]` |

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
   - a script function called locally from Blueprint: neither the detour nor the native seam fires;
     hook a visible caller upstream, a native callee downstream with a source filter, or observe
     the function's effect (the join's pose gate observes the loader's end by quiescence);
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

The bytecode seam publishes the innermost matched verb for the calling thread, so a consumer's own
native-seam hooks firing inside a verb body can attribute a spawn or destroy to it. It is a
project-wide namespace, and two readings of it are wrong: the "active" flag alone means any
registered verb on this thread, and the verb id is a caller-chosen tag unique only within its own
consumer. The verb name is the identity. A consumer reading the bracket handed to its own callback
is already scoped. And a context gate belongs in a hot ambient callback while a context resolve
does not: a class resolve on a miss walks the whole object array on every click. `[V]`

## Two traps a smoke can fall into

A render-blind smoke proves dispatch and no crash; it cannot prove an effect landed on the GPU. A
static-mobility component silently ignores a mesh swap and a move while the call still returns
true, so the trash proxies were invisible through a whole smoke that passed. Confirm anything
visual by hand. `[V]`

A smoke that drives an entity through a non-representative slot can pass falsely: grabbing a pile
by calling the grab function puts the clump in the physics-handle slot, where the native re-pile
gate aborts, while a real press carries it in the hand slot, where the gate never fires and the
held clump re-piles on contact. An interaction smoke drives the entity through the seam the player
uses. `[V]`

## Needs a probe

- Whether begin-play of a normal runtime-spawned actor reaches `ProcessEvent`. `[?]`
