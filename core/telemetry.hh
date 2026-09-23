#pragma once

#include <cstdint>

// What the audio thread knows about each voice, published once per Wwise buffer for the HUD/menu (render
// thread). The audio thread never waits: if the reader holds the lock, that buffer's snapshot is skipped.
namespace telemetry
{
	// Why a positioned voice is (or isn't) an object.
	enum class Reason : uint8_t
	{
		Object,        // is a dynamic object (or crossfading into/out of one)
		Candidate,     // qualifies, but no slot / ranked too low / holding its role
		Spread,        // Wwise spreads it over many speakers: an area sound, bed
		Quiet,         // below the level worth an object
		NotMono,       // stereo or more channels
		MultiPosition, // several emitter positions (one sound, many places)
		Player,        // the local player's own sounds: kept in the bed like the original mix
		BusFx,         // its bus chain has insert effects an object would skip
		BedRule,       // its bank-side bus chain is listed in BedBuses
	};
	constexpr int kReasonCount = 9;

	struct Voice
	{
		float mTheta;   // radians, 0 = camera forward, positive = right
		float mPhi;     // radians, positive = up
		float mDistance; // meters
		float mLevel;   // gain × downstream × RMS (linear, ~dBFS reference)
		uint32_t mSoundID;
		int8_t mSlot;   // object slot, -1 = bed
		Reason mReason;
	};

	constexpr uint32_t kMaxVoices = 96;

	struct Frame
	{
		uint64_t mFrame = 0;      // Wwise buffer counter
		uint32_t mVoiceCount = 0; // positioned voices in mVoices
		uint32_t mUnpositioned = 0; // 2D / no ray: not drawable
		uint32_t mDry = 0;
		uint32_t mObjectsTarget = 0; // slots the router may use
		Voice mVoices[kMaxVoices];
	};

	// Audio thread: collect this buffer's voices, then publish at buffer end.
	void Add(const Voice& voice);
	void AddUnpositioned();
	void Publish(uint64_t frame, uint32_t objectsTarget);

	// Render thread: copies the latest published frame. False if nothing was published yet.
	bool Read(Frame& out);
}
