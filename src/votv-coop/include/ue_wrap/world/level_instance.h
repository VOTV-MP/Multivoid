// ue_wrap/world/level_instance.h -- another map loaded INTO the running world: ULevelStreamingDynamic's
// LoadLevelInstance, at an offset of our choosing, and its unload. The map's actors begin play when its level is
// added, as they would in a world of their own, and are destroyed with it. Engine-wrapper layer: reflection calls
// only, no coop state. Game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <string>

namespace ue_wrap::level_instance {

// LevelStreamingDynamic::LoadLevelInstance(world context, map, location, no rotation): the streaming level, or null
// when the call does not run or the package is not found. `map` is the map's short name (sl_backroomsObsolete); the
// engine finds its package.
void* Load(void* worldContext, const std::wstring& map, const FVector& at);

// The streaming level's loaded ULevel once it is loaded and shown, else null.
void* LoadedLevel(void* streaming);

// The streaming level's state in a few words (loaded, visible, the level), for a log line.
std::string DescribeState(void* streaming);

// Unload: the level is hidden and removed, and its actors end their play. True when the calls ran.
bool Unload(void* streaming);

// UGameplayStatics::DeactivateReverbEffect(context, NAME_None): the reverb a level's script turned on with no tag.
bool DeactivateUntaggedReverb(void* worldContext);

}  // namespace ue_wrap::level_instance
