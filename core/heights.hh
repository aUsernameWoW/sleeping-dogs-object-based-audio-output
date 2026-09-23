#pragma once

#include <cstdint>

#include "height_dsp.hh"
#include "wwise.hh"

// The height bed: the four top channels of a 7.1.4 static bed (TFL TFR TBL TBR), fed with the diffuse part
// of the mix. Dolby's game guidance puts ambience and reverb in the bed, and height-channel practice (Pro
// Logic IIz onwards, the DTS height upmixer, Atmos mixing guides) sends exactly that kind of material
// overhead: rain, wind, thunder, city ambience, reverb returns. The game itself never produced height
// content (7.1 only), so it is derived here:
//
//   - at the point where a bus hands its output to its parent (bus→bus or bus→final mix), buses of three
//     kinds carve a share of their floor channels into the height accumulators, energy-preserving (the
//     floor keeps sqrt(1 - share²)): "sky" buses (weather, birds: the loudest share), "ambience" buses (the
//     ambient subtree: a smaller share of what's left) and reverb buses (any bus running a reverb effect);
//   - at the end of the buffer the accumulators are decorrelated (Haas pre-delay + all-passes + high-pass,
//     see height_dsp.hh) and pushed with the bed.
//
// Bus IDs come from the game's Init.bnk (names recovered by dictionary FNV-1: "ambient" 77978275, "weather"
// 317282339, "birds" 352130103); the ini lists them so other games/builds could be configured.
// All functions run on the Wwise audio thread.
namespace heights
{
	// Once the engine rate is known (sink init), before the first frame.
	void Init(uint32_t sampleRate);

	// A bus is about to be mixed into its parent: `vpl` is the source AkVPL, `buffer` its output (modified in
	// place: the floor loses what goes up), `downstream` the gain from the parent to the device output.
	void OnBusTransfer(const void* vpl, wwise::AkAudioBufferBus* buffer, float downstream);

	// End of a rendered buffer: decorrelates and returns this buffer's four height channels (planar, 1024
	// frames each), or nullptr when nothing is fed. Then StartFrame() clears them for the next buffer.
	const float* const* FinishFrame();
	void StartFrame();

	// Feature switched on and the stream has height channels.
	bool Active();
}
