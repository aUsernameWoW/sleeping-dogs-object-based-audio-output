#pragma once

#include <cstdint>

// Plays the game's final mix through ISpatialAudioClient: the channel mix as static bed objects, plus up to
// kMaxObjects dynamic objects taken out of the mix by the voice router (objects.cc).
//
// Wwise keeps rendering into its XAudio2 sink as before (XAudio2 stays the clock that paces Wwise); the sink
// hook copies every buffer here and silences XAudio2's copy while this stream is active. Both streams run on
// the same endpoint clock, so the ring's fill level only jitters; it's trimmed if it creeps up.
//
// Nothing here is Dolby-specific: object limits, bed layout and format come from whichever spatial sound
// format Windows has active (Dolby Atmos for home theater, DTS:X, Windows Sonic, ...).
namespace spatial
{
	constexpr uint32_t kMaxObjects = 32;
	constexpr uint32_t kBlockFrames = 1024; // one Wwise buffer
	constexpr uint32_t kHeights = 4;        // TFL TFR TBL TBR of a x.x.4 bed

	// One Wwise buffer worth of dynamic objects. Slots are stable: a slot keeps its Windows object for as long
	// as the router keeps putting sound in it.
	struct ObjectBlock
	{
		uint32_t mActiveMask = 0;
		float mPosition[kMaxObjects][3] = {}; // meters; Windows convention: +x right, +y up, +z behind
		float mSamples[kMaxObjects][kBlockFrames] = {};
	};

	// Whether the default render endpoint currently offers ISpatialAudioClient (a spatial sound format is on).
	// Blocks briefly; callable from any thread regardless of its COM apartment.
	bool IsAvailable();

	// Starts the render thread. `channelMask` is the SPEAKER_* layout of the interleaved frames pushed.
	// With `objects`, as many dynamic objects as the format allows (up to kMaxObjects) are reserved; how many
	// are used is the router's business and can change at runtime. With `heights`, the four top channels are
	// added to the bed whenever the format's native bed has them (Atmos: 7.1.4).
	void Start(uint32_t sampleRate, uint32_t channelMask, bool objects, bool heights);

	// True while the spatial stream is running. The sink hook must then push every buffer and silence XAudio2;
	// otherwise it leaves XAudio2 alone (spatial sound off, device lost, ...).
	bool IsActive();

	// Object slots the router may fill right now: 0 while the stream isn't running.
	uint32_t ObjectSlots();

	// The running stream has the four height bed channels.
	bool HeightsActive();

	// Audio thread only. `interleaved` == nullptr pushes a silent bed; `heights` (kHeights planar channels of
	// `frames` samples) and `objects` may be null.
	void Push(const float* interleaved, uint32_t frames, const float* const* heights, const ObjectBlock* objects);

	// For the menu/HUD, refreshed by the render thread about 10 times a second.
	struct Status
	{
		bool mActive = false;
		char mEndpoint[128] = {};
		char mBed[64] = {};
		uint32_t mHeights = 0;      // height bed channels on the stream (0 or kHeights)
		uint32_t mSlots = 0;        // dynamic objects reserved on the stream
		uint32_t mFormatMax = 0;    // what the spatial format allows
		uint32_t mSounding = 0;     // slots with sound (latest pass)
		uint32_t mHeld = 0;         // Windows objects held (latest pass)
		uint32_t mFill = 0;         // ring fill in frames (latency)
		uint64_t mUnderruns = 0;    // since the stream opened
		uint64_t mActivationFailures = 0;
		uint64_t mFoldedPasses = 0;
	};
	void GetStatus(Status& out);
}
