// coop/dev/build_identity_detail.h -- the building identity drill's private seams between its files (src tree, not
// include/); see coop/dev/build_identity_drill.h.

#pragma once

#include <string>
#include <vector>

namespace coop::dev::build_identity {

// One live building piece of the current world: its class, the key GetInteractableKey reads off it now, the level it
// belongs to, and whether that level is the world's persistent one (a level-placed piece).
struct Piece {
    std::string  cls;
    std::wstring key;
    std::string  level;
    bool         persistent = false;
};

// Every live nail, wall, unfinished wall and poster of the current world: any instance of a class that walks to one
// of the four roots, resolved here by name, so the walk does not stand on the test under judgement. Game thread.
void CollectPieces(std::vector<Piece>& out);

// What the level-placed pieces came to: how many customWall_C and poster_C, and their census text, the tokens
// "<class>=<key>" sorted and joined by ';' -- what the host and the client compare.
struct LevelCensus {
    int         walls = 0;
    int         posters = 0;
    std::string text;
};

// One "build-identity: census ..." line per piece, a summary line, and the level census of `pieces`.
LevelCensus SayAndTotal(const char* role, const std::vector<Piece>& pieces);

// "build-identity: ..." at info level.
void Info(const char* fmt, ...);

}  // namespace coop::dev::build_identity
