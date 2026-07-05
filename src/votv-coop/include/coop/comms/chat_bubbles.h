// coop/chat_bubbles.h -- MTA/SAMP-style overhead chat bubbles (12g, 2026-07-05).
//
// Gameplay layer (principle 7), pure data like chat_feed: chat_sync feeds the
// LAST chat message per sender slot in here (game thread, right next to its
// PushChat calls); coop::nameplate's Update() reads the current bubble + fade
// for each slot into the Plate snapshot; ui::hud draws it above the nameplate.
// A bubble rides the nameplate anchor BY DESIGN: a peer that hid its plate
// (v94 pref) shows no bubble either -- the whole overhead unit is one privacy
// surface. Self never renders (you have no puppet of yourself).
//
// Store is mutex-guarded (writers + the Update reader are all game-thread
// today; the lock keeps the module correct if a reader ever moves).

#pragma once

#include <cstdint>

namespace coop::chat_bubbles {

// Wire chat text is <=203 bytes (ChatMessagePayload); +NUL headroom.
inline constexpr int kMaxBubbleBytes = 208;

// SAMP-feel timing: hold ~8 s, fade the last ~0.7 s.
inline constexpr uint64_t kHoldMs = 8000;
inline constexpr uint64_t kFadeMs = 700;

// A player chat line arrived (local echo or wire). utf8Text is the MESSAGE ONLY
// (no nick prefix -- the plate already names the speaker). Replaces the slot's
// previous bubble (last-message-wins, the SAMP shape). Game thread.
void OnChatLine(int slot, const char* utf8Text);

// Copy slot's current bubble into out[kMaxBubbleBytes] and return the fade
// alpha 0..1 (0 = no bubble; out untouched). Called by nameplate::Update per
// visible plate (game thread).
float BubbleForSlot(int slot, char* out);

// Lifecycle: session start clears all (chat_sync Install seam); a disconnected
// slot's bubble must not survive onto a reused slot.
void ResetSlots();
void OnSlotDisconnected(int slot);

}  // namespace coop::chat_bubbles
