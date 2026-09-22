#pragma once

#include "spatial_out.hh"
#include "wwise.hh"

// Voice router: decides which Wwise voices leave the channel bed and become dynamic objects.
//
// Industry practice (Dolby's game guidelines, Wwise's own object pipeline): point sources that move go to
// objects, diffuse sound (ambience, reverb, wide spread) stays in the bed, and whatever exceeds the object
// budget folds into the bed. Here:
//   - only mono, single-position, point-like (little spread) 3D voices are candidates;
//   - a brand-new candidate grabs a free slot immediately, so its attack is an object from the first sample;
//   - once per frame the candidates are ranked by level (gain × signal RMS, current objects count double for
//     hysteresis) and the budget is redistributed;
//   - moving between bed and object crossfades over one buffer (21 ms) through Wwise's own gain ramp.
// All functions run on the Wwise audio thread.
namespace objects
{
	// The dry-path mix of one voice into its bus is about to happen. May capture the voice's samples into its
	// object slot and scale `mix` so the bed gets only the part that isn't an object.
	void OnDryMix(const void* cbx, const void* pbi, const void* mixBus, const wwise::AkVPLState* state, wwise::AkAudioMix* mix);

	// End of a rendered buffer: returns this buffer's objects (for spatial::Push) and decides the next buffer's.
	const spatial::ObjectBlock* FinishFrame();

	// After the block returned by FinishFrame has been pushed.
	void StartFrame();

	// For the voice log: the voice's current slot, or -1 when it's in the bed.
	int SlotOf(const void* pbi);

	// Periodic summary (promotions, demotions, voices tracked).
	void LogStats();
}
