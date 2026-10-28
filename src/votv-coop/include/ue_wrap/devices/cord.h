// ue_wrap/devices/cord.h -- reflected access to VOTV's cord (cord_C), its socket (cordSocket_C) and the corded
// prop that owns a cord (prop_corded_C). Reads of the three classes' relationship fields and single calls of the
// cord's own verbs; no network or gameplay logic (principle 7). Game thread.
#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace ue_wrap::cord {

bool EnsureResolved();
bool IsCord(void* actor);
bool IsSocket(void* actor);
bool IsCordedProp(void* actor);

// The corded prop's cord (its obj_cord), the cord's prop (its parent) and its socket, the socket's cord. Null when
// the argument is not of the class or the field is empty.
void* CordOf(void* prop);
void* PropOf(void* cord);
void* SocketOf(void* cord);
void* CordIn(void* socket);

// The world location of the cord's head, the end the player carries. False when it does not read.
bool HeadLocation(void* cord, ue_wrap::FVector& out);

// The game's own verbs on a cord: connect(socket, false), whose occupied-socket test unplugs THIS cord and leaves
// the occupant, and disconnect(). True when the call ran.
bool Connect(void* cord, void* socket);
bool Disconnect(void* cord);

// Up to `cap` of the running world's cordSocket_C instances, in no particular order (the object index); returns the
// count. A caller that keys them reads each one's key with prop::GetActorSaveKeyString.
int32_t Sockets(void** out, int32_t cap);

// The socket that the UChildActorComponent named `childName` on `owner` spawned. Null when `owner` has no such
// component: the name is a peer's word, so only a property that is an object property holding a live
// ChildActorComponent is read.
void* ChildSocket(void* owner, const std::wstring& childName);

// A socket that is a child actor of a prop: that prop's key and the component's name. True only when the socket's
// parent actor is a prop with a key.
bool ChildSocketName(void* socket, std::wstring& ownerKey, std::wstring& childName);

}  // namespace ue_wrap::cord
