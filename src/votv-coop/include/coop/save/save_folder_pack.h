// coop/save/save_folder_pack.h -- a save folder's load set as one byte stream, and back.
//
// A save is a folder (ue_wrap/engine/save_folder.h), and a joiner's load needs three of its files:
// main.sav (the world), persistent.sav, and main.meta (mode, rules, starter map). A save left inside an
// area needs a fourth, areas/<area>.sav: the world the load swaps in for main.sav. The save transfer
// streams them as one blob: "MVSF", a u32 file count, then per file a u8 name length, the ASCII name, a
// u32 byte count and the bytes, little-endian. Only those names are packed or accepted -- the area's
// file by a name that is one plain path component -- so a blob can never name a path outside the folder
// it is unpacked into. Pure file I/O, any thread.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace coop::save_folder_pack {

// The files of `dir` a load needs, packed into `out` (replaced): the three, and areas/<area>.sav when
// `area` names one (empty for a save outside any area). False when one is missing or unreadable, or
// `area` is not a name one file can have.
bool Pack(const std::filesystem::path& dir, const std::wstring& area, std::vector<uint8_t>& out);

// Unpack `data` into `dir` (created when absent), each file written through a temporary and a
// rename. False on a malformed blob, a missing file, a name outside the set, or a failed write.
bool Unpack(const uint8_t* data, size_t len, const std::filesystem::path& dir);

}  // namespace coop::save_folder_pack
