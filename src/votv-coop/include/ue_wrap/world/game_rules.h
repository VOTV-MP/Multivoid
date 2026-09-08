// ue_wrap/game_rules.h -- read the LOCAL peer's world rules (engine substrate).
//
// The per-world settings live in one struct, Fstruct_gameRules -- about 41 members: fall damage,
// difficulty, seasons, food spoilage, the minigame and decay toggles. What the game consults at
// runtime is the PER-PEER copy at mainGameInstance.gameRules, since every rule read funnels through
// lib->getMainGameInstance().gameRules and a GameInstance is one per process and never replicated.
//
// A joining client boots from the host's live-captured save, so the host's localGameRules ride the
// blob; whether the load then copies them into GI.gameRules is not visible in the blueprints, so
// whether every peer's rules end up equal to the host's is unproven. This reads the LOCAL copy on purpose, so the panel shows the rules a peer is
// ACTUALLY under and a mismatch stays visible. Members are enumerated by reflection, not a
// hardcoded list, so offsets resolve by name and the panel adapts if a patch adds or removes a
// rule; GUID-mangled names are trimmed to a stable prefix. Principle 7: the engine read only -- ui/
// owns the render. Game thread; snapshot once and cache, rules being static after the world load.
#pragma once

#include <string>
#include <vector>

namespace ue_wrap::game_rules {

enum class Kind { Bool, Enum, Float };

// One rule, resolved + read. `label` is the trimmed, prettified member name
// (e.g. "Fall damage"); `kind` picks which value member is meaningful.
struct RuleField {
    std::string label;
    Kind        kind = Kind::Bool;
    bool        bval = false;   // Kind::Bool
    int         ival = 0;       // Kind::Enum (raw ordinal -- VOTV strips enum
                                //             display names in the cook)
    float       fval = 0.f;     // Kind::Float
};

struct Snapshot {
    bool                   valid = false;
    int                    gamemode = -1;   // mainGameInstance.GameMode ordinal
    std::string            gamemodeName;    // "Story"/"Sandbox"/... or "#N"
    std::vector<RuleField> fields;          // gameRules members, declaration order
};

// Snapshot the local peer's world rules into `out`. Returns false (out.valid
// stays false) if the GameInstance / gameRules struct isn't resolvable yet
// (still booting). Game-thread only.
bool ReadLocal(Snapshot& out);

}  // namespace ue_wrap::game_rules
