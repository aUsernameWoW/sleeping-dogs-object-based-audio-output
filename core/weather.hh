#pragma once

// Debug helper: force rain on/off, so the weather tier of the height bed can be heard without waiting for
// the game's random weather (the user played an hour without a drop). Drives the game's own
// UFG::TimeOfDayManager the way its script atomics weather_set_amount / weather_lock do.
namespace weather
{
	// Finds the game's weather manager through the weather_set_amount atomic (pattern-scanned). Logs.
	void Install();

	// Any thread. Rain on: state/target 2.0 (full rain) and the random weather interval locked; rain off:
	// 0.0 and unlocked. Returns the new state, or false if the manager wasn't found.
	bool ToggleRain();
	bool IsRaining();
}
