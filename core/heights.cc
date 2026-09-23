#include "heights.hh"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.hh"
#include "log.hh"
#include "spatial_out.hh"

namespace heights
{
	using namespace wwise;
	using namespace height_dsp;
	using spatial::kBlockFrames;

	enum class Tier : uint8_t
	{
		None,
		Sky,
		Ambience,
		Reverb,
	};

	static uint32_t gSampleRate = 0;
	static DecorrelatorBank gBank;
	static float gMix[kChannels][kBlockFrames];
	static float* const gMixPtr[kChannels] = { gMix[0], gMix[1], gMix[2], gMix[3] };
	static bool gFed = false;      // something was carved this buffer
	static bool gTail = false;     // the decorrelators still ring from an earlier buffer

	// Enable ramp: 0..1, moves by at most one full step per buffer, so switching the feature (F7) or the
	// stream coming and going crossfades the floor/height split over one buffer rather than clicking.
	static float gMaster = 0.0f;
	static float gMasterNext = 0.0f;

	struct Stats
	{
		uint32_t mBuffers[4] = {}; // carved bus transfers per tier this period
		float mPeak[kChannels] = {};
	};
	static Stats gStats;
	static ULONGLONG gLastStats = 0;

	// Buses seen, so each is logged once with its parent and tier (bus pointers recycle; the ID is what
	// matters). Every bus is logged, not just the carved ones: the log then shows the runtime bus tree, which
	// is how a wrong bus ID in the ini gets found.
	constexpr int kMaxSeen = 256;
	static uint32_t gSeen[kMaxSeen];
	static int gSeenCount = 0;

	// Names recovered from Init.bnk by dictionary FNV-1 (logging only; the IDs drive the routing).
	static const char* BusName(uint32_t id)
	{
		switch (id) {
		case 77978275: return "ambient";
		case 317282339: return "weather";
		case 186852181: return "thunder";
		case 1537061107: return "wind";
		case 2043403999: return "rain";
		case 352130103: return "birds";
		case 3888786832: return "city";
		case 3463109076: return "traffic";
		case 2458178259: return "water_amb";
		case 1930490682: return "boat_amb";
		case 689383231: return "crowd_market";
		case 1587111019: return "crowd_club";
		case 1854869158: return "crowd_restaurant";
		case 1830469890: return "interior_rain";
		case 3462011115: return "master_sfx";
		case 3946296192: return "master_aux";
		case 3995202064: return "master_hdr";
		case 3627036714: return "master_dialog";
		case 1900298039: return "master_music";
		case 393239870: return "sfx";
		case 2385628198: return "footsteps";
		case 1287408361: return "gunshot";
		case 3444197610: return "root";
		case 0: return "-";
		default: return "?";
		}
	}

	static const char* TierName(Tier tier)
	{
		switch (tier) {
		case Tier::Sky: return "sky";
		case Tier::Ambience: return "ambience";
		case Tier::Reverb: return "reverb";
		default: return "none";
		}
	}

	static bool Listed(uint32_t id, const uint32_t* list, int count)
	{
		for (int i = 0; i < count; ++i) {
			if (list[i] == id) {
				return true;
			}
		}
		return false;
	}

	// The bus itself decides (not its ancestors): the ambient bus carves once when its whole subtree passes
	// through it, a weather bus carves its own share first and the remainder is carved again as ambience.
	static Tier Classify(const void* vpl, uint32_t busID)
	{
		if (Listed(busID, gConfig.mSkyBuses, gConfig.mSkyBusCount)) {
			return Tier::Sky;
		}
		if (Listed(busID, gConfig.mAmbienceBuses, gConfig.mAmbienceBusCount)) {
			return Tier::Ambience;
		}
		// What CAkVPLMixBusNode::ProcessAllFX would have run on this bus's output.
		const bool bypassAll = At<uint8_t>(vpl, vpl::kBypassAllFx) & 1;
		for (uint32_t i = 0; i < vpl::kFxSlots && !bypassAll; ++i) {
			const uint8_t* slot = static_cast<const uint8_t*>(vpl) + vpl::kFx + i * bus_fx::kStride;
			if (At<void*>(slot, bus_fx::kEffect) && !(At<uint8_t>(slot, bus_fx::kFlags) & 1) && IsReverbFx(At<uint32_t>(slot, bus_fx::kID))) {
				return Tier::Reverb;
			}
		}
		return Tier::None;
	}

	static float ShareOf(Tier tier)
	{
		switch (tier) {
		case Tier::Sky: return DbToGain(gConfig.mHeightSky.load(std::memory_order_relaxed));
		case Tier::Ambience: return DbToGain(gConfig.mHeightAmbience.load(std::memory_order_relaxed));
		case Tier::Reverb: return DbToGain(gConfig.mHeightReverb.load(std::memory_order_relaxed));
		default: return 0.0f;
		}
	}

	void Init(uint32_t sampleRate)
	{
		gSampleRate = sampleRate;
		const float delay = gConfig.mHeightDelay;
		gBank.Setup(sampleRate, delay, delay + 4.0f, gConfig.mHeightHighPass);
		std::memset(gMix, 0, sizeof(gMix));
		LOG("heights: %u Hz, pre-delay %.1f/%.1f ms front/back, high-pass %.0f Hz, shares sky %.1f dB, ambience %.1f dB, "
			"reverb %.1f dB, %d sky + %d ambience bus IDs",
			sampleRate, delay, delay + 4.0f, gConfig.mHeightHighPass, gConfig.mHeightSky.load(), gConfig.mHeightAmbience.load(),
			gConfig.mHeightReverb.load(), gConfig.mSkyBusCount, gConfig.mAmbienceBusCount);
	}

	bool Active()
	{
		return gConfig.mHeights.load(std::memory_order_relaxed) && spatial::HeightsActive();
	}

	void OnBusTransfer(const void* vpl, AkAudioBufferBus* buffer, float downstream)
	{
		if (!gSampleRate || (gMaster <= 0.0f && gMasterNext <= 0.0f) || !buffer->buffer.pData || !buffer->buffer.uValidFrames) {
			return;
		}
		const uint32_t busID = At<uint32_t>(vpl, vpl::kBusID);
		const Tier tier = Classify(vpl, busID);
		if (!Listed(busID, gSeen, gSeenCount)) {
			if (gSeenCount < kMaxSeen) {
				gSeen[gSeenCount++] = busID;
			}
			const void* parent = At<void*>(vpl, vpl::kParent);
			const uint32_t parentID = parent ? At<uint32_t>(parent, vpl::kBusID) : 0;
			LOG("heights: bus %u (%s) -> %s%u (%s), mask 0x%X: %s%s", busID, BusName(busID), parent ? "" : "final mix ",
				parentID, BusName(parentID), buffer->buffer.uChannelMask, TierName(tier), tier == Tier::None ? "" : " tier");
		}
		if (tier == Tier::None) {
			return;
		}
		const float share = ShareOf(tier);
		if (share <= 0.0f) {
			return;
		}
		FloorMap map;
		if (!BuildFloorMap(buffer->buffer.uChannelMask, map)) {
			return;
		}

		// Same volume ramp CAkMixer::Mix applies, times the chain to the output, times the share and the
		// enable ramp; the floor scale needs only the last two (Wwise applies its own ramp afterwards).
		const uint32_t frames = std::min<uint32_t>(buffer->buffer.uMaxFrames, kBlockFrames);
		const uint32_t valid = std::min<uint32_t>(buffer->buffer.uValidFrames, frames);
		float gain[kBlockFrames];
		float keep[kBlockFrames];
		const float v0 = buffer->fPreviousVolume;
		const float v1 = buffer->fNextVolume;
		const float step = 1.0f / static_cast<float>(frames);
		for (uint32_t i = 0; i < valid; ++i) {
			const float t = i * step;
			const float master = gMaster + (gMasterNext - gMaster) * t;
			keep[i] = share * master;
			gain[i] = (v0 + (v1 - v0) * t) * downstream * keep[i];
		}
		float* floor[8];
		for (int c = 0; c < map.mChannels && c < 8; ++c) {
			floor[c] = static_cast<float*>(buffer->buffer.pData) + static_cast<size_t>(c) * buffer->buffer.uMaxFrames;
		}
		Carve(map, floor, valid, gain, keep, gMixPtr);
		gFed = true;
		++gStats.mBuffers[static_cast<int>(tier)];
	}

	static void LogStats()
	{
		char peaks[64];
		snprintf(peaks, sizeof(peaks), "%.0f %.0f %.0f %.0f",
			gStats.mPeak[0] > 0.0f ? 20.0f * std::log10(gStats.mPeak[0]) : -99.0f,
			gStats.mPeak[1] > 0.0f ? 20.0f * std::log10(gStats.mPeak[1]) : -99.0f,
			gStats.mPeak[2] > 0.0f ? 20.0f * std::log10(gStats.mPeak[2]) : -99.0f,
			gStats.mPeak[3] > 0.0f ? 20.0f * std::log10(gStats.mPeak[3]) : -99.0f);
		LOG("heights: %s; bus transfers carved: %u sky, %u ambience, %u reverb; peak dBFS TFL TFR TBL TBR: %s",
			Active() ? "on" : gConfig.mHeights.load() ? "on but the stream has no height channels" : "off",
			gStats.mBuffers[static_cast<int>(Tier::Sky)], gStats.mBuffers[static_cast<int>(Tier::Ambience)],
			gStats.mBuffers[static_cast<int>(Tier::Reverb)], peaks);
		gStats = {};
	}

	const float* const* FinishFrame()
	{
		gMaster = gMasterNext;
		gMasterNext = Active() ? 1.0f : 0.0f;

		const ULONGLONG now = GetTickCount64();
		if (now - gLastStats >= 10000) {
			if (gLastStats) {
				LogStats();
			}
			gLastStats = now;
		}

		if (!gFed && !gTail) {
			return nullptr;
		}
		gBank.Process(gMixPtr, kBlockFrames);
		float peak = 0.0f;
		for (int h = 0; h < kChannels; ++h) {
			float channelPeak = 0.0f;
			for (uint32_t i = 0; i < kBlockFrames; ++i) {
				channelPeak = std::max(channelPeak, std::fabs(gMix[h][i]));
			}
			gStats.mPeak[h] = std::max(gStats.mPeak[h], channelPeak);
			peak = std::max(peak, channelPeak);
		}
		// Keep flushing the delay lines for a while after the last input; below -90 dBFS nobody hears the rest.
		gTail = gFed || peak > 3e-5f;
		return gMixPtr;
	}

	void StartFrame()
	{
		std::memset(gMix, 0, sizeof(gMix));
		gFed = false;
	}
}
