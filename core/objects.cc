#include "objects.hh"

#include <algorithm>
#include <bit>
#include <cmath>

#include "config.hh"
#include "log.hh"

namespace objects
{
	using namespace wwise;

	constexpr int kMaxVoices = 128;

	// Largest single speaker gain / total gain. A point source panned between two speakers is >= 0.7; Wwise
	// spread smears the gain evenly (0.41-0.45 over 5-6 speakers). Spread sounds are area sounds: bed.
	constexpr float kPointLike = 0.6f;

	// gain × RMS below this (about -70 dBFS) isn't worth an object.
	constexpr float kMinLevel = 0.0003f;

	// A voice keeps a new role at least this many buffers (~250 ms), so rankings that flip back and forth
	// don't make it ping-pong between bed and object.
	constexpr uint64_t kMinRoleFrames = 12;

	// Current objects rank as if this much louder (hysteresis).
	constexpr float kKeepBonus = 2.0f;

	enum class Role : uint8_t
	{
		Bed,
		FadeIn,  // this buffer: bed → object crossfade
		Object,
		FadeOut, // this buffer: object → bed crossfade; the slot stays reserved until it ends
	};

	struct Voice
	{
		const void* mPbi;
		uint32_t mPlayingID;
		uint64_t mLastSeen;
		uint64_t mRoleSince;
		float mScore;
		bool mCandidate;
		Role mRole;
		int mSlot;
	};

	static Voice gVoices[kMaxVoices];
	static int gVoiceCount = 0;
	static spatial::ObjectBlock gBlock;
	static uint64_t gFrame = kMinRoleFrames + 1;
	static uint32_t gBudget = 0;    // slots usable this buffer
	static uint32_t gSlotsUsed = 0; // slots owned by a voice (any role but Bed)

	struct Stats
	{
		uint32_t mInstant = 0;
		uint32_t mPromoted = 0;
		uint32_t mDemoted = 0;
		uint32_t mEnded = 0;
		int mVoicesMax = 0;
		int mCandidatesMax = 0;
		uint32_t mTableFull = 0;
	};
	static Stats gStats;

	static Voice* Find(const void* pbi, uint32_t playingID)
	{
		for (int i = 0; i < gVoiceCount; ++i) {
			if (gVoices[i].mPbi == pbi && gVoices[i].mPlayingID == playingID) {
				return &gVoices[i];
			}
		}
		return nullptr;
	}

	static int FreeSlot()
	{
		for (uint32_t slot = 0; slot < gBudget; ++slot) {
			if (!(gSlotsUsed & (1u << slot))) {
				return static_cast<int>(slot);
			}
		}
		return -1;
	}

	static void SetRole(Voice& v, Role role, int slot)
	{
		if (v.mSlot >= 0 && slot != v.mSlot) {
			gSlotsUsed &= ~(1u << v.mSlot);
		}
		if (slot >= 0) {
			gSlotsUsed |= 1u << slot;
		}
		v.mRole = role;
		v.mSlot = slot;
		v.mRoleSince = gFrame;
	}

	static float Norm7(const float* gains)
	{
		// Index 7 is LFE (Wwise's internal order: FL FR C BL BR SL SR LFE); objects carry no LFE send.
		float sum = 0.0f;
		for (int i = 0; i < 7; ++i) {
			sum += gains[i] * gains[i];
		}
		return std::sqrt(sum);
	}

	void OnDryMix(const void* cbx, const void* pbi, const void* mixBus, const AkVPLState* state, AkAudioMix* mix)
	{
		if (!gBudget) {
			return;
		}

		const uint32_t playingID = At<uint32_t>(pbi, pbi::kPlayingID);
		Voice* v = Find(pbi, playingID);
		if (!v) {
			if (gVoiceCount == kMaxVoices) {
				++gStats.mTableFull;
				return;
			}
			v = &gVoices[gVoiceCount++];
			// Eligible for promotion right away: it has no bed/object history to protect.
			*v = { pbi, playingID, 0, gFrame - kMinRoleFrames, 0.0f, false, Role::Bed, -1 };
		}
		const bool isNew = v->mLastSeen == 0;
		v->mLastSeen = gFrame;

		const uint32_t channelMask = state->buffer.uChannelMask;
		const auto* rays = At<AkRayVolumeData*>(cbx, cbx::kVolumeData);
		const uint32_t rayCount = At<uint32_t>(cbx, cbx::kVolumeData + 8);
		const bool renderable = (At<uint8_t>(pbi, pbi::kPannerBits) & 3) != 0 && std::popcount(channelMask) == 1 &&
			!(channelMask & 0x8) && rays && rayCount == 1 && state->buffer.pData;

		if (!renderable) {
			// Can't be an object this buffer (not 3D/mono any more, several positions...): straight back to the bed.
			v->mCandidate = false;
			v->mScore = 0.0f;
			if (v->mRole != Role::Bed) {
				SetRole(*v, Role::Bed, -1);
				++gStats.mDemoted;
			}
			return;
		}

		const float gainPrev = Norm7(mix[0].previous);
		const float gainNext = Norm7(mix[0].next);
		const float maxNext = *std::max_element(mix[0].next, mix[0].next + 7);
		const float downstream = At<float>(mixBus, vpl::kDownstreamGain);
		const float* samples = static_cast<const float*>(state->buffer.pData);
		const uint32_t valid = std::min<uint32_t>(state->buffer.uValidFrames, spatial::kBlockFrames);

		float energy = 0.0f;
		for (uint32_t i = 0; i < valid; ++i) {
			energy += samples[i] * samples[i];
		}
		const float rms = std::sqrt(energy / spatial::kBlockFrames);

		v->mCandidate = gainNext > 0.0f ? maxNext / gainNext >= kPointLike : v->mCandidate;
		v->mScore = v->mCandidate ? gainNext * downstream * rms : 0.0f;

		if (isNew && v->mCandidate && v->mRole == Role::Bed && v->mScore >= kMinLevel) {
			const int slot = FreeSlot();
			if (slot >= 0) {
				SetRole(*v, Role::Object, slot);
				++gStats.mInstant;
			}
		}

		if (v->mRole == Role::Bed) {
			return;
		}

		// Object share of the voice at the start/end of this buffer.
		const float w0 = v->mRole == Role::FadeIn ? 0.0f : 1.0f;
		const float w1 = v->mRole == Role::FadeOut ? 0.0f : 1.0f;

		// Same ramp as CAkMixer::Mix3D: Previous at sample 0, towards Next over the full buffer. Bus volumes
		// (mix sliders, ducking) are applied at bus mix time, which objects skip, hence the downstream gain.
		float* out = gBlock.mSamples[v->mSlot];
		const float step = 1.0f / spatial::kBlockFrames;
		for (uint32_t i = 0; i < spatial::kBlockFrames; ++i) {
			const float t = i * step;
			const float gain = (gainPrev + (gainNext - gainPrev) * t) * (w0 + (w1 - w0) * t) * downstream;
			out[i] = i < valid ? samples[i] * gain : 0.0f;
		}

		// Wwise's listener-relative direction: theta = atan2(right, front), phi = asin(up / r). Windows wants
		// +x right, +y up, +z behind.
		const float theta = rays[0].theta;
		const float phi = rays[0].phi;
		const float distance = gConfig.mObjectDistance;
		float* position = gBlock.mPosition[v->mSlot];
		position[0] = distance * std::sin(theta) * std::cos(phi);
		position[1] = distance * std::sin(phi);
		position[2] = -distance * std::cos(theta) * std::cos(phi);
		gBlock.mActiveMask |= 1u << v->mSlot;

		// What's left for the bed; the mixer ramps between these just like it would have.
		for (int k = 0; k < 8; ++k) {
			mix[0].previous[k] *= 1.0f - w0;
			mix[0].next[k] *= 1.0f - w1;
		}
	}

	const spatial::ObjectBlock* FinishFrame()
	{
		// Voices that didn't render this buffer have ended (or gone virtual): drop them. Wwise already faded
		// them out, so their slot is free without a crossfade.
		for (int i = 0; i < gVoiceCount;) {
			Voice& v = gVoices[i];
			if (v.mLastSeen != gFrame) {
				if (v.mSlot >= 0) {
					gSlotsUsed &= ~(1u << v.mSlot);
					++gStats.mEnded;
				}
				v = gVoices[--gVoiceCount];
				continue;
			}
			++i;
		}

		for (int i = 0; i < gVoiceCount; ++i) {
			Voice& v = gVoices[i];
			if (v.mRole == Role::FadeIn) {
				v.mRole = Role::Object;
			}
			else if (v.mRole == Role::FadeOut) {
				SetRole(v, Role::Bed, -1);
			}
		}

		gStats.mVoicesMax = std::max(gStats.mVoicesMax, gVoiceCount);
		++gFrame;

		const uint32_t budget = gConfig.mObjects && spatial::IsActive() ? spatial::ObjectSlots() : 0;
		if (budget < gBudget) {
			// Fewer slots than before (stream reopened with fewer objects, or gone): no time to crossfade.
			for (int i = 0; i < gVoiceCount; ++i) {
				if (gVoices[i].mSlot >= static_cast<int>(budget)) {
					SetRole(gVoices[i], Role::Bed, -1);
				}
			}
		}
		gBudget = budget;
		if (!gBudget) {
			return &gBlock;
		}

		// Rank candidates for the next buffer.
		int order[kMaxVoices];
		int candidates = 0;
		for (int i = 0; i < gVoiceCount; ++i) {
			if (gVoices[i].mCandidate && gVoices[i].mScore >= kMinLevel) {
				order[candidates++] = i;
			}
		}
		gStats.mCandidatesMax = std::max(gStats.mCandidatesMax, candidates);
		auto rank = [](const Voice& v) { return v.mRole == Role::Object ? v.mScore * kKeepBonus : v.mScore; };
		std::sort(order, order + candidates, [&](int a, int b) { return rank(gVoices[a]) > rank(gVoices[b]); });

		bool wanted[kMaxVoices] = {};
		for (int n = 0; n < candidates && n < static_cast<int>(gBudget); ++n) {
			wanted[order[n]] = true;
		}

		for (int i = 0; i < gVoiceCount; ++i) {
			Voice& v = gVoices[i];
			const bool settled = gFrame - v.mRoleSince >= kMinRoleFrames;
			if (v.mRole == Role::Object && !wanted[i] && (settled || !v.mCandidate)) {
				SetRole(v, Role::FadeOut, v.mSlot);
				++gStats.mDemoted;
			}
		}
		for (int n = 0; n < candidates && n < static_cast<int>(gBudget); ++n) {
			Voice& v = gVoices[order[n]];
			if (v.mRole == Role::Bed && gFrame - v.mRoleSince >= kMinRoleFrames) {
				const int slot = FreeSlot();
				if (slot < 0) {
					break;
				}
				SetRole(v, Role::FadeIn, slot);
				++gStats.mPromoted;
			}
		}
		return &gBlock;
	}

	void StartFrame()
	{
		gBlock.mActiveMask = 0;
	}

	int SlotOf(const void* pbi)
	{
		for (int i = 0; i < gVoiceCount; ++i) {
			if (gVoices[i].mPbi == pbi) {
				return gVoices[i].mRole == Role::Bed ? -1 : gVoices[i].mSlot;
			}
		}
		return -1;
	}

	void LogStats()
	{
		LOG("objects: budget %u, slots in use %d; since last: %u instant, %u promoted, %u demoted, %u ended as objects, "
			"max %d voices tracked, max %d candidates%s",
			gBudget, std::popcount(gSlotsUsed), gStats.mInstant, gStats.mPromoted, gStats.mDemoted, gStats.mEnded,
			gStats.mVoicesMax, gStats.mCandidatesMax, gStats.mTableFull ? " (voice table full!)" : "");
		gStats = {};
	}
}
