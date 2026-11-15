// ue_wrap/core/class_load.h -- resolve a Blueprint class by its short name, and when this process has not loaded it,
// load it from the content by its object path.
//
// The game loads a Blueprint class on first use, so a peer can name a class this one never loaded (the host spawns a
// food subclass the client has not met). A name lookup (FindClass, ClassByName) answers null for it, and a spawn that
// needs the class is dropped. This is the one seam that turns that miss into a load: the lookup first, and only on a
// miss the synchronous load of `objectPath` (asset_load), once per class. A class it loaded is rooted (GcPin), since
// nothing else refers to a class a nameless load made, and the collector frees such a class before the next spawn
// that wants it. A failed load is remembered and not asked again for kRetryAfterMs, so a path that never resolves
// costs one load attempt, not one per spawn.
//
// Game thread only (the load goes through ProcessEvent).

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::class_load {

// How long a failed load is remembered before the same name is tried again.
constexpr uint64_t kRetryAfterMs = 15000;

using Lookup = void* (*)(const wchar_t* shortName);

// The class named `shortName` ("prop_food_mre_AB_C"): `lookup` first (null = reflection::FindClass), then, on a miss and
// when `objectPath` is a content path ("/Game/objects/prop_food_mre_AB.prop_food_mre_AB_C"), its load. A path that
// is not under /Game/, that holds a character outside [A-Za-z0-9_/.], or whose object name is not `shortName`, is
// refused without a load; a load whose object is not a class of that name is kept as a failure. Null when neither
// finds it. The returned class is one the lookup answers from now on (an index-backed lookup is caught up before this
// returns).
void* Resolve(const wchar_t* shortName, const wchar_t* objectPath, Lookup lookup = nullptr);

// The same for a class named by a stored soft path ("/Game/objects/x.x_C", with an optional ":sub" tail as a record writes it):
// the leaf is what follows the last dot. An inventory record carries its class this way, so no wire field is needed for it.
void* ResolveSoftPath(const std::wstring& softPath, Lookup lookup = nullptr);

// The object path of a loaded Blueprint class ("/Game/objects/x.x_C": its package, a dot, its name), or empty for a class
// that does not live in a /Game/ package (a native class every peer has) or whose outer cannot be read.
std::wstring ObjectPathOfClass(void* cls);

// Whether `path` is a content path Resolve will load: "/Game/" first, then only [A-Za-z0-9_/.], at most 200 characters.
bool IsLoadablePath(const std::wstring& path);

}  // namespace ue_wrap::class_load
