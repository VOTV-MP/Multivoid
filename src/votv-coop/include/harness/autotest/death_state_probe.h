// harness/autotest/death_state_probe.h -- one game-thread reading of everything the death chain
// touches, and the four censuses that name a source a targeted read cannot find.
//
// Extracted from autotest_death.cpp when that file reached 1088 lines: READING the death state
// and DRIVING a death are two concerns, and the readers had already grown a second consumer --
// autotest_runend.cpp re-derived the ragdoll flags, the location and the landing distance by hand.
// A third (the banana drill) needed the same. The drills own their own threading; this module
// owns the reads, and every entry point here must be called ON the game thread.

#pragma once

#include <cstdint>
#include <string>

namespace harness::autotest {

// One game-thread sample of everything the arms and the timeline read.
struct DeathSnapshot {
    bool  havePawn = false;
    bool  canRagdoll = true;
    bool  haveCanRagdoll = false;
    bool  isRagdoll = false;
    bool  dead = false;
    bool  haveState = false;
    float health = -1.f;
    bool  blackScreen = false;        // a blackScreen_C OBJECT exists
    // Whether it is on the screen, a different question: RemoveFromParent detaches, so a removed
    // widget is still findable, and "is the black screen gone" must read IsInViewport.
    bool  blackScreenInViewport = false;
    bool  inGameplay = false;   // the live UWorld is still untitled_1 (we did not travel)
    bool  haveWorld = false;
    // Add Player Damage's own early-out terms (gamemode.immortal, isDreaming, dead, startInvinc),
    // so a hit that lands nowhere can name the term.
    bool  startInvinc = false;
    bool  haveStartInvinc = false;
    bool  immortal = false;
    bool  haveImmortal = false;
    // The physics-grabbed actor. The revive's teleport drops the grabbed actor, transforms it and
    // picks it back up; ragdollMode already ran dropGrabObject on the death path, so this should
    // read invalid before a revive teleports, and if it does not, the revive writes someone else's
    // prop.
    bool  grabValid = false;
    bool  haveGrab = false;
    // Filled by the caller, off the game thread: neither reads UObject state, and the drill that
    // samples RSS on a cadence wants the sample at its own instant, not at the task's.
    bool  sessionRunning = false;
    double rssMb = -1.0;
    // Where the player is. The revive repositions to its target (JudgeRevivedLanding), and its
    // three-tier fallback reports that a call was dispatched, not that the player moved; the
    // position is the only honest assertion.
    float locX = 0.f, locY = 0.f, locZ = 0.f;
    bool  haveLoc = false;
    // The menu prep lib.loadLevel WOULD have written, read back. pause_mainMenu lives on the
    // screen tree all session, so before the cut moved to the loadLevel body those two writes
    // stuck through a cancelled travel and the next ESC showed a loading screen; now they are
    // never made, and the assertion is that they were not.
    int32_t screenSwiIdx = -1;   // in-game value is 1 (ui_menu uber @2445)
    int32_t canvasLoadingVis = -1;  // in-game value is 1 = ESlateVisibility::Collapsed
    // The damage indicator's worst directional accumulator
    // (gamemode.playerInterface.umg_damageIndicator.damage_{up,down,left,right}): a revived player
    // at full health wearing a red screen is death state that outlived the revive.
    float dmgRed = -1.f;
    // dmg_full's live Visibility. Separate from dmgRed: the death branch zeroes the four quadrants
    // and shows dmg_full in the same block, so a reader of the floats alone reports a clean HUD
    // while a full-screen red image is on screen.
    int dmgFullVis = -1;   // ESlateVisibility; 1 = Collapsed = the authored default
    // The second red: Add Player Damage spawns an effect_bloodLoss_C whose post-process and
    // ui_bloodLossBlur wash the whole world red, and a lethal hit pins its duration at the 120 s
    // cap, so it outlives the revive by two minutes unless the revive expires it. Counted as live
    // actors.
    int32_t bloodLossActors = -1;
    float bloodLossTime = -1.f;
    // The effect's own widget: effect_bloodLoss_C removes it in ReceiveDestroyed, so it should die
    // with the actor; counted separately, since an actor gone with the blur still up is the
    // teardown's bug, and both gone with the screen still red is a third source.
    int32_t bloodBlurInViewport = -1;
};

// The whole snapshot, GAME THREAD ONLY. Every field but `sessionRunning` and `rssMb`, which read
// no UObject state and belong to the caller's own instant.
DeathSnapshot ReadDeathState();

// The revive's landing graded against ITS OWN target (death_revive's LastReviveTarget): the pawn
// at `s`'s location within `toleranceCm` of it, and the target's kind `bed` whenever the world
// holds a bed (the dev switch VOTVCOOP_DEATH_NO_BED expects `kpp-fallback` instead). `detail`
// carries the reading, delta included. GAME THREAD ONLY: it walks the object index once for the bed.
struct LandingJudgement {
    bool  pass = false;
    float distCm = -1.f;
    char  detail[320] = {};
};
LandingJudgement JudgeRevivedLanding(const DeathSnapshot& s, float toleranceCm);

// Every UUserWidget-descended object on the viewport, by class name: a probe aimed at a suspect
// cannot find a source nobody thought of, an enumeration can. One array walk, once per run.
std::wstring CensusViewportWidgets();

// Every live ui_damageIndicator_C instance with its four quadrant values, and which one the
// revive's path points at: a reader of that one object can report 0.00 honestly while a
// different instance is the one on screen. Plus the world-side red sources.
std::wstring CensusDamageIndicators();

// What can tint the scene while leaving UMG untouched: a camera fade, a post-process component
// on the camera, player or gamemode, a PostProcessVolume, or scene lighting.
std::wstring CensusRenderState();

// Every live actor descending from effect_C, plus the gamemode's own effects_names array: the two
// halves of VOTV's effect system, which can disagree.
std::wstring CensusEffects();

}  // namespace harness::autotest
