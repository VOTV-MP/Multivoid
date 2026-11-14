// ue_wrap/devices/tv.h -- the game's television (prop_tv2_C and its children): the engine-wrapper
// layer. No network or coop state.
//
// A television plays through one of the gamemode's sixteen shared media channels (useChannel binds
// the set to a channel's UMediaPlayer and texture) and opens a video through its own openLink, which
// takes a struct_tv {name, link, isUrl}: a file of the custom-content folder, or a web address handed
// to UMediaPlayer::OpenUrl. openLink does nothing while the set has no power or the game's Custom
// Content setting is off. Game thread only.

#pragma once

#include <string>
#include <vector>

namespace ue_wrap::tv {

// `obj` is a live television of this lineage.
bool IsTv(void* obj);

// The televisions of the current world.
std::vector<void*> FindAll();

// The set's `powered` flag (its cord is plugged into a live socket).
bool Powered(void* tv);

// The media player of the set's channel, or null when it has none.
void* PlayerOf(void* tv);

// Bind the set to the first channel the gamemode will give it. True when it has a player after.
bool TakeAnyChannel(void* tv);

// The set's own openLink with {name, link, isUrl=true}. True when the call ran; the game decides
// whether the address opens and writes Loading, Successful or Error on the set's remote.
bool OpenLink(void* tv, const std::wstring& name, const std::wstring& link);

// openLink's arguments, read from its frame (`function` is the UFunction, `locals` its locals).
bool ReadOpenLinkArgs(void* function, const unsigned char* locals, std::wstring* name, std::wstring* link,
                      bool* isUrl);

// What the set was last asked to open: its own `Data` member, which openLink writes.
bool ReadCurrent(void* tv, std::wstring* name, std::wstring* link, bool* isUrl);

// The player's state: 1 or 0, or -1 with no player. And its play time in seconds, or -1.
int    IsPlaying(void* tv);
double PlayTime(void* tv);

// The player's length in seconds, or -1 with no player or an unopened one.
double Duration(void* tv);

// The player's own Pause, Play and Seek. False with no player or when the call does not run.
bool Pause(void* tv);
bool Resume(void* tv);
bool Seek(void* tv, double seconds);

// The remote (ui_tvInterface_C): the set it serves, the time its playback slider points at in seconds (-1 when
// unreadable), and its search box emptied with the list rebuilt, as the game does after a search is cleared.
void*  RemoteSet(void* remote);
double RemoteSliderSeconds(void* remote);
bool   ClearRemoteSearch(void* remote);

// Drill aid: write the `powered` flag as a plugged cord would.
bool ForcePowered(void* tv, bool on);

}  // namespace ue_wrap::tv
