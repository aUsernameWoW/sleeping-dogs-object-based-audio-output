#pragma once

#include <cstdint>

// What the bank says about a sound, cached per sound ID: its bus chain (output bus, then the buses above it,
// nearest first). Wwise 2012 only instantiates "mixing" buses at runtime, so the AkVPL chain a voice mixes
// into (almost always master_hdr) says nothing about what the sound *is*; the bank-side chain still names
// gunshot, footsteps, weather, traffic... The height bed's sky/ambience tiers and the router's per-bus rules
// both read it. The walk (CAkParameterNodeBase::m_pParentNode / m_pBusOutputNode, as GetControlBus does) is
// done once per sound ID. The table is sized for the whole game's sound set: a 256-entry table filled in
// 2.5 minutes of play, after which every voice mix re-walked and re-logged its chain.
// Audio thread only.
namespace sounds
{
	constexpr int kMaxChain = 12;

	struct Info
	{
		uint32_t mID = 0;
		uint8_t mChainLen = 0;
		uint32_t mChain[kMaxChain] = {}; // output bus first, then its parents up to the root

		// Cached decisions and "logged once" flags of the users (heights.cc, objects.cc), kept here so they
		// are per sound rather than per voice.
		int8_t mHeightTier = -1; // heights' Tier, -1 = not classified yet
		bool mElevationLogged = false;
		bool mShapeLogged = false; // stereo / multi-position sighting logged
	};

	// The cached entry for `sound` (a CAkSoundBase*), walked on first sight. Null only for sound ID 0 or when
	// the table is full; callers then Walk() into a local Info and skip their once-only logging.
	Info* Lookup(const void* sound, uint32_t soundID);

	// Fills `out` from the bank-side hierarchy.
	void Walk(const void* sound, uint32_t soundID, Info& out);

	// Is a bus of the chain in `list`? `depth` (optional) receives the index of the nearest such bus.
	bool Listed(const Info& info, const uint32_t* list, int count, int* depth = nullptr);

	// Bus names recovered from Init.bnk (dictionary FNV-1); "?" when unknown. Logging only.
	const char* BusName(uint32_t id);

	// "gunshot>sfx>master_sfx>3995202064>..." (names where known, IDs otherwise); returns `buffer`.
	const char* FormatChain(const Info& info, char* buffer, size_t size);
}
