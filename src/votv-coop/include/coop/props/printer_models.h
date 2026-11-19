// coop/props/printer_models.h -- what this machine's copy of a 3D printer model is, for VOTV-MP/Multivoid#49.
//
// The game's printer and a printed object both read a model from the player's own disc: the folder
// <asset folder>\meshes\printer\<name>, its <name>.obj and whatever textures and settings sit beside it, where the asset
// folder is mainGamemode_C's `assetFolder` or, when that is empty, %LOCALAPPDATA%\VotV\Assets (lib_C's
// GetAssetFolder). A printed object shared between players is only the same object if every player's folder is the same,
// so a model is named here by a hash of its whole folder: each file's path inside it and its bytes. Game thread.
#pragma once

#include <cstdint>
#include <string>

namespace coop::printer_models {

// The hash of the model folder `name` on this machine, never 0; 0 when the folder or its <name>.obj is missing or the name
// is not a plain folder name. Hashed again only when a file's size or time changed since the last call.
uint64_t ModelHash(const std::wstring& name);

// Whether `name` can name a model folder: 1 to 95 UTF-8 bytes, no path separator, no "..", no control character.
bool IsPlainName(const std::wstring& name);

// UTF-8 and back, for the wire's name bytes.
std::string ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const char* s, size_t len);

}  // namespace coop::printer_models
