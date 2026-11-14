// coop/interactables/tv_link_resolver.h -- from an address a player typed to one the game's media
// player can open.
//
// The game's player opens a file served whole over http(s): one stream with picture and sound. A
// YouTube page is not that, so its address is handed to the yt-dlp program shipped beside the mod
// (tools\yt-dlp.exe), which prints the address of such a stream; YouTube ties that address to the
// machine that asked, so every peer resolves a link for itself. An address comes from another
// player, so it is checked before it reaches the game or a command line: http or https, printable,
// no quote, space or backslash, and short enough for the wire.

#pragma once

#include <functional>
#include <string>

namespace coop::tv_link_resolver {

// The address may be opened at all: http or https, printable, nothing a command line would split on.
bool ValidLink(const std::wstring& link);

// The address is valid and short enough to cross the wire.
bool Sendable(const std::wstring& link);

// The address names a YouTube page: it is resolved before it is opened.
bool NeedsResolve(const std::wstring& link);

// The yt-dlp program is where the package puts it.
bool ToolPresent();

// Run the tool for `link` on a worker thread. `done` runs on the game thread with the stream's
// address, or with an empty string when the tool is missing, fails or takes over 45 s. An address
// resolved in the last twenty minutes is answered from memory, without a run.
void Resolve(const std::wstring& link, std::function<void(std::wstring direct)> done);

}  // namespace coop::tv_link_resolver
