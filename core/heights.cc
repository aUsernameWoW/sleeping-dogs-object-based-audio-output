#include "heights.hh"

#include <Windows.h>

#include <algorithm>
#include <bit>
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
		uint32_t mVoices[4] = {};  // carved voice mixes per tier this period
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

	// Bus-level: only reverb. Sky/ambience are per voice (see the header): those buses aren't mixing buses in
	// this game and never transfer.
	static Tier Classify(const void* vpl, uint32_t)
	{
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
		LOG("heights: %s; carved %u reverb bus transfers, %u sky + %u ambience voice mixes; peak dBFS TFL TFR TBL TBR: %s",
			Active() ? "on" : gConfig.mHeights.load() ? "on but the stream has no height channels" : "off",
			gStats.mBuffers[static_cast<int>(Tier::Reverb)], gStats.mVoices[static_cast<int>(Tier::Sky)],
			gStats.mVoices[static_cast<int>(Tier::Ambience)], peaks);
		gStats = {};
	}

	// ---- Per-voice tiers from the bank-side bus chain ----

	// Sound ID → tier, so the node chain is walked once per sound (and logged once).
	struct SoundTier
	{
		uint32_t mSoundID;
		Tier mTier;
	};
	constexpr int kSoundTable = 512; // open addressing, never full: the game has far fewer distinct sounds at once
	static SoundTier gSoundTiers[kSoundTable];
	static int gSoundTierCount = 0;

	static Tier ClassifySound(const void* sound, uint32_t soundID)
	{
		// CAkParameterNodeBase::GetControlBus: the first output bus up the parent chain.
		const void* bus = nullptr;
		const void* node = sound;
		for (int depth = 0; node && depth < 32 && !bus; ++depth) {
			bus = At<void*>(node, node::kBusOutputNode);
			node = At<void*>(node, node::kParentNode);
		}
		// Then the bus's parents; the nearest listed one decides.
		Tier tier = Tier::None;
		char chain[160] = "";
		size_t used = 0;
		for (int depth = 0; bus && depth < 16; ++depth) {
			const uint32_t id = At<uint32_t>(bus, indexable::kID);
			if (used < sizeof(chain) - 12) {
				used += static_cast<size_t>(snprintf(chain + used, sizeof(chain) - used, "%s%u", used ? ">" : "", id));
			}
			if (tier == Tier::None) {
				if (Listed(id, gConfig.mSkyBuses, gConfig.mSkyBusCount)) {
					tier = Tier::Sky;
				}
				else if (Listed(id, gConfig.mAmbienceBuses, gConfig.mAmbienceBusCount)) {
					tier = Tier::Ambience;
				}
			}
			bus = At<void*>(bus, node::kBusOutputNode);
		}
		if (tier != Tier::None) {
			LOG("heights: sound %u (buses %s) lifts as %s", soundID, chain, TierName(tier));
		}
		return tier;
	}

	static Tier TierOfSound(const void* sound, uint32_t soundID)
	{
		uint32_t slot = (soundID * 2654435761u) % kSoundTable;
		for (int probe = 0; probe < kSoundTable; ++probe, slot = (slot + 1) % kSoundTable) {
			if (gSoundTiers[slot].mSoundID == soundID) {
				return gSoundTiers[slot].mTier;
			}
			if (gSoundTiers[slot].mSoundID == 0) {
				break;
			}
		}
		const Tier tier = ClassifySound(sound, soundID);
		if (gSoundTierCount < kSoundTable / 2) {
			gSoundTiers[slot] = { soundID, tier };
			++gSoundTierCount;
		}
		return tier;
	}

	void OnVoiceMix(const void* pbi, const void* mixBus, const AkVPLState* state, AkAudioMix* mix)
	{
		if (!gSampleRate || (gMaster <= 0.0f && gMasterNext <= 0.0f) || !state->buffer.pData || !state->buffer.uValidFrames) {
			return;
		}
		const void* sound = At<void*>(pbi, pbi::kSound);
		if (!sound) {
			return;
		}
		const uint32_t soundID = At<uint32_t>(sound, indexable::kID);
		if (!soundID) {
			return;
		}
		const Tier tier = TierOfSound(sound, soundID);
		if (tier == Tier::None) {
			return;
		}
		const float share = ShareOf(tier);
		if (share <= 0.0f) {
			return;
		}

		// The mix is always 8 gains in Wwise order (FL FR C BL BR SL SR LFE), whatever the bus layout.
		static FloorMap map;
		static bool mapReady = false;
		if (!mapReady) {
			mapReady = BuildFloorMap(0x63F, map);
		}

		const uint32_t frames = std::min<uint32_t>(state->buffer.uMaxFrames, kBlockFrames);
		const uint32_t valid = std::min<uint32_t>(state->buffer.uValidFrames, frames);
		const uint32_t inputs = std::min<uint32_t>(std::popcount(state->buffer.uChannelMask), 8);
		const float downstream = At<float>(mixBus, vpl::kDownstreamGain);
		const float step = 1.0f / static_cast<float>(frames);
		const float share0 = share * gMaster;
		const float share1 = share * gMasterNext;

		// Per floor speaker: the voice's contribution (all input channels × their ramped gains), sent up with
		// the map's weights; then the gains handed to Wwise lose the same energy.
		float floor[kBlockFrames];
		for (int s = 0; s < kFloor; ++s) {
			if (map.mSquares[s] == 0.0f) {
				continue;
			}
			bool any = false;
			for (uint32_t k = 0; k < inputs; ++k) {
				any |= mix[k].previous[s] != 0.0f || mix[k].next[s] != 0.0f;
			}
			if (!any) {
				continue;
			}
			std::memset(floor, 0, valid * sizeof(float));
			for (uint32_t k = 0; k < inputs; ++k) {
				const float g0 = mix[k].previous[s];
				const float g1 = mix[k].next[s];
				if (g0 == 0.0f && g1 == 0.0f) {
					continue;
				}
				const float* in = static_cast<const float*>(state->buffer.pData) + static_cast<size_t>(k) * state->buffer.uMaxFrames;
				for (uint32_t i = 0; i < valid; ++i) {
					floor[i] += in[i] * (g0 + (g1 - g0) * i * step);
				}
			}
			for (int h = 0; h < kChannels; ++h) {
				const float w = map.mWeight[h][s];
				if (w == 0.0f) {
					continue;
				}
				float* out = gMix[h];
				for (uint32_t i = 0; i < valid; ++i) {
					const float t = i * step;
					out[i] += floor[i] * w * downstream * (share0 + (share1 - share0) * t);
				}
			}
			const float k0 = share0 * share0 * map.mSquares[s];
			const float k1 = share1 * share1 * map.mSquares[s];
			const float keep0 = k0 < 1.0f ? std::sqrt(1.0f - k0) : 0.0f;
			const float keep1 = k1 < 1.0f ? std::sqrt(1.0f - k1) : 0.0f;
			for (uint32_t k = 0; k < inputs; ++k) {
				mix[k].previous[s] *= keep0;
				mix[k].next[s] *= keep1;
			}
		}
		gFed = true;
		++gStats.mVoices[static_cast<int>(tier)];
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
