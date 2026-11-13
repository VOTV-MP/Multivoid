// coop/player/leave_in_event.h -- the pause menu's Main Menu and Exit work during an event in a live session.
//
// The pause menu (ui_menu_C) asks lib_C::getEvent before its Main Menu and Exit buttons act, and on true they do
// nothing; Save is greyed out and the game does not pause either. getEvent is `activeEvents > 0` (or the camera far
// outside the map), and a running event is anything that called lib_C::setEvent(true): a creature, a story event,
// a video camera recording (coop/player/vidcam_hint). In single player an event ends in minutes. In a session the
// Main Menu button is how a player leaves, and a client's count holds the host world's events too, so a client could
// not leave until every one of them ended.
//
// While a session runs in a gameplay world, the getEvent the two buttons ask (the menu's ubergraph, the only frame of
// the menu that asks it outside enterPause) is refused at the script-body gate with its out parameter false, so the
// buttons act as they do with no event. The travel the Main Menu button makes is the pause menu's own quit, which
// coop/player/run_end_travel lets through and ends the session at. Save, the blur and the pause keep the game's
// answer. Solo, the watch lets every call run. Gameplay layer: the engine only through the gate and reflection.

#pragma once

namespace coop::net { class Session; }

namespace coop::player::leave_in_event {

// Cache the session, register the watch on lib_C::getEvent, and bind the menu's and the library's classes, which are
// new objects with each level: a class is looked up when the class set moves and through the load window after it,
// and not at all while it stays bound (two listing-id hash lookups a tick). Idempotent. Game thread.
void Install(coop::net::Session* session);

}  // namespace coop::player::leave_in_event
