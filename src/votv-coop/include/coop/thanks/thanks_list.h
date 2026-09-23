// coop/thanks/thanks_list.h -- the thanks list: the people the main menu names under the version
// lines, grouped in sections. The names are data, never code: one text file
// (assets/thanks/thanks.txt) is embedded as the fallback and served by the master at /v1/thanks,
// so a name is added or taken out by publishing the file, with no release. The game keeps its
// supporter list the same way (downloaded, one name per line under a tier tag); what differs is
// that this one has a real fallback, is cached beside the executable, and is fetched only where
// the mod already talks to its master, never from the title screen alone.
//
// Two copies are weighed: the build's, and what the master last said. The higher `revision` wins
// and the master takes a tie, so a master holding an old file cannot take names away from a newer
// build. The cache is only a memo of one master's last word, and names that master: a new answer
// from the chosen master replaces it whatever its revision, and that master's "no list" erases it
// (another master having none says nothing about it), so a copy a master once served never
// outlives that master's say. The format is in the data file's header.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::thanks_list {

struct Section {
    std::string title;               // UTF-8, as written; a word of the menu's sentence ("testers")
    uint32_t    rgb = 0xFFFFFF;      // sRGB 0xRRGGBB: the colour of that word and of every name below
    bool        apart = false;       // `apart`: its names stand in a column of their own
    std::vector<std::string> names;  // UTF-8, one per line of the file
};

struct List {
    int         revision = 0;
    std::string title;
    std::vector<Section> sections;   // only sections that hold a name
};

// Parses one copy of the file. Text from the master is untrusted: the size, the section count
// and the name count are capped, a name that is not well-formed UTF-8 is dropped whole, and
// controls, line separators and the invisible and bidi-override codepoints are taken out of what
// is kept. False when nothing displayable came out or the revision is unreadable, `out` then
// untouched. Pure.
bool Parse(const char* text, size_t size, List& out);

// Loads the build's copy and the chosen master's cached word and settles what is shown. Once, at
// boot; any thread.
void Init();

// Asks the chosen master on a worker, records its answer as the cache, and settles again.
// Coalesced and rate-floored like the version check it rides with; an unreachable master changes
// nothing.
void RefreshFromMaster();

// The current list, and a counter that moves when it is replaced, so a consumer rebuilds on a
// change and otherwise asks only this. Any thread.
uint64_t Generation();
uint64_t Copy(List& out);

}  // namespace coop::thanks_list
