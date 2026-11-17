// coop/props/prop_spawn_authoring.h -- did a PLAYER author this prop birth, or did the game?
//
// A client's world mints keyed props constantly with nobody asking: spawners, a dirthole, a
// lightning strike, an impact, lib_C::replaceProp. Each resolves the same catalog and finishes the
// same GameplayStatics spawn the sandbox spawn menu does, and none may cross -- every peer's game
// makes its own, so an intent per birth would double the world. So the drop-intent lane cannot
// admit by CLASS; the discriminator is WHO ASKED. Background:
// docs/coop-entity-expression-map.md, "Who asked, for a client's fresh birth".

// The two player spawn verbs, read from bytecode on the shipped pak. The MENU: a catalog click
// runs ui_spawnmenu_C::spawn, which calls gamemode->spawnPropThroughGamemode -- a verb replaceProp
// and the impact component call too, so it is identified by its CALLER, through a script-body gate
// watch. The TOOLGUN: tool_spawn_C::init enters ExecuteUbergraph_tool_spawn, which finishes its
// own spawns and holds no other FinishSpawningActor, so the native seam's caller frame names it.

// Gameplay layer (principle 7): reaches the engine only through ue_wrap. Game thread only -- the
// gate and the native seam both hand their callbacks the VM's live frame.

#pragma once

namespace coop::prop_spawn_authoring {

// Install the script-body gate watch on the gamemode's spawn verb. A no-op on the host, which
// never asks the question: its own births are the shared world whoever asked for them. Idempotent
// and retry-throttled while mainGamemode_C is unresolved (it loads on gameplay entry). Safe to
// call every subsystem-install tick. Game thread.
void Install(bool isClient);

// Is the birth the caller is looking at RIGHT NOW authored by a player's own spawn verb? Call it
// from inside a FinishSpawningActor post callback and nowhere else: it reads that callback's
// calling frame (ue_wrap::ufunction_hook::CurrentCallerFrame) and this thread's open menu
// bracket, both of which mean nothing outside one. False when neither verb is on the stack, which
// is every ambient spawn the world makes for itself. Game thread.
// `born` is the actor that callback is looking at: while an ATV's unscrew of its battery runs (below), the battery prop it
// spawns is recorded and still reported as no player's birth. Game thread.
bool BirthIsPlayerAuthored(void* born);

// An ATV's unscrewPanel is starting on this client (`unscrewFn`, the watched body), and whether it is the battery's
// branch (no tire looked at, a battery fitted). That branch spawns a battery prop on this client's game alone, which the
// host's own spawn replaces, so it is recorded as it is born (BirthIsPlayerAuthored) and never crosses. A tire's branch
// spawns a wheel, which is not recorded. A record a body without a POST left is destroyed here, with no destroy sent. Called from
// coop/interactables/atv_gate's pre callback. Game thread.
void NoteAtvUnscrewBranch(void* unscrewFn, bool batteryBranch);

// The unscrewPanel body ended: the battery prop it spawned on this client (null when it spawned none or it is gone), and the
// record and the branch cleared. Called from coop/interactables/atv_gate's post callback. Game thread.
void* TakeAtvUnscrewBattery();

// Session teardown: a battery prop still recorded is destroyed (no destroy sent); report both columns of the caller tally -- the menu's calls, which crossed as
// intents, and every other caller's, which stayed local -- and zero it. The watch and the resolved
// UFunctions stay: they name the game's own classes, not this session's world. Game thread.
void Reset();

}  // namespace coop::prop_spawn_authoring
