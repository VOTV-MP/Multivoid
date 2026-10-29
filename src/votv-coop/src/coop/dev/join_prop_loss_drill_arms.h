// coop/dev/join_prop_loss_drill_arms.h -- the join prop-loss drill's shared helpers and the client steps of its arms
// that live beside join_prop_loss_drill.cpp: swing and throwat (join_prop_loss_drill_arms.cpp), heldbreak
// (join_prop_loss_drill_held.cpp) and rekey, twin, stack and late (join_prop_loss_drill_bracket.cpp). A private header
// of the drill (src tree, not include/): the helpers are defined in join_prop_loss_drill.cpp.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::join_prop_loss_drill::detail {

enum class Arm : uint8_t { Off, Break, Swing, HeldBreak, Fire, Rekey, Twin, Stack, Late, ThrowAt };

inline constexpr const wchar_t* kCanClass = L"prop_snack_beer_C";
inline constexpr const wchar_t* kCanRow = L"beer_c";           // the props-table row of the can
inline constexpr float          kMirrorRangeCm = 1500.f;       // the can nearest the client player
inline constexpr uint64_t       kMirrorBudgetMs = 30000;       // a wire mirror shows near the client within this
inline constexpr float          kPairRangeCm = 3000.f;         // the cans a bracket arm destroys: nearest the client player
inline constexpr float          kPairMaxCm = 25.f;             // two cans this close are the pair
inline constexpr float          kStackMaxCm = 30.f;            // three cans all this close are the stack
inline constexpr float          kBreakDamage = 100000.f;       // past any can's health
inline constexpr uint64_t       kAnswerBudgetMs = 15000;       // the key index answers within this
inline constexpr const char*    kBrokeLine = "[JPL] broke ";   // the client's copy of the host's can is gone
inline constexpr const char*    kStoodLine = "[JPL] stood ";   // throwat: the copy stands
inline constexpr const char*    kHostJudgedLine = "[JPL] host judged";
inline constexpr const char*    kClientJudgedLine = "[JPL] client judged ";   // + the key of the first can the host placed
inline constexpr const char*    kBracketDoneLine = "[JPL] bracket done";
inline constexpr const char*    kLateCanLine = "[JPL] late can ";             // + the key of the can placed after it

Arm         ArmOf();
const char* ArmName();
const char* Role();
bool        IsMeasureArm();   // swing or throwat
bool        IsThrowAt();
bool        IsBracketArm();   // rekey, twin, stack or late: judged by the host once the client's line names its can

void Pass();
void Fail(const char* fmt, ...);
void Inconclusive(const char* fmt, ...);   // the client's also says the line that ends the host
void Finish();

void*        LocalPlayer();
void         Cans(std::vector<void*>& out);
float        Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b);
std::string  Narrow(const std::wstring& w);
bool         JoinedAndQuiet();
void*        NearestMirror(void* me);
bool         KeyLive(const std::wstring& key);
bool         SpawnCan(const ue_wrap::FVector& at);        // the gamemode's own spawn verb; it returns no actor
bool         CallImpactDamage(void* can, void* causer);   // the can's physicsImpact component, damage kBreakDamage
bool         CallFireDamage(void* can);
bool         EndOnHostJudged();                           // the host's verdict line came: the client's DONE is printed

// The client's steps of the arms; the host's are in join_prop_loss_drill.cpp.
void MeasureClientTick(uint64_t now);
void MeasureOnDisconnect();
void HeldClientTick(uint64_t now);
void HeldOnDisconnect();
void BracketLoadTick();                 // rekey, twin, stack: the destroy before the bracket; late: sees the bracket open
void BracketClientTick(uint64_t now);
void BracketOnDisconnect();

}  // namespace coop::dev::join_prop_loss_drill::detail
