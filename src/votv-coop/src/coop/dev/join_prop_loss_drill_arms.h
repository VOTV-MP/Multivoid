// coop/dev/join_prop_loss_drill_arms.h -- the join prop-loss drill's shared helpers and its two measure arms, swing and
// throwat (join_prop_loss_drill_arms.cpp). A private header of join_prop_loss_drill.cpp (src tree, not include/): the
// helpers are defined there, the arms' client steps here.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::join_prop_loss_drill::detail {

inline constexpr const wchar_t* kCanClass = L"prop_snack_beer_C";
inline constexpr const wchar_t* kCanRow = L"beer_c";           // the props-table row of the can
inline constexpr float          kMirrorRangeCm = 1500.f;       // the can nearest the client player
inline constexpr uint64_t       kMirrorBudgetMs = 30000;       // a wire mirror shows near the client within this
inline constexpr const char*    kBrokeLine = "[JPL] broke ";   // the client's copy of the host's can is gone
inline constexpr const char*    kStoodLine = "[JPL] stood ";   // throwat: the copy stands
inline constexpr const char*    kHostJudgedLine = "[JPL] host judged";

bool        IsMeasureArm();   // swing or throwat
bool        IsThrowAt();
const char* ArmName();

void Pass();
void Fail(const char* fmt, ...);
void Inconclusive(const char* fmt, ...);   // the client's also says the line that ends the host
void Finish();

void*       LocalPlayer();
void        Cans(std::vector<void*>& out);
float       Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b);
std::string Narrow(const std::wstring& w);
bool        JoinedAndQuiet();
void*       NearestMirror(void* me);

// The client's steps of the two measure arms; the host's are the break arms'.
void MeasureClientTick(uint64_t now);
void MeasureOnDisconnect();

}  // namespace coop::dev::join_prop_loss_drill::detail
