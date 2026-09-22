#pragma once

#include <Windows.h>

// In-game controls and visual debugging:
//   - hotkeys (own polling thread, work without ReShade): objects on/off for A/B listening, HUD on/off;
//   - with ReShade: an "SDAtmos" tab in the ReShade menu (settings, stream status, voice table) and a HUD
//     drawn over the game: a radar of every positioned voice (bed vs object, level, elevation) and markers
//     at each voice's direction projected onto the screen (the Wwise listener is the camera).
namespace overlay
{
	void Install(HMODULE self);
}
