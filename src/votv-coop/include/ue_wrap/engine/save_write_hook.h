// ue_wrap/engine/save_write_hook.h -- the seam on the game's save write: CompressedSaveLoadLib::SaveGameCompressed, the
// native every save file of a world goes through (saveSlot_C::saveToSlot's main.sav, autosave.sav and areas/<area>.sav,
// and the persistent, container and metadata saves beside them). The Blueprint funnel above it is called
// Blueprint-to-Blueprint, which dispatches through ProcessInternal and never reaches the ProcessEvent detour, so the
// native is the first place a save can be seen at all. It is patched through ue_wrap/core/ufunction_hook's pre hook, which
// reads the call's parameters off the bytecode and answers a refused call false. Two seams a caller can want:
//   * a GATE, asked before a write of a world file, which may cancel it;
//   * a WRITTEN notice, given after the engine reported the world's own container written.
// A world file is one of the world's saves: a saveSlot_C (the world itself), or a save_container_C, save_persistent_C or
// save_metadata_C (its companions). Everything else -- the keybinds and settings saves -- passes straight through and is
// reported to nobody. A world file is not always THE world: the save-slot menu writes regenerated and copied saveSlot_C
// objects of its own, and the join capture writes the live one to a scratch folder. The object and the folder are handed
// over so the caller can tell; this layer does not. Engine-wrapper layer (principle 7): no session, no role, no coop
// state. Both seams run on the thread that called SaveGameCompressed, the game thread for every save the game makes.

#pragma once

namespace ue_wrap::save_write_hook {

// One write of a world file. The strings are valid for the call only.
struct Write {
    void*          object;     // the USaveGame being written
    const wchar_t* folder;     // the folder it goes into: <ProjectSaved>/SaveGames/<name>[/areas]
    const wchar_t* slot;       // the file's name without its extension: "main", "autosave", an area, ...
    const wchar_t* extension;  // ".sav" or ".meta"
    bool           world;      // a saveSlot_C: the world itself, not one of its companion files
};

// A world file is about to be written. True lets the engine write it; false cancels it, and the
// engine call returns false -- "the save did not happen", which is what the Blueprint save flow
// shows the player. A gate that FAULTS cancels the write: a process that cannot decide does not write.
using Gate = bool (*)(const Write& write);

// The engine reported the world's own container (a saveSlot_C) written. Companion files are not
// announced: one save of the world writes the container once.
using Written = void (*)(const Write& write);

// Patch SaveGameCompressed. Idempotent, and cheap once it has answered for good. False while the
// library is not loaded yet (call again later); a function or frame that does not resolve is logged
// once and is final.
bool Install();

// One gate; null clears. Game thread.
void SetGate(Gate gate);

// Up to kMaxWritten written subscribers, each called in the order added. Adding one already added is a
// no-op; false when the table is full. Game thread.
inline constexpr int kMaxWritten = 4;
bool AddWritten(Written written);

}  // namespace ue_wrap::save_write_hook
