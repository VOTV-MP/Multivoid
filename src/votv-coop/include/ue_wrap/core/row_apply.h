// ue_wrap/core/row_apply.h -- what became of a row handed to one of the game's own append verbs (the desk's
// saveSignal, the laptop's addEmail). Three answers, because a receive lane does different things with
// them: Applied, the verb ran to its end; Refused, the game was asked and said no, or the row cannot be
// put to it at all, which is final for that row; NotNow, the game cannot be asked yet (no world, the verb
// or the string library the row is minted with not resolved), which is the lane's state and never the
// row's.
#pragma once

#include <cstdint>

namespace ue_wrap {

enum class RowApplyResult : std::uint8_t { Applied, Refused, NotNow };

struct RowApply {
    RowApplyResult result = RowApplyResult::Refused;
    const char* why = "";  // a static string naming the cause; empty when Applied
};

}  // namespace ue_wrap
