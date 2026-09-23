#pragma once

#include <atomic>
#include <string>

struct Config
{
	// Send the game's final 7.1 mix through ISpatialAudioClient (static bed objects) instead of XAudio2.
	// This alone makes the receiver switch to Dolby Atmos; falls back to XAudio2 if spatial audio is off.
	bool mSpatialBed = true;

	// --- Changeable at runtime (menu / hotkeys), hence atomic: read by the audio and render threads. ---

	// Take the loudest point-like 3D voices out of the bed and send them as dynamic objects (A/B switch).
	std::atomic<bool> mObjects{ true };

	// Upper bound on dynamic objects; the active spatial format may allow fewer (Atmos over HDMI: 20).
	std::atomic<int> mMaxObjects{ 20 };

	// Objects are placed on a sphere of this radius (meters) around the listener: Wwise has already applied
	// distance attenuation, only the direction is new information.
	std::atomic<float> mObjectDistance{ 2.0f };

	// The Wwise listener is the camera, so the player's own footsteps/foley sit ~3 m ahead of it. As dynamic
	// objects they become a sharp point in front of the viewer; in the bed they get the game's own close-range
	// spread (front + side pairs), which is what the original mix did.
	std::atomic<bool> mPlayerInBed{ true };

	// Objects are carved out before the bus mix, so they skip every insert effect on the voice's bus chain
	// (EQ, compressor, limiter...). 0 = ignore that, 1 = such voices stay in the bed (master bus excepted:
	// a master limiter would rule out every object), 2 = master bus included.
	std::atomic<int> mBusFx{ 1 };

	// HUD drawn over the game through ReShade (needs ReShade with add-on support).
	std::atomic<bool> mHud{ false };
	std::atomic<bool> mHudRadar{ true };
	std::atomic<bool> mHudMarkers{ true };
	std::atomic<bool> mHudLabels{ false };
	std::atomic<bool> mHudBedVoices{ true };  // also draw voices that stay in the bed
	std::atomic<float> mHudFov{ 60.0f };      // vertical camera FOV (degrees) for the on-screen markers
	std::atomic<float> mHudRadarRange{ 60.0f }; // meters at the radar's edge

	// Virtual-key codes.
	int mToggleObjectsKey = 0x78; // VK_F9
	int mToggleHudKey = 0x77;     // VK_F8

	// Write SDAtmos.log next to the .asi.
	bool mLogging = true;

	// Every few seconds, log the 3D voices Wwise is mixing (position, gains, bed/object).
	bool mVoiceLog = true;
};

extern Config gConfig;

namespace config
{
	// Loads <dir>\SDAtmos.ini, writing a default one if it doesn't exist.
	void Load(const std::wstring& dir);

	// Writes the runtime-changeable values back (keys are updated in place, comments survive).
	void Save();
}
