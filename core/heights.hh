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
//   - reverb: where a bus running a reverb effect (the aux returns) hands its output to its parent, a share
//     of its floor channels is carved into the height accumulators, energy-preserving (the floor keeps
//     sqrt(1 - share²));
//   - sky (weather, birds) and ambience (the ambient subtree): per voice, where the voice is mixed into its
//     bus. Wwise 2012 only instantiates "mixing" buses (effects, aux, HDR, positioning, channel config);
//     the game's ambient/weather buses have none of that and fold into master_hdr, so they never exist at
//     runtime. The voice's bank-side bus chain (sound → parents → output bus → parent buses) still names
//     them, and the nearest listed ancestor decides the tier. The carve works on the voice's speaker gains:
//     the floor gains of FL FR BL BR SL SR are scaled down and the same share × gain × PCM goes up;
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

	// A voice's dry path is about to be mixed into `mixBus` (after the object router had its say): may scale
	// `mix` (one AkAudioMix per input channel) and take the lifted share into the height accumulators.
	void OnVoiceMix(const void* pbi, const void* mixBus, const wwise::AkVPLState* state, wwise::AkAudioMix* mix);

	// End of a rendered buffer: decorrelates and returns this buffer's four height channels (planar, 1024
	// frames each), or nullptr when nothing is fed. Then StartFrame() clears them for the next buffer.
	const float* const* FinishFrame();
	void StartFrame();

	// Feature switched on and the stream has height channels.
	bool Active();
}
