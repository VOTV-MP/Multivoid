// coop/dev/pi3_drill_internal.h -- what pi3_drill.cpp (the phases and the tick) and pi3_arms.cpp (the arm table,
// the verdict tests and the lines) share. See coop/dev/pi3_drill.h.

#pragma once

#include "coop/dev/pi3_observer.h"

#include <cstddef>
#include <cstdint>

namespace coop::dev::pi3_drill::detail {

constexpr int kMaxVerbs = 6;

enum class ArmId : uint8_t { Off, WispW, WispO, WispP, Kelura, Orborb, OrbBirth, Grunt, Control, Client };

// The spawner's steps: Wait for the session and the observer, Apart (walk away from the still body), Face it, Rest
// (the body comes to a stand), Spawn the site with the observer armed, Observe until the horizon. Hold is the
// orbbirth arm's: the site lives on and is kept in view, with no verdict, for the join the rig makes after it.
enum class Phase : uint8_t { Wait, Apart, Face, Rest, Spawn, Observe, Hold, Done };

// One site and its verdict. `judged` are the verbs a puppet's touch must not reach (recorded: FAIL); `required` are
// the verbs that must be recorded (a green's rewind, the control's served end and quit), `unmet` naming them.
struct Spec {
    const char*    label;    // the arm's config value, and its name on every line
    const wchar_t* cls;
    const wchar_t* path;     // the /Game object path the class is loaded from when the index lacks it
    const char*    counter;  // the PI-2b row counter the verdict reads
    const char*    judged[kMaxVerbs];
    const char*    required[kMaxVerbs];
    const char*    unmet;
    bool           counterMoves;  // the PI counter must rise (a puppet's touch is refused); false: it must stand
    int            horizonMs;   // how long the arm watches after its anchor, measured from its red (pi3_arms.cpp)
};

// The table's rows, in the order the arms use them: a row arm's own, the control's, and the client arm's three legs.
enum SpecId : int {
    kSpecWispW, kSpecWispO, kSpecWispP, kSpecKelura, kSpecOrborb, kSpecOrbBirth, kSpecGrunt, kSpecControl,
    kSpecClientWispW, kSpecClientWispP, kSpecClientReading,
    kSpecCount
};
const Spec& SpecOf(int id);

// The arm a config value names (Off for anything else), the config value of an arm, and the row an arm's leg uses
// (leg 1..3 only matters to Client).
ArmId       ArmFromName(const char* name);
const char* ArmName(ArmId arm);
int         SpecIdOf(ArmId arm, int leg);

// The spec's class: from the index, else loaded by its path; null when neither finds it.
void* ResolveClass(const Spec& spec);

// How many calls the PI-2b row of that name has refused since the process started.
uint32_t Counter(const char* row);

using Seen = pi3_observer::Seen;

// A verb's record in the observer's report, or null.
const Seen* Find(const Seen* seen, int n, const char* verb);

// The judged verb the report holds that was reached first, or null.
const char* FirstJudged(const Spec& spec, const Seen* seen, int n);

// A cancel-list verb with more calls than cancels, or null.
const char* FirstUncancelled(const Seen* seen, int n);

// The report holds a verb the orb's BeginPlay guard runs when it fails: lib_C::end or QuitGame.
bool BirthGuardReached(const Seen* seen, int n);

// Every judged and every required verb of the spec is in the report.
bool Complete(const Spec& spec, const Seen* seen, int n);

// What a verdict finds unmet: "counter" when the PI counter did not do what the spec wants, else the spec's `unmet`
// when a required verb is missing, else null.
const char* Unmet(const Spec& spec, const Seen* seen, int n, bool counterOk);

// The lines. `label` is the arm, or "client leg=<n>".
void Label(char* out, size_t cap, const Spec& spec, int leg);
void FormatVerbs(char* out, size_t cap, const Seen* seen, int n);
void LogSeen(const char* label, bool complete, const Seen* seen, int n, int puppet, int own);
void LogStill(const char* arm, const Seen* seen, int n, int own);

}  // namespace coop::dev::pi3_drill::detail
