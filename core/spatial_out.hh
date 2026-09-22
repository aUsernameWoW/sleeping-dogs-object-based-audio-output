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

	// Starts the render thread. `channelMask` is the SPEAKER_* layout of the interleaved frames pushed;
	// `maxObjects` caps the dynamic objects requested (the endpoint may allow fewer).
	void Start(uint32_t sampleRate, uint32_t channelMask, uint32_t maxObjects);

	// True while the spatial stream is running. The sink hook must then push every buffer and silence XAudio2;
	// otherwise it leaves XAudio2 alone (spatial sound off, device lost, ...).
	bool IsActive();

	// Object slots the router may fill right now: 0 while the stream isn't running.
	uint32_t ObjectSlots();

	// Audio thread only. `interleaved` == nullptr pushes a silent bed; `objects` may be null.
	void Push(const float* interleaved, uint32_t frames, const ObjectBlock* objects);
}
