// ue_wrap/engine/save_folder.h -- the game's save folder and its compressed save files.
//
// A save is a folder, <ProjectSaved>/SaveGames/<name>/: main.sav and autosave.sav (saveSlot_C, the
// world), persistent.sav (save_persistent_C, shared by both), main.meta and autosave.meta
// (save_metadata_C: mode, rules, starter map, area, version, dates), *.png previews, and the
// areas/, local/ and backups/ subfolders. Every file goes through the game's native
// CompressedSaveLoadLib, never GameplayStatics' slot calls, and the game's blueprints name each
// file by (slot, folder path, extension). Engine-wrapper layer (principle 7). Game thread.
// Implementation: src/ue_wrap/engine/save_folder.cpp.

#pragma once

#include <string>

namespace ue_wrap::save_folder {

// The folder of save `name`, as the game builds it (lib_C's "get save slot folder path":
// GetProjectSavedDirectory + "SaveGames/" + name), without a trailing separator. Empty while the
// native is unresolved.
std::wstring FolderPath(const std::wstring& name);

// CompressedSaveLoadLib::LoadGameCompressed(slot, folder, extension, 0): the USaveGame* read from
// <folder>/<slot><extension>, or null when the file is missing or unreadable.
void* LoadCompressed(const std::wstring& slot, const std::wstring& folder, const wchar_t* extension);

// CompressedSaveLoadLib::SaveGameCompressed(save, folder, slot, extension, 0): writes
// <folder>/<slot><extension>. False when the native is unresolved or reports failure.
bool SaveCompressed(void* save, const std::wstring& folder, const std::wstring& slot,
                    const wchar_t* extension);

// The folder a save keeps its areas' files in, as lib_C::createAreaSlot builds it (the save's
// folder path combined with "areas"): areas/<area>.sav holds the world of a save left inside an
// area.
std::wstring AreaFolder(const std::wstring& saveFolderPath);

// The area a save was left in: main.meta's `area`, empty for a save outside one (or one whose
// metadata cannot be read). `folderPath` is the save's folder path.
std::wstring AreaOf(const std::wstring& folderPath);

// The save a folder path belongs to: the component after "SaveGames" ("<...>/SaveGames/<name>" and
// "<...>/SaveGames/<name>/areas" both give <name>); empty for a path outside the saves.
std::wstring SaveNameOf(const wchar_t* folderPath);

// Where SaveGameCompressed's parameters sit in its frame, by position (the native names are not
// relied on): the object, the folder, the slot, the extension.
struct SaveFrame {
    void*   function = nullptr;
    int32_t object = -1;
    int32_t folder = -1;
    int32_t slot = -1;
    int32_t extension = -1;
};

// SaveGameCompressed's UFunction and frame; false (once logged) while the library is unresolved or the
// frame is not the expected shape.
bool ResolveSaveFrame(SaveFrame& out);

}  // namespace ue_wrap::save_folder
