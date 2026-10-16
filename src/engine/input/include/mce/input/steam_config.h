#pragma once
// Steam Input configuration files from an input map (M-STEAM step 4). Steam reads a game's
// controllers as actions only after it knows them: the action manifest (game_actions_<appid>.vdf,
// next to the executable or uploaded on the app's Steamworks page) lists the action sets and their
// actions, and per controller type a default configuration binds them to the controller's inputs.
// Both are written here from the cooked map the player runs with, so they cannot drift from it.
//
// - One action set, "Game", with every action that has an authored name (the map's labels): a
//   Button is a Steam button, an Axis1D bound to a trigger an analog trigger, any other axis a
//   stick (joystick_move). Several maps' actions of the same name are one action.
// - The default configurations take the map's own gamepad bindings: face buttons, d-pad buttons,
//   shoulders, Back/Start, paddles, stick clicks, sticks and triggers. The Steam Deck's adds the
//   right trackpad to the action on the right stick. An Axis2D action read from d-pad buttons
//   (a d-pad "Move") stays with the stick it also reads: Steam binds a d-pad to buttons only.
//
// The text is Valve's KeyValues format. Nothing here needs the Steamworks SDK.

#include "mce/containers/array.h"
#include "mce/input/input_map.h"

namespace mce::input {

inline constexpr const char* kSteamActionSet = "Game";

enum class SteamController : u8 { Deck, Xbox, PlayStation, Switch };
inline constexpr u32 kSteamControllerCount = 4;

// The manifest's file name for an app ("game_actions_480.vdf") and a configuration's file name
// ("controller_neptune.vdf") and Steam's controller type ("controller_neptune").
void steam_manifest_file(u32 app_id, Array<char>& out);
const char* steam_controller_type(SteamController c);
const char* steam_controller_file(SteamController c);

// The action manifest, naming every default configuration. `title` is the game's name.
void steam_action_manifest(const InputMapSet& set, StringView title, Array<char>& out);
// One controller type's default configuration.
void steam_controller_config(const InputMapSet& set, SteamController c, StringView title, Array<char>& out);

}  // namespace mce::input
