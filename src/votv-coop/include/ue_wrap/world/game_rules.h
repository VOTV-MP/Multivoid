// ue_wrap/world/game_rules.h -- the world rules of the running world (engine substrate).
//
// The per-world settings are one struct, Fstruct_gameRules: 36 members, fall damage, difficulty,
// seasons, food spoilage, the minigame and decay toggles. The game keeps it twice. The SAVED copy is
// saveSlot.localGameRules, and a save file names only the rules that differ from the defaults. The
// PER-PROCESS copy is mainGameInstance.gameRules, never saved, and it is the one nearly every rule
// is read from (three are read off the save object: blood loss, experimental WIP, cold swap). One
// blueprint writes the process copy, the game's slot menu on its way into a world, so a load that
// does not pass that menu puts the rules in force itself: ApplySavedToProcess, called by the two
// boots in ue_wrap/engine/engine_save.cpp, on the host and on a joiner alike. A joiner loads the
// host's captured save, whose saved copy is the host's, so every peer ends under the host's rules.
// ReadLocal reads BOTH copies of the local peer, so a rule whose copies differ stays visible.
// Members are enumerated by reflection, so the list adapts if a patch adds or removes a rule.
// Principle 7: the engine read and write only -- ui/ owns the render. Game thread.
#pragma once

#include <string>
#include <vector>

namespace ue_wrap::game_rules {

enum class Kind { Bool, Enum, Float };

// One rule, resolved + read. `key` is the member name with its blueprint tail cut ("fallDamage"),
// the name a rule is known by; `label` is the same, prettified ("Fall damage"); `kind` picks which
// value member is meaningful.
struct RuleField {
    std::string key;
    std::string label;
    Kind        kind = Kind::Bool;
    bool        bval = false;   // Kind::Bool
    int         ival = 0;       // Kind::Enum: the ordinal
    std::string valueName;      // Kind::Enum: the game's own name for it ("Normal"); empty if the
                                //             enum object did not resolve
    float       fval = 0.f;     // Kind::Float
};

struct Snapshot {
    bool                   valid = false;
    int                    gamemode = -1;   // mainGameInstance.GameMode ordinal
    std::string            gamemodeName;    // "Story"/"Sandbox"/... or "#N"
    std::vector<RuleField> fields;          // gameRules members, declaration order
    bool                   savedValid = false;  // the save object was up and `saved` was read
    std::vector<RuleField> saved;           // the same members off the save object's localGameRules
};

// Snapshot the local peer's world rules into `out`. Returns false (out.valid
// stays false) if the GameInstance / gameRules struct isn't resolvable yet
// (still booting). Game-thread only.
bool ReadLocal(Snapshot& out);

// The `n`-th rule of `kind` in declaration order, or null. The game's rules pane addresses a rule
// this way: a checkbox row carries the index of its bool among the bools, the slider row of its
// float among the floats, a combo row of its enum among the enums.
const RuleField* NthOfKind(const std::vector<RuleField>& rules, Kind kind, int n);

// Put the save's rules in force: copy `save.localGameRules` over `gameInstance.gameRules`. The
// game does this in its slot menu on the way into a world (ui_saveSlots, the one blueprint that
// writes the process copy), so a load that does not pass that menu has to do it itself, and BEFORE
// the travel: difficulty, fall damage, water fall damage, nightmares and the grass rule are latched
// on a BeginPlay. Returns how many rules the copy changed, 0 when the two already agreed, -1 when
// either struct could not be resolved (nothing written). Game thread.
int ApplySavedToProcess(void* gameInstance, void* save);

// enum_gamemode's member count (enum_MAX): an ordinal at or above it is not a mode.
constexpr int kGameModeCount = 8;

// The local GameInstance's mode ordinal alone, without the rules walk; -1 before it boots.
// Game-thread only.
int ReadLocalGameMode();

}  // namespace ue_wrap::game_rules
