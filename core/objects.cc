#include "objects.hh"

#include <algorithm>
#include <bit>
#include <cmath>

#include <Windows.h>

#include "config.hh"
#include "game.hh"
#include "log.hh"

namespace objects
{
	using namespace wwise;
	using telemetry::Reason;

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
	static uint32_t gTarget = 0;    // slots new objects may take this buffer
	static uint32_t gBudget = 0;    // slots that may still be in use (> gTarget while shrinking)
	static uint32_t gSlotsUsed = 0; // slots owned by a voice (any role but Bed)

	struct Stats
	{
		uint32_t mInstant = 0;
		uint32_t mPromoted = 0;
		uint32_t mOutranked = 0;     // demoted: other voices louder
		uint32_t mDisqualified = 0;  // demoted: became spread/quiet
		uint32_t mUnrenderable = 0;  // demoted at once: lost its position or stopped being mono
		uint32_t mShrunk = 0;        // demoted: objects switched off / limit lowered
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
		for (uint32_t slot = 0; slot < gTarget; ++slot) {
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

	// ---- The player's own game objects ----

	// Wwise game object IDs are addresses of the game's AudioEntity objects (see game.hh). The player's actor
	// component is found by its SimObject name; its second ("__SFX") entity by the pointer the component holds.
	static uint64_t gPlayerEntity = 0;
	static uint64_t gPlayerSfxEntity = 0;

	// Reads the entity's name and, for the player's component, its SFX entity pointer. The game frees the SFX
	// entity right after unregistering it while a last buffer of its voices can still render, and the ID isn't
	// guaranteed to be an entity at all, so the reads are SEH-guarded (hence no C++ objects in this function).
	static bool ReadEntity(uint64_t entity, uint32_t& nameUID, uint64_t& sfxEntity)
	{
		__try {
			nameUID = *reinterpret_cast<const uint32_t*>(entity + game::audio_entity::kName);
			sfxEntity = nameUID == game::kPlayerNameUID
				? *reinterpret_cast<const uint64_t*>(entity - game::actor_audio::kEntityBase + game::actor_audio::kSfxEntity)
				: 0;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	static bool IsPlayerVoice(const void* pbi)
	{
		const void* gameObj = At<void*>(pbi, pbi::kGameObj);
		const uint64_t entity = gameObj ? At<uint64_t>(gameObj, game_obj::kID) : 0;
		if (entity < 0x10000 || entity >= 0x00007FFFFFFF0000ull) {
			// Not a user-mode pointer: the game also uses small numbers for global (2D) objects.
			return false;
		}
		if (entity == gPlayerSfxEntity) {
			return true;
		}
		uint32_t name = 0;
		uint64_t sfx = 0;
		if (!ReadEntity(entity, name, sfx) || name != game::kPlayerNameUID) {
			return false;
		}
		if (entity != gPlayerEntity || sfx != gPlayerSfxEntity) {
			// Re-created after a load, or the component re-initialized (new SFX entity).
			LOG("objects: player audio entity %llX, SFX entity %llX", entity, sfx);
			gPlayerEntity = entity;
			gPlayerSfxEntity = sfx;
		}
		return true;
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

	uint32_t Target()
	{
		return gTarget;
	}

	bool OnDryMix(const void* cbx, const void* pbi, const void* mixBus, const AkVPLState* state, AkAudioMix* mix,
		telemetry::Voice& report)
	{
		const auto* rays = At<AkRayVolumeData*>(cbx, cbx::kVolumeData);
		const uint32_t rayCount = At<uint32_t>(cbx, cbx::kVolumeData + 8);
		const bool is3D = (At<uint8_t>(pbi, pbi::kPannerBits) & 3) != 0;
		const void* sound = At<void*>(pbi, pbi::kSound);
		const uint32_t playingID = At<uint32_t>(pbi, pbi::kPlayingID);
		Voice* v = gBudget ? Find(pbi, playingID) : nullptr;

		if (!is3D || !rays || !rayCount) {
			// No position (2D, or rays not computed): can't be an object, nothing to draw.
			if (v && v->mRole != Role::Bed) {
				SetRole(*v, Role::Bed, -1);
				++gStats.mUnrenderable;
			}
			if (v) {
				v->mLastSeen = gFrame;
				v->mCandidate = false;
			}
			return false;
		}

		const uint32_t channelMask = state->buffer.uChannelMask;
		const bool mono = std::popcount(channelMask) == 1 && !(channelMask & 0x8) && state->buffer.pData;
		const float gainPrev = Norm7(mix[0].previous);
		const float gainNext = Norm7(mix[0].next);
		const float maxNext = *std::max_element(mix[0].next, mix[0].next + 7);
		const float downstream = At<float>(mixBus, vpl::kDownstreamGain);
		const float* samples = static_cast<const float*>(state->buffer.pData);
		const uint32_t valid = samples ? std::min<uint32_t>(state->buffer.uValidFrames, spatial::kBlockFrames) : 0;

		float energy = 0.0f;
		for (uint32_t i = 0; i < valid; ++i) {
			energy += samples[i] * samples[i];
		}
		const float level = gainNext * downstream * std::sqrt(energy / spatial::kBlockFrames);

		Reason reason = Reason::Candidate;
		if (gConfig.mPlayerInBed.load(std::memory_order_relaxed) && IsPlayerVoice(pbi)) {
			reason = Reason::Player;
		}
		else if (rayCount > 1) {
			reason = Reason::MultiPosition;
		}
		else if (!mono) {
			reason = Reason::NotMono;
		}
		else if (gainNext <= 0.0f) {
			reason = Reason::Quiet;
		}
		else if (maxNext / gainNext < kPointLike) {
			reason = Reason::Spread;
		}
		else if (level < kMinLevel) {
			reason = Reason::Quiet;
		}

		report.mTheta = rays[0].theta;
		report.mPhi = rays[0].phi;
		report.mDistance = rays[0].r;
		report.mLevel = level;
		report.mSoundID = sound ? At<uint32_t>(sound, indexable::kID) : 0;
		report.mSlot = -1;
		report.mReason = reason;

		if (!gBudget) {
			return true;
		}

		if (!v) {
			if (gVoiceCount == kMaxVoices) {
				++gStats.mTableFull;
				return true;
			}
			v = &gVoices[gVoiceCount++];
			// Eligible for promotion right away: it has no bed/object history to protect.
			*v = { pbi, playingID, 0, gFrame - kMinRoleFrames, 0.0f, false, Role::Bed, -1 };
		}
		const bool isNew = v->mLastSeen == 0;
		v->mLastSeen = gFrame;
		v->mCandidate = reason == Reason::Candidate;
		v->mScore = v->mCandidate ? level : 0.0f;

		if (rayCount > 1 || !mono) {
			// Can't be rendered as one mono object this buffer: straight back to the bed.
			if (v->mRole != Role::Bed) {
				SetRole(*v, Role::Bed, -1);
				++gStats.mUnrenderable;
			}
			return true;
		}

		if (isNew && v->mCandidate && v->mRole == Role::Bed) {
			const int slot = FreeSlot();
			if (slot >= 0) {
				SetRole(*v, Role::Object, slot);
				++gStats.mInstant;
			}
		}

		if (v->mRole == Role::Bed) {
			return true;
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
		const float distance = gConfig.mObjectDistance.load(std::memory_order_relaxed);
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

		report.mSlot = static_cast<int8_t>(v->mSlot);
		report.mReason = Reason::Object;
		return true;
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

		const uint32_t slots = spatial::IsActive() ? spatial::ObjectSlots() : 0;
		const int maxObjects = std::max(0, gConfig.mMaxObjects.load(std::memory_order_relaxed));
		const uint32_t target = gConfig.mObjects.load(std::memory_order_relaxed) ? std::min(slots, static_cast<uint32_t>(maxObjects)) : 0;

		if (slots < gBudget) {
			// The stream went away or reopened with fewer objects: those slots are gone, no crossfade possible.
			for (int i = 0; i < gVoiceCount; ++i) {
				if (gVoices[i].mSlot >= static_cast<int>(slots)) {
					SetRole(gVoices[i], Role::Bed, -1);
				}
			}
			gBudget = slots;
		}
		gTarget = target;
		if (target >= gBudget) {
			gBudget = target;
		}
		else {
			// Switched off (A/B) or limit lowered: crossfade the objects above the new limit back into the bed,
			// and only shrink once they're all out.
			bool fading = false;
			for (int i = 0; i < gVoiceCount; ++i) {
				Voice& v = gVoices[i];
				if (v.mSlot >= static_cast<int>(target)) {
					if (v.mRole == Role::Object) {
						SetRole(v, Role::FadeOut, v.mSlot);
						++gStats.mShrunk;
					}
					fading = true;
				}
			}
			if (!fading) {
				gBudget = target;
			}
		}
		if (!gBudget) {
			gVoiceCount = 0;
			gSlotsUsed = 0;
			return &gBlock;
		}

		// Rank candidates for the next buffer.
		int order[kMaxVoices];
		int candidates = 0;
		for (int i = 0; i < gVoiceCount; ++i) {
			if (gVoices[i].mCandidate) {
				order[candidates++] = i;
			}
		}
		gStats.mCandidatesMax = std::max(gStats.mCandidatesMax, candidates);
		auto rank = [](const Voice& v) { return v.mRole == Role::Object ? v.mScore * kKeepBonus : v.mScore; };
		std::sort(order, order + candidates, [&](int a, int b) { return rank(gVoices[a]) > rank(gVoices[b]); });

		bool wanted[kMaxVoices] = {};
		for (int n = 0; n < candidates && n < static_cast<int>(gTarget); ++n) {
			wanted[order[n]] = true;
		}

		for (int i = 0; i < gVoiceCount; ++i) {
			Voice& v = gVoices[i];
			if (v.mRole != Role::Object || wanted[i] || v.mSlot >= static_cast<int>(gTarget)) {
				continue;
			}
			if (!v.mCandidate) {
				SetRole(v, Role::FadeOut, v.mSlot);
				++gStats.mDisqualified;
			}
			else if (gFrame - v.mRoleSince >= kMinRoleFrames) {
				SetRole(v, Role::FadeOut, v.mSlot);
				++gStats.mOutranked;
			}
		}
		for (int n = 0; n < candidates && n < static_cast<int>(gTarget); ++n) {
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
		LOG("objects: target %u, budget %u, slots in use %d; since last: %u instant, %u promoted; demoted %u outranked, "
			"%u disqualified, %u unrenderable, %u switched off; %u ended as objects; max %d voices tracked, "
			"max %d candidates%s",
			gTarget, gBudget, std::popcount(gSlotsUsed), gStats.mInstant, gStats.mPromoted, gStats.mOutranked,
			gStats.mDisqualified, gStats.mUnrenderable, gStats.mShrunk, gStats.mEnded, gStats.mVoicesMax,
			gStats.mCandidatesMax, gStats.mTableFull ? " (voice table full!)" : "");
		gStats = {};
	}
}
