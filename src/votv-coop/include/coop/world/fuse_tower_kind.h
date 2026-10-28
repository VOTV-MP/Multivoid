// coop/world/fuse_tower_kind.h -- the kinds of fuse tower, one table over their wrappers. The lane's two modules
// (coop/world/fuse_tower_ops: what a player does at a tower and the host's judgement; coop/world/fuse_tower_rows: a
// tower's state as rows) are written once and ask this table what differs: which towers exist, how a tower reads
// into a row and a row is applied to it, which part an op strikes and where, what a pull and an insert need, what
// each does on the host's copy, and which script bodies the gate watches. A kind reaches the engine only through
// its ue_wrap wrapper. The coordinate towers (coordRadarDish_C, three, by id) and the radio tower (radiotower_C,
// one, id 0) are the two kinds. Game thread.

#pragma once

#include "coop/net/protocol.h"
#include "coop/world/fuse_tower_rows.h"  // Edges

#include "ue_wrap/core/types.h"  // FVector

#include <cstdint>

namespace ue_wrap::facts { class Function; }

namespace coop::fuse_tower_kind {

enum class Kind : uint8_t { Coordinate = coop::net::kFuseTowerKindCoordinate, Radio = coop::net::kFuseTowerKindRadio };
inline constexpr int kKinds = coop::net::kFuseTowerKinds;

// A tower in this world: its kind, its id within the kind, and the engine object.
struct Tower {
    Kind    kind = Kind::Coordinate;
    int32_t id = -1;
    void*   obj = nullptr;
};

// What a gate reads of a tower.
struct View {
    bool    broken = false;
    uint8_t fuseCount = 0;
    uint8_t fuses[8] = {};  // 0 empty, 1 good, 2 blown
    uint8_t lightCount = 0;
};

// ---- finding -----------------------------------------------------------------------------------------------

// Whether the kind's tower can be found now (its wrapper has resolved, and for the radio tower the gamemode holds
// it); a wrapper not resolved tries again at its own pace.
bool Resolved(Kind kind);

// Every live tower of every kind into `out`, in the order the host writes its rows: the radio tower first, so a
// cap never drops it, then the coordinate towers by id. How many, or -1 while nothing has resolved, so a caller
// never reads an unresolved world as "no towers".
int32_t ReadAll(Tower* out, int32_t cap);

// The tower of (kind, id), or null.
void* Find(Kind kind, int32_t id);

// The kind and id of `obj`, which a script-gate callback hands over: a name compare, never a walk of the object
// array. False when it is not a tower.
bool Identify(void* obj, Kind& kind, int32_t& id);

bool ReadView(Kind kind, void* tower, View& out);

// ---- rows --------------------------------------------------------------------------------------------------

// The tower into `row`; false when it does not read.
bool ReadRow(const Tower& tower, coop::net::FuseTowerRow& row);

// CLIENT: what a row's application knows besides the row. `live` is false on the tower's first row, which is
// state and not a player's act: no sound or alert of its own. `prevFlags` are the flags of the row last applied to
// this tower (0 before any), so a lever started by the host is started here once. `sizeWarned` is the lane's
// once-flag for a puzzle sized differently here and on the host.
struct ApplyCtx {
    bool                             live = false;
    uint8_t                          prevFlags = 0;
    coop::fuse_tower_rows::Edges*    edges = nullptr;
    bool*                            sizeWarned = nullptr;
};

// CLIENT: the tower brought to `row` (the host's row with my untaken claims), each change made as the tower's own
// graph makes it.
void ApplyRow(Kind kind, void* tower, const coop::net::FuseTowerRow& row, const ApplyCtx& ctx);

// CLIENT: the panel brought toward the row's, through the tower's own retract. True when the retract was asked;
// `opens` is the way it was asked, `ran` whether the retract ran. A kind with no panel asks nothing.
bool ConvergePanel(Kind kind, void* tower, void* player, const coop::net::FuseTowerRow& row, bool& opens, bool& ran);

// ---- ops ---------------------------------------------------------------------------------------------------

// The world location of the part op `op` strikes, where a use reaches. False while the part is not there.
bool PartLocation(Kind kind, void* tower, uint8_t op, int32_t index, ue_wrap::FVector& out);

// The tower's own use of the part, as `player` makes it.
bool Use(Kind kind, void* tower, void* player, uint8_t op, int32_t index);

// CLIENT: what a use the player is making does, from the look-at the tower holds. Send: the op is `op` and `index`
// (the use is refused here and goes to the host). Pull: a fuse pull, which runs here and is claimed as it ends.
// None: the player's own game runs it (a use the tower's own gates would turn away: a radio tower that is whole or
// busy answers with its own hint).
enum class Look : uint8_t { None, Send, Pull };
Look ReadUse(Kind kind, void* tower, uint8_t& op, int32_t& index);

// CLIENT: the fuse slot an insert fills, which the tower's own insert reads.
bool ReadFuseLook(Kind kind, void* tower, int32_t& slot);

// HOST: nullptr when the tower's own use of the part op `op` strikes (a button, a lever, the retract, the reset
// lever) would act on the host's copy, else why not: the use's own gates, which otherwise leave a hint on the
// host's screen and nothing else.
const char* CanUse(Kind kind, void* tower, uint8_t op);

// HOST: nullptr when the move is allowed on the host's copy, else why not.
const char* CanPull(Kind kind, void* tower, const View& view, int32_t slot);
const char* CanInsert(Kind kind, void* tower, const View& view, int32_t slot);

// Whether an insert in this world keeps the fuse in the player's hand: on the two easiest difficulties the game
// solves the puzzle with it and spends nothing, so the insert is no claim on a fuse but a verb the host runs (a
// client's is refused at the gate and sent, like a press). False while the difficulty is not readable.
bool EasyInsert();

// HOST: the move on the host's copy; the fuse is already in or out of the player's hand (an insert's already
// spent, above the two easiest difficulties).
void TakePull(Kind kind, void* tower, int32_t slot);
void TakeInsert(Kind kind, void* tower, int32_t slot);

// HOST: the easy insert on the host's copy, as the game runs it with the fuse kept: the radio tower writes the
// slot good and completes its minigame, the coordinate tower solves its puzzle and writes no fuse.
void TakeEasyInsert(Kind kind, void* tower, int32_t slot);

// The fuse props every kind shares: a good fuse (what an insert takes), one spawned at `at`, and the plain prop a
// pull hands the puller.
bool IsFuse(void* actor);
void* SpawnFuse(const ue_wrap::FVector& at);
bool IsPulledFuse(void* actor);

// ---- the gate ----------------------------------------------------------------------------------------------

// The script bodies a kind's gate watches: its roll, the lever's judgement (both refused on a client), a use
// (actionOptionIndex) and an insert (playerUsedOn); and, where the kind has them, the puzzle's and the fuses' own
// generators, watched after the fact so a client's own generation is followed by the host's kept row (null for a
// kind that is not watched there).
struct WatchRows {
    const ue_wrap::facts::Function* roll;
    const ue_wrap::facts::Function* judge;
    const ue_wrap::facts::Function* use;
    const ue_wrap::facts::Function* insert;
    const ue_wrap::facts::Function* genPuzzle;
    const ue_wrap::facts::Function* genFuses;
};
const WatchRows& WatchesOf(Kind kind);

}  // namespace coop::fuse_tower_kind
