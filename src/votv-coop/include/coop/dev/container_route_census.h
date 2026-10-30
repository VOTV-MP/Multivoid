// coop/dev/container_route_census.h -- [dev] which route a container verb came by. Every addObject / takeObj on a
// shared world container is counted by the route whose body was running (sg::IsBodyActive) when it ran: R1 a container slot press
// (uicomp_playerInvContainerSlot_C::pressButton), R2 the take key (ui_playerInventory_C::em_take), R3 hold+run
// (prop_container_C::playerUsedOn with the player running), R4 the overlap (prop_container_C::putObjectIn_overlap),
// R5 the vacuum (prop_vacuum2_C::gather), R6 none of them (the game's own logic, named by its caller's class and
// the outermost script function of its stack). A client's take is an act the host answers, and a client sends a
// container slice only for a birth (an item it made, filed into a container), so the slices it sends are charged to
// the route that last wrote the container and are few. Observe-only: it never cancels a call.
//
// Armed by the [dev] row container_route_census (both peers): 1 the census, drill the census plus the drill that
// drives each route once (coop/dev/container_route_drill). Lines tagged
// [CI-CENSUS]. Game thread.

#pragma once

#include "ue_wrap/core/script_gate.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dev::container_route_census {

inline constexpr int kRoutes = 6;   // R1..R5, then R6 (none open)

// Unix milliseconds of the system clock, which both peers of one machine share: the stamp of the r4 lines.
uint64_t UnixMs();

bool IsEnabled();           // the row is non-empty
bool IsDrill();             // drill

// Per pump tick: registers the watches once the session runs, says each seam's state once, and reports every 30 s.
void Tick(coop::net::Session* session);

// The final report, then the counts start over.
void OnDisconnect();

// The census hook of container_writer_watch's verb entry: counts the verb. `take` is takeObj.
void OnVerb(const ue_wrap::script_gate::Call& call, bool take);

// A client-authored slice for `eid` left this peer (charged to the route that last wrote the container). A client
// sends one only for a birth, so after a take by act the count stays at 0.
void NoteSliceSent(uint32_t eid);

// Prints the non-zero cells now, `why` naming the moment.
void ReportNow(const char* why);

// What the drill reads. All since the session began.
uint64_t RouteEntries(int route);      // seam entries of route 0..4 (R1..R5)
uint64_t R4Overlaps();
// The overlaps of a SHARED ITEM (an actor that is no container) into a container, counted where the overlap is entered (the PRE of
// putObjectIn_overlap): all, those whose entering component was not simulating physics, and the clock (UnixMs) of the first since the
// last MarkItemOverlaps, 0 when none.
uint64_t R4ItemOverlaps();
uint64_t R4ItemOverlapsNoSim();
uint64_t FirstItemOverlapMs();
void MarkItemOverlaps();

// The last overlap a SHARED actor made into a container's putObjectIn_overlap, for the container intent drill's throw
// arms: `seq` counts them since the session began, `container` and `eid` are the element ids of the container and of the
// actor (kInvalidId when a side has none), `sim` whether the overlapping component simulates physics, `t` the clock of the
// r4 lines. `actor` is a bare pointer for an equality test: it is never dereferenced.
struct R4Overlap {
    uint64_t seq = 0;
    uint32_t container = 0;
    uint32_t eid = 0;
    bool     sim = false;
    uint64_t t = 0;
    const void* actor = nullptr;
};
R4Overlap LastR4Overlap();

uint64_t GenCalls();                  // ui_playerInventory_C::gen_player bodies run (the screen's rebuild)
uint64_t RouteVerbs(int route, bool take);      // verbs counted on a shared container under route 0..5 (R1..R6)
// The clock (UnixMs) of the first verb counted under `route` since the last MarkVerbs for it, 0 when none: the verb's own event,
// stamped where it is counted. A drill marks before its act and reads after, and polls nothing.
void MarkVerbs(int route, bool take);
uint64_t FirstVerbMs(int route, bool take);

// The six watches' state, said by the watches themselves: Live once every one is live, Dead as soon as one settles
// without a hook (its name in `dead`), Pending before that and while the census has not registered them.
enum class Seams : uint8_t { Pending, Live, Dead };
Seams SeamsState(const char** dead);

// Verbs run inside the scope do not count (a drill's own setup act).
class Quiet {
public:
    Quiet();
    ~Quiet();
    Quiet(const Quiet&) = delete;
    Quiet& operator=(const Quiet&) = delete;
};

}  // namespace coop::dev::container_route_census
