// ue_wrap/game_rules.h -- read the LOCAL peer's world rules (engine substrate).
//
// VOTV's per-world settings live in one struct, Fstruct_gameRules (~41 members:
// fallDamage, difficulty, funnySetting, customContent, seasons, extremeCombat,
// foodSpoilage, the 8 minigame toggles, the decay toggles, ...). The value the
// game actually consults at runtime is the PER-PEER copy at
// mainGameInstance.gameRules -- every rule read funnels through
// lib->getMainGameInstance().gameRules (RE 2026-07-08). GameInstance is one per
// process and is never replicated, so this reads THIS peer's effective rules.
//
// A joining client boots from the host's LIVE-captured save
// (coop/session/save_transfer.cpp), so the host's localGameRules travel inside
// the save blob; IF VOTV's load copies localGameRules -> GI.gameRules on the
// client (the same path single-player uses to restore rules), every peer's
// effective rules equal the host's. This accessor deliberately reads the LOCAL
// copy so the F1 panel shows the rules a peer is ACTUALLY under -- a host/client
// mismatch stays visible (diagnostic) instead of being masked by a broadcast.
//
// Members are enumerated by reflection (ue_wrap::reflection::EnumerateStructFields
// + FindBoolProperty), NOT a hardcoded field list: offsets resolve by name and
// the panel auto-adapts if a game patch adds/removes a rule. GUID-mangled BP
// names ("fallDamage_8_AEE...") are trimmed to their stable prefix for display.
//
// Principle 7: this is the engine read only -- no network, no UI. ui/ owns the
// render. Game-thread only (UObject + FField reads); snapshot once (rules are
// static post-world-load) and cache the result off-thread for the render.
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
