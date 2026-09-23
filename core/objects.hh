#pragma once

#include "spatial_out.hh"
#include "telemetry.hh"
#include "wwise.hh"

// Voice router: decides which Wwise voices leave the channel bed and become dynamic objects.
//
// Industry practice (Dolby's game guidelines, Wwise's own object pipeline): point sources that move go to
// objects, diffuse sound (ambience, reverb, wide spread) stays in the bed, and whatever exceeds the object
// budget folds into the bed. Here:
//   - only mono, single-position, point-like (little spread) 3D voices are candidates;
//   - the player's own voices (the listener is the camera, so they sit ~3 m ahead of it) stay in the bed, where
//     the game's close-range spread makes them enveloping rather than a point in front (PlayerInBed);
//   - a brand-new candidate grabs a free slot immediately, so its attack is an object from the first sample;
//   - once per frame the candidates are ranked by level (gain × signal RMS, current objects count double for
//     hysteresis) and the budget is redistributed;
//   - moving between bed and object crossfades over one buffer (21 ms) through Wwise's own gain ramp;
//   - turning objects off (A/B) or lowering the limit crossfades the affected objects back first.
// All functions run on the Wwise audio thread.
namespace objects
{
	// The dry-path mix of one voice into its bus is about to happen. May capture the voice's samples into its
	// object slot and scale `mix` so the bed gets only the part that isn't an object. Always fills `report`
	// (for the HUD) and returns false if the voice has no position to report.
	bool OnDryMix(const void* cbx, const void* pbi, const void* mixBus, const wwise::AkVPLState* state,
		wwise::AkAudioMix* mix, telemetry::Voice& report);

	// End of a rendered buffer: returns this buffer's objects (for spatial::Push) and decides the next buffer's.
	const spatial::ObjectBlock* FinishFrame();

	// After the block returned by FinishFrame has been pushed.
	void StartFrame();

	// Slots the router may fill in the current buffer.
	uint32_t Target();

	// For the voice log: the voice's current slot, or -1 when it's in the bed.
	int SlotOf(const void* pbi);

	// Periodic summary (promotions, demotions, voices tracked).
	void LogStats();
}
