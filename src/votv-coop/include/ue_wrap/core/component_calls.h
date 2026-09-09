// ue_wrap/core/component_calls.h -- GENERIC reflected component/widget calls.
//
// The six helpers are not desk-specific: each resolves its UFunction lazily from the PASSED
// object's own class and caches it, so any module driving a UTextBlock / UAudioComponent /
// UActorComponent / USceneComponent through reflection can use them.
// Game thread (reflected UFunction dispatch).

#pragma once

namespace ue_wrap::component_calls {

// UTextBlock::SetText(FText) -- param name InText/inText resolved once.
bool SetText(void* textBlock, const wchar_t* text);

// UAudioComponent::SetSound(USoundBase*).
bool SetSound(void* comp, void* sound);

// UActorComponent::Activate(bReset=true).
bool Activate(void* comp);

// UActorComponent::SetActive(bNewActive, bReset=true) -- the audio-loop switch.
bool SetActive(void* comp, bool value);

// USceneComponent::SetVisibility(bNewVisibility, bPropagateToChildren=false).
bool SetVisibility(void* comp, bool value);

// UAudioComponent::SetVolumeMultiplier(float). The caller owns any clamping
// semantics (e.g. the desk's FClamp(v/10, 0.1, 5)).
bool SetVolumeMultiplier(void* comp, float mult);

// Dispatch a parameterless UFunction on obj (fn already resolved by the caller).
bool CallParamless(void* obj, void* fn);

}  // namespace ue_wrap::component_calls
