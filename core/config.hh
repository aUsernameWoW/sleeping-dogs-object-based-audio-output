#pragma once

#include <string>

struct Config
{
	// Send the game's final 7.1 mix through ISpatialAudioClient (static bed objects) instead of XAudio2.
	// This alone makes the receiver switch to Dolby Atmos; falls back to XAudio2 if spatial audio is off.
	bool mSpatialBed = true;

	// Take the loudest point-like 3D voices out of the bed and send them as dynamic objects.
	bool mObjects = true;

	// Upper bound on dynamic objects; the active spatial format may allow fewer (Atmos over HDMI: 20).
	int mMaxObjects = 20;

	// Objects are placed on a sphere of this radius (meters) around the listener: Wwise has already applied
	// distance attenuation, only the direction is new information.
	float mObjectDistance = 2.0f;

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
}
