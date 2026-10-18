// coop/dev/laptop_tab_drill_steps.h -- how a laptop_tab_drill peer holds the PC or reads the tab: the holder walks to
// the laptop's Box, aims at its boxScreen, enters, and switches tabs a tick; the reader judges the last tab a holder
// reached. The arms (laptop_tab_drill.cpp) choose who does which and when. Private to coop/dev; game thread only.

#pragma once

#include "coop/interactables/laptop_tab.h"

#include <cstddef>
#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dev::laptop_tab_drill_steps {

// The tabs the arms name.
constexpr int kFloppyTab  = 10;
constexpr int kCamerasTab = 5;
// In a sequence, a step that is not a plain tab number: the sub-game button, whose tab is where canvas_subgame sits in
// the switcher, and a raw report past the switcher's last tab, which the drill sends itself.
constexpr int kSubgameMark = 100;
constexpr int kRangeMark   = 101;
constexpr int kRangeTab    = 255;

constexpr const wchar_t* kClaim = L"laptop";
constexpr uint64_t kFindBoundMs = 120000;

uint64_t NowMs();
uint8_t  LocalSlot();
bool     ReadOwnTab(int& idx);

// ---- the verdict: one line per arm, said once, then the peer's part is over -------------------------------
void Pass(const char* what);
void Fail(const char* what);
void Abandon(const char* why);
bool Done();
void MarkDone();
// What the judging peer says if the session ends before its verdict; empty on a peer that judges nothing.
void SetEndWhy(const char* why);
void ClearEndWhy();
void VerdictOnDisconnect();  // a viewer still waiting fails
void VerdictReset();

// ---- what the arm judges against, taken at its start -------------------------------------------------------
// The module's counts only grow, and a seed or a refusal that came before the judging peer got to its first step must
// still count as before the arm. `armName` is the arm's log name, a string with static storage.
void ArmBegin(const char* armName);
bool ArmStarted();
void ArmReset();
uint64_t ReceivedSince();
uint64_t RefusedSince(coop::laptop_tab::Cause c);

// ---- the reader: a viewer that judges the FINAL tab of a holder's run --------------------------------------
enum class ReadState : uint8_t { Waiting, Reached, Failed };
bool ReaderActive();
bool ReaderStart(int finalMark);  // False, the arm abandoned, when the final tab does not resolve on this peer
ReadState ReaderStep(char* why, size_t whySize);
void ReaderVerdict(const char* passWhat);
void ReaderReset();

// ---- the holder: walks to the PC, aims at boxScreen, enters it, and switches tabs -----------------------------------
void HolderStart(const int* seq, size_t n, int entryTab);
bool HolderIdle();
bool HolderDone();
void HolderTick(coop::net::Session* s);
void HolderCancelWalk();
void HolderReset();
uint64_t HeldReceived();  // the arrivals at the confirmed claim: what came after was sent while this peer held it

}  // namespace coop::dev::laptop_tab_drill_steps
