// ue_wrap/devices/security_camera.h -- the security camera class's engine access: the base prop_camera_bad_C
// and its descendants (the store's prop_camera_good_C, the cursed camera), the two bits a player and the
// game's own events write (`active`: the LED, the click, the laptop's camera list and capture gate;
// `isLight`: the spotlight, flipped from the laptop's camera panel), their refresh verbs, and the laptop
// widget's selected camera and button repaint. Principle-7 engine-wrapper layer: no network, no coop state.
//
// Resolved lazily like ue_wrap/devices/appliance: the base class by name through the object index once it
// loads, the two bits' offsets and masks once from the class (a BP bool may share its byte with its
// siblings, so each carries a mask), the verbs per instance through the dispatch cache. A camera is an
// Aprop_C, so its identity is the prop's own Key. Game thread only.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::security_camera {

// The one action a camera offers a player (its getActionOptions lists only this): actionOptionIndex with it
// negates `active` and runs updState().
inline constexpr uint8_t kUseAction = 8;

// Resolve the base class and the two bits. True once both bits resolved; a class not loaded yet costs one
// object-index lookup. Idempotent. Game thread.
bool EnsureResolved();

// A prop_camera_bad_C or any descendant, by class alone: the scan hub's class-pure predicate (the hub keeps
// one verdict per class). False until resolved.
bool IsCameraClass(void* obj);

// A camera of the cursed class (its night roll writes `active` in its own tick). False until resolved.
bool IsCursed(void* obj);

// The camera's prop Key, or "" when `obj` is no camera, its key is empty or None, or it is the key its class
// carries by default (the cursed class's fixed "cursedcam": a copy that never took a key of its own, which no
// per-key lane can name).
std::wstring KeyOf(void* cam);

// A camera a per-key lane can address: IsCameraClass and a KeyOf that is not empty.
bool IsCamera(void* obj);

// The two bits. False when the camera or the bit did not resolve; `on` is left untouched then.
bool ReadActive(void* cam, bool& on);
bool ReadLight(void* cam, bool& on);
bool WriteActive(void* cam, bool on);
bool WriteLight(void* cam, bool on);

// upd(): the LED template from `active` and the spotlight from `isLight`. updState(): upd() and, when the
// camera is on, the button click. False when the verb did not resolve or the call did not run.
bool CallUpd(void* cam);
bool CallUpdState(void* cam);

// The laptop interface widget's selected camera (ui_laptop_C::cam), or null; live or null.
void* LaptopCamOf(void* widget);

// The widget's updCam(): the camera panel's buttons from the selected camera's bits.
bool CallUpdCam(void* widget);

// updCam() on every laptop widget that has `cam` selected. The number repainted.
int RefreshLaptopsShowing(void* cam);

// [dev] The drills' hands on the laptop's camera panel: the interface widget this world holds (null before it
// is built), the camera it has selected (setCam's own write of `cam`, without the camera slot it also builds),
// and its camera-light button's click, dispatched as the button's own event.
void* LaptopWidget();
bool SetLaptopCam(void* widget, void* cam);
bool CallLaptopLightClick(void* widget);

}  // namespace ue_wrap::security_camera
