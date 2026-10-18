// ue_wrap/devices/cord.h -- reflected access to VOTV's cord, socket and corded prop.
#pragma once

#include <string>

namespace ue_wrap::cord {

bool EnsureResolved();
bool IsCord(void* actor);
bool IsSocket(void* actor);
bool IsCordedProp(void* actor);
std::wstring Key(void* actor);
void* SocketOf(void* prop);
void* CordOf(void* prop);
bool ApplyLink(void* prop, void* socket);
bool ApplyUnplug(void* prop);

}  // namespace ue_wrap::cord
