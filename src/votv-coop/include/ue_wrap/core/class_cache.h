// ue_wrap/core/class_cache.h -- the loaded class named X, held by slot and serial for a caller that asks about the same few
// names on every call (a judge's base class). A dead read asks the class index again (object_index::ClassByName); a live one
// costs a compare. A class not loaded yet stays unresolved and is asked at the next call. Game thread.
#pragma once

namespace ue_wrap::class_cache {

// The loaded class named `name` (compared without case), or null when none is loaded.
void* ByName(const wchar_t* name);

}  // namespace ue_wrap::class_cache
