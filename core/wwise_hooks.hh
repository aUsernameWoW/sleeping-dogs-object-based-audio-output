#pragma once

#include <cstdint>

namespace wwise
{
	// Finds Wwise's sink and voice pipeline in the game exe and hooks them. Must run before the game
	// initializes Wwise (i.e. from DllMain). Logs what was found; missing pieces disable their feature.
	void Install();

	// The master bus (CAkVPLFinalMixNode, a CAkBusFX) of the output device `deviceID` (AkVPL::m_uDevice), or
	// null if CAkOutputMgr::m_Devices wasn't found. Audio thread.
	const void* FinalMixOf(uint64_t deviceID);

	// Plugin name for logging ("ParametricEQ", "McDSP ML1", ...) from the IDs the game registers.
	const char* PluginName(uint32_t pluginID);
}
