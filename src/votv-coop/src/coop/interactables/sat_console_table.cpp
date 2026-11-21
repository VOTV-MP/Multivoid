// coop/interactables/sat_console_table.cpp -- see coop/interactables/sat_console_table.h.

#include "coop/interactables/sat_console_table.h"

#include <cwctype>

namespace coop::sat_console_table {
namespace {

// The commands that rest on the shared world. The calibration (sd.cal, sd.calall) writes the dishes;
// sv.request writes the slotted floppy's data; sv.eject spawns the disc; sauce.get a gift box on every
// call; the rest read the world's servers, dishes, towers, generators and clock. Read off
// ui_console.enterCommand's switch.
const wchar_t* const kHostCommands[] = {
    L"sv.ping", L"sd.cal", L"sd.calch", L"sd.calall", L"sv.hash", L"tr.check", L"tw.check", L"time",
    L"sv.request", L"sv.eject", L"sv.check", L"cr.check", L"sauce.get", L"sv.upgrades",
    // Under the game rule funnySetting: a door alienated, a dingus, the rufus and thiccfus spawns and
    // their removal, the llama and its soul, the madness at night.
    L"summon_alien", L"alien", L"maxwell", L"argemwell", L"gnarpwell", L"eriewell",
    L"gooseworx.rufus", L"rufus.fuckoff", L"gooseworx.thiccfus", L"llama.saatana", L"madness.combat",
};

// The debug rows that rest on the shared world: the events' list and their scrape, the two deletes,
// the drone sack's respawn, the moon, the reputation, the game rules, and the two flags a lightning
// strike's world effects read, ligh_env (its environment damage) and ligh_exp (its explosion, which
// pushes, burns and damages). The others -- help, the meta log, the strike's flash, light and particles
// on this machine, the PC screen on the viewport, the damage notifications -- are the typist's.
const wchar_t* const kHostDebugRows[] = {
    L"events", L"del.wg", L"del.murderfur", L"scrape", L"moonphase", L"moontype", L"rep", L"rules",
    L"respawn_sack", L"ligh_env", L"ligh_exp",
};

// The commands among those that spawn on every call; the others that spawn stop at one live (a dingus,
// the llama's soul, the madness) or need something to spawn from (the ejected disc).
const wchar_t* const kSpawnEveryCall[] = {L"sauce.get", L"gooseworx.rufus", L"gooseworx.thiccfus"};

bool EqualsNoCase(const std::wstring& a, const wchar_t* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    return i == a.size() && b[i] == L'\0';
}

template <size_t N>
bool Listed(const std::wstring& word, const wchar_t* const (&list)[N]) {
    for (const wchar_t* w : list)
        if (EqualsNoCase(word, w)) return true;
    return false;
}

// enterCommand: n = the first space. With none the command and the argument are both the whole line;
// with a leading one the command is the whole line and the argument what follows that space; otherwise
// the command is what precedes the space and the argument what follows it.
void Split(const std::wstring& line, std::wstring& command, std::wstring& arg) {
    const size_t space = line.find(L' ');
    command = (space == std::wstring::npos || space == 0) ? line : line.substr(0, space);
    arg = space == std::wstring::npos ? line : line.substr(space + 1);
}

}  // namespace

Runs Classify(const std::wstring& line) {
    std::wstring command, arg;
    Split(line, command, arg);
    if (Listed(command, kHostCommands)) return Runs::OnHost;
    if (EqualsNoCase(command, L"debug") && Listed(arg, kHostDebugRows)) return Runs::OnHost;
    return Runs::Here;
}

bool SpawnsEveryCall(const std::wstring& line) {
    std::wstring command, arg;
    Split(line, command, arg);
    return Listed(command, kSpawnEveryCall);
}

std::wstring CommandOf(const std::wstring& line) {
    std::wstring command, arg;
    Split(line, command, arg);
    return command;
}

bool Conflicts(const std::wstring& line, const std::wstring& running) {
    const std::wstring a = CommandOf(line), b = CommandOf(running);
    const bool aAll = EqualsNoCase(a, L"sd.calall"), bAll = EqualsNoCase(b, L"sd.calall");
    const bool aOne = EqualsNoCase(a, L"sd.cal"), bOne = EqualsNoCase(b, L"sd.cal");
    return (aAll && (bAll || bOne)) || (aOne && bAll);
}

}  // namespace coop::sat_console_table
