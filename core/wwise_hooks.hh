#pragma once

namespace wwise
{
	// Finds Wwise's sink and voice pipeline in the game exe and hooks them. Must run before the game
	// initializes Wwise (i.e. from DllMain). Logs what was found; missing pieces disable their feature.
	void Install();
}
