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

	// Objects are carved out before the bus mix, so they skip every insert effect on the voice's bus chain.
	// Parametric EQs are reproduced on the objects; for the rest (compressor, limiter...): 0 = ignore, 1 = such
	// voices stay in the bed (master bus excepted: a master limiter would rule out every object), 2 = master
	// bus included.
	std::atomic<int> mBusFx{ 1 };

	// Characters' audio entities sit at the feet; lift them this many meters towards head height in the
	// positions handed to Wwise (0 = off). Applied on the next position update of each character.
	std::atomic<float> mActorLift{ 1.5f };

	// --- Height bed (7.1.4): diffuse content derived from the floor channels, see core/heights.hh ---

	// Feed the four top bed channels (A/B switch; needs a spatial format whose bed has them, e.g. Atmos).
	std::atomic<bool> mHeights{ true };

	// Share (dB) of each source kind's floor output moved up. -60 or less = none.
	std::atomic<float> mHeightSky{ -3.0f };      // weather, birds: rain and thunder do come from above
	std::atomic<float> mHeightAmbience{ -6.0f }; // the rest of the ambient subtree (city, crowds, water...)
	std::atomic<float> mHeightReverb{ -6.0f };   // buses running a reverb effect (the aux returns)

	// Bed voices above the horizon are lifted by sin(elevation) × this (dB; 0 = the full sine, -60 = off).
	std::atomic<float> mHeightElevation{ 0.0f };

	// Decorrelation of the height channels (static; see height_dsp.hh): pre-delay in ms (the back pair gets
	// 4 ms more), high-pass in Hz (0 = off).
	float mHeightDelay = 8.0f;
	float mHeightHighPass = 200.0f;

	// Bus IDs (Wwise short IDs from Init.bnk) whose output is treated as sky / ambience when it enters its
	// parent. Reverb buses are recognized by their effects instead.
	static constexpr int kMaxBusIds = 8;
	uint32_t mSkyBuses[kMaxBusIds] = { 317282339, 352130103 }; // weather, birds
	int mSkyBusCount = 2;
	uint32_t mAmbienceBuses[kMaxBusIds] = { 77978275 }; // ambient
	int mAmbienceBusCount = 1;

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
	int mToggleHeightsKey = 0x76; // VK_F7
	int mToggleRainKey = 0x75;    // VK_F6: debug, forces the game's weather to rain / clear

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
