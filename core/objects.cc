#include "objects.hh"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <Windows.h>

#include "config.hh"
#include "game.hh"
#include "log.hh"
#include "sounds.hh"
#include "wwise_hooks.hh"

namespace objects
{
	using namespace wwise;
	using telemetry::Reason;

	// Big gunfights while driving reach ~160 3D voices (second in-game log hit the old limit of 128 twice).
	constexpr int kMaxVoices = 256;

	// Largest single speaker gain / total gain. A point source panned between two speakers is >= 0.7; Wwise
	// spread smears the gain evenly (0.41-0.45 over 5-6 speakers). Spread sounds are area sounds: bed.
	constexpr float kPointLike = 0.6f;

	// An object keeps its slot down to this much below kPointLike: Wwise spread grows with proximity, and a
	// source hovering at the threshold would otherwise crossfade back and forth every few buffers.
	constexpr float kSpreadHysteresis = 0.1f;

	// gain × RMS below this (about -70 dBFS) isn't worth a new object. A current object isn't demoted for being
	// quiet: a decaying tail moving into the bed is an audible position jump for nothing.
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
		bool mCandidate; // may become an object
		bool mHoldable;  // may stay one (looser than mCandidate: quiet is fine, spread has hysteresis)
		Role mRole;
		int mSlot;
		Reason mReason; // last decision, for the voice log
	};

	static Voice gVoices[kMaxVoices];
	static int gVoiceCount = 0;
	static spatial::ObjectBlock gBlock;
	static uint64_t gFrame = kMinRoleFrames + 1;
	static uint32_t gTarget = 0;    // slots new objects may take this buffer
	static uint32_t gBudget = 0;    // slots that may still be in use (> gTarget while shrinking)
	static uint32_t gSlotsUsed = 0; // slots owned by a voice (any role but Bed)

	// Bus Parametric EQs an object reproduces (see "Insert effects" below): biquad memories (x1 x2 y1 y2) per
	// slot, EQ stage and band, so the filters run continuously across buffers like the bus's do. Cleared when a
	// slot changes hands.
	constexpr int kMaxEqStages = 4; // EQ instances along one voice's bus chain
	static float gEqState[spatial::kMaxObjects][kMaxEqStages][eq_fx::kBands][4];

	struct Stats
	{
		uint32_t mInstant = 0;
		uint32_t mPromoted = 0;
		uint32_t mOutranked = 0;     // demoted: other voices louder
		uint32_t mDisqualified = 0;  // demoted: became spread, or a bed rule applies (player, bus fx)
		uint32_t mUnrenderable = 0;  // demoted at once: lost its position or stopped being mono
		uint32_t mShrunk = 0;        // demoted: objects switched off / limit lowered
		uint32_t mEnded = 0;
		int mVoicesMax = 0;
		int mCandidatesMax = 0;
		uint32_t mTableFull = 0;
		uint32_t mReasons[telemetry::kReasonCount] = {}; // 3D voice mixes per final reason
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
			if (slot != v.mSlot) {
				std::memset(gEqState[slot], 0, sizeof(gEqState[slot]));
			}
		}
		v.mRole = role;
		v.mSlot = slot;
		v.mRoleSince = gFrame;
	}

	// ---- The player's own game objects ----

	// Wwise game object IDs are addresses of the game's AudioEntity objects (see game.hh). The player's actor
	// component is found by its SimObject name, its second ("__SFX") entity by the pointer the component holds,
	// and its footsteps/impacts play on pooled OneShot entities whose owner handle points into the component.
	static uint64_t gPlayerComponent = 0;
	static uint64_t gPlayerSfxEntity = 0;

	// One-shots fired for the player by other systems (action-tree audio tasks: the walk↔sprint transition
	// steps, damage rigs, VFX) hold their handle in the task, not in the audio component, so they're recognized
	// by position instead: within this many meters of the player's audio entity. NPC one-shots in melee range
	// get caught too, which only sends them to the bed, as the original mix had them.
	constexpr float kNearPlayerMeters = 1.5f;

	// The game frees entities (OneShots, the SFX entity) right after unregistering them while a last buffer of
	// their voices can still render: reads go through the guarded helpers in game.hh.
	using game::IsPointer;
	using game::ReadU32;
	using game::ReadU64;

	static bool ReadPosition(uint64_t entity, float* xyz)
	{
		return game::ReadFloats3(entity + game::audio_entity::kPosition, xyz);
	}

	static bool IsOneShotName(uint32_t nameUID)
	{
		static uint32_t names[game::one_shot::kMaxIndex - game::one_shot::kFirstIndex + 1];
		static bool ready = false;
		if (!ready) {
			for (uint32_t i = game::one_shot::kFirstIndex; i <= game::one_shot::kMaxIndex; ++i) {
				char text[16];
				snprintf(text, sizeof(text), "OneShot_%3u", i);
				names[i - game::one_shot::kFirstIndex] = game::SymbolUID(text);
			}
			std::sort(std::begin(names), std::end(names));
			ready = true;
		}
		return std::binary_search(std::begin(names), std::end(names), nameUID);
	}

	static void LearnPlayerComponent(uint64_t component)
	{
		uint64_t sfx = 0;
		ReadU64(component + game::actor_audio::kSfxEntity, sfx);
		if (component != gPlayerComponent || sfx != gPlayerSfxEntity) {
			// First sighting, re-created after a load, or the component re-initialized (new SFX entity).
			LOG("objects: player audio component %llX (entity %llX), SFX entity %llX", component,
				component + game::actor_audio::kEntityBase, sfx);
			gPlayerComponent = component;
			gPlayerSfxEntity = sfx;
		}
	}

	static bool IsPlayerVoice(const void* pbi)
	{
		const void* gameObj = At<void*>(pbi, pbi::kGameObj);
		const uint64_t entity = gameObj ? At<uint64_t>(gameObj, game_obj::kID) : 0;
		if (!IsPointer(entity)) {
			return false;
		}
		if (entity == gPlayerSfxEntity) {
			return true;
		}
		uint32_t name = 0;
		if (!ReadU32(entity + game::audio_entity::kName, name)) {
			return false;
		}
		if (name == game::kPlayerNameUID) {
			LearnPlayerComponent(entity - game::actor_audio::kEntityBase);
			return true;
		}
		if (!IsOneShotName(name)) {
			return false;
		}
		// A one-shot: whose handle owns it? Any handle inside the player's component counts (footsteps, but
		// also whatever else the component fires). Before the component is known, the footstep handles reveal
		// it: the handle's owner has the player's name.
		uint64_t handle = 0;
		if (ReadU64(entity + game::one_shot::kOwnerHandle, handle) && IsPointer(handle)) {
			if (gPlayerComponent && handle >= gPlayerComponent && handle < gPlayerComponent + game::actor_audio::kSize) {
				return true;
			}
			for (const size_t offset : { game::actor_audio::kLeftFootstep, game::actor_audio::kRightFootstep }) {
				const uint64_t component = handle - offset;
				uint32_t owner = 0;
				if (ReadU32(component + game::actor_audio::kEntityBase + game::audio_entity::kName, owner) && owner == game::kPlayerNameUID) {
					LearnPlayerComponent(component);
					return true;
				}
			}
		}
		// Not fired by the component: is it at the player's position?
		float player[3], shot[3];
		if (!gPlayerComponent || !ReadPosition(gPlayerComponent + game::actor_audio::kEntityBase, player) || !ReadPosition(entity, shot)) {
			return false;
		}
		const float dx = shot[0] - player[0], dy = shot[1] - player[1], dz = shot[2] - player[2];
		return dx * dx + dy * dy + dz * dz < kNearPlayerMeters * kNearPlayerMeters;
	}

	// ---- Insert effects on the bus chain ----

	// Objects are taken before the bus mix, so whatever effects the voice's bus and its parents run never touch
	// them. A Parametric EQ is linear and its instance holds the biquad coefficients, so objects run the same
	// filters themselves (EqChain); anything else (compressor, limiter, delay...) can't be reproduced and keeps
	// the voice in the bed under the BusFx policy. Each bus's active effects are remembered so the log shows the
	// chain once, and again when it changes (bypass actions, buses re-created).
	struct BusRecord
	{
		const void* mBus;
		uint32_t mIds[vpl::kFxSlots];
	};
	constexpr int kMaxBusRecords = 32;
	static BusRecord gBuses[kMaxBusRecords];
	static int gBusCount = 0;
	static int gBusNext = 0;

	struct EqChain
	{
		const void* mEffects[kMaxEqStages]; // CAkParametricEQFX*, voice bus first
		int mCount;
	};

	// True if `bus` (a CAkBusFX: mix bus or final mix) runs an effect objects can't reproduce: an effect in the
	// slot, not bypassed, not "bypass all" (what CAkVPLMixBusNode::ProcessAllFX executes), not the Meter (which
	// only measures) and not a Parametric EQ (collected into `eq` instead, when there is room).
	static bool BusHasFx(const void* bus, EqChain* eq)
	{
		uint32_t ids[vpl::kFxSlots] = {};
		bool audible = false;
		const bool bypassAll = At<uint8_t>(bus, vpl::kBypassAllFx) & 1;
		for (uint32_t i = 0; i < vpl::kFxSlots; ++i) {
			const uint8_t* slot = static_cast<const uint8_t*>(bus) + vpl::kFx + i * bus_fx::kStride;
			const void* effect = At<void*>(slot, bus_fx::kEffect);
			if (effect && !(At<uint8_t>(slot, bus_fx::kFlags) & 1) && !bypassAll) {
				ids[i] = At<uint32_t>(slot, bus_fx::kID);
				if (ids[i] == kParametricEqFx && eq && eq->mCount < kMaxEqStages) {
					eq->mEffects[eq->mCount++] = effect;
				}
				else {
					audible |= ids[i] != kMeterFx;
				}
			}
		}

		BusRecord* rec = nullptr;
		for (int i = 0; i < gBusCount; ++i) {
			if (gBuses[i].mBus == bus) {
				rec = &gBuses[i];
				break;
			}
		}
		if (!rec) {
			// Buses are created and freed as they become active; the ring just forgets the oldest.
			rec = &gBuses[gBusNext];
			gBusNext = (gBusNext + 1) % kMaxBusRecords;
			gBusCount = std::min(gBusCount + 1, kMaxBusRecords);
			rec->mBus = bus;
			std::memset(rec->mIds, 0, sizeof(rec->mIds));
			if (!ids[0] && !ids[1] && !ids[2] && !ids[3]) {
				return false; // the common case: nothing to say
			}
		}
		if (std::memcmp(rec->mIds, ids, sizeof(ids)) != 0) {
			std::memcpy(rec->mIds, ids, sizeof(ids));
			char text[200] = "";
			for (uint32_t i = 0; i < vpl::kFxSlots; ++i) {
				if (ids[i]) {
					snprintf(text + std::strlen(text), sizeof(text) - std::strlen(text), "%s%s (0x%X)%s",
						text[0] ? ", " : "", PluginName(ids[i]), ids[i],
						ids[i] == kParametricEqFx ? " [applied to objects]" : ids[i] == kMeterFx ? " [ignored]" : "");
				}
			}
			LOG("objects: bus %p (id %u, parent %p) fx: %s", bus, At<uint32_t>(bus, vpl::kBusID),
				At<void*>(bus, vpl::kParent), text[0] ? text : "none");
		}
		return audible;
	}

	// Effects anywhere between the voice's bus and the output that objects can't reproduce; `eq` receives the
	// Parametric EQs they can. The master bus (final mix) counts only under policy 2: a master limiter/EQ is
	// common and would rule out every object.
	static bool ChainHasFx(const void* mixBus, int policy, EqChain& eq)
	{
		bool any = false;
		uint64_t device = 0;
		const void* bus = mixBus;
		eq.mCount = 0;
		for (int depth = 0; bus && depth < 16; ++depth) {
			any |= BusHasFx(bus, &eq);
			device = At<uint64_t>(bus, vpl::kDevice);
			bus = At<void*>(bus, vpl::kParent);
		}
		if (const void* master = FinalMixOf(device)) {
			const bool masterFx = BusHasFx(master, policy >= 2 ? &eq : nullptr); // always evaluated so the log shows it
			any |= masterFx && policy >= 2;
		}
		return any;
	}

	// Runs the chain's EQs over an object's samples the way CAkParametricEQFX::Execute does: each enabled band
	// as a biquad with the instance's coefficients, then the output level. A band whose coefficients are dirty
	// (parameters just changed, not yet recomputed by the bus) is skipped for this buffer.
	static void ApplyEq(const EqChain& eq, int slot, float* samples, uint32_t frames)
	{
		for (int stage = 0; stage < eq.mCount; ++stage) {
			const void* effect = eq.mEffects[stage];
			const void* params = At<void*>(effect, eq_fx::kSharedParams);
			if (!params) {
				continue;
			}
			for (uint32_t band = 0; band < eq_fx::kBands; ++band) {
				const size_t bandOffset = eq_params::kBand + band * eq_params::kBandStride;
				if (!At<bool>(params, bandOffset + eq_params::kBandOn) || At<bool>(params, eq_params::kBandDirty + band)) {
					continue;
				}
				const float* c = &At<const float>(effect, eq_fx::kCoefs + band * 5 * sizeof(float));
				float* m = gEqState[slot][stage][band];
				float x1 = m[0], x2 = m[1], y1 = m[2], y2 = m[3];
				for (uint32_t i = 0; i < frames; ++i) {
					const float x = samples[i];
					const float y = c[0] * x + c[1] * x1 + c[2] * x2 + c[3] * y1 + c[4] * y2;
					x2 = x1;
					x1 = x;
					y2 = y1;
					y1 = y;
					samples[i] = y;
				}
				m[0] = x1; m[1] = x2; m[2] = y1; m[3] = y2;
			}
			const float level = At<float>(params, eq_params::kOutputLevel);
			if (level != 0.0f) {
				const float gain = std::pow(10.0f, level * 0.05f);
				for (uint32_t i = 0; i < frames; ++i) {
					samples[i] *= gain;
				}
			}
		}
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

	static bool Route(const void* cbx, const void* pbi, const void* mixBus, const AkVPLState* state, AkAudioMix* mix,
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

		const float pointness = gainNext > 0.0f ? maxNext / gainNext : 1.0f;
		const bool player = gConfig.mPlayerInBed.load(std::memory_order_relaxed) && IsPlayerVoice(pbi);
		const int busPolicy = gConfig.mBusFx.load(std::memory_order_relaxed);
		EqChain eq;
		const bool busFx = ChainHasFx(mixBus, busPolicy, eq) && busPolicy > 0;

		// What the sound is, from its bank-side bus chain: the per-bus rules (ObjectBuses / BedBuses).
		const uint32_t soundID = sound ? At<uint32_t>(sound, indexable::kID) : 0;
		sounds::Info* info = sounds::Lookup(sound, soundID);
		sounds::Info local;
		if (!info && sound) {
			sounds::Walk(sound, soundID, local);
		}
		const sounds::Info& bank = info ? *info : local;
		const bool forced = sounds::Listed(bank, gConfig.mObjectBuses, gConfig.mObjectBusCount);
		const bool bedRule = sounds::Listed(bank, gConfig.mBedBuses, gConfig.mBedBusCount);

		Reason reason = Reason::Candidate;
		if (player) {
			reason = Reason::Player;
		}
		else if (busFx) {
			reason = Reason::BusFx;
		}
		else if (bedRule) {
			reason = Reason::BedRule;
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
		else if (pointness < kPointLike && !forced) {
			reason = Reason::Spread;
		}
		else if (level < kMinLevel) {
			reason = Reason::Quiet;
		}

		if ((rayCount > 1 || !mono) && info && !info->mShapeLogged) {
			// Plan items "stereo 3D voices as two objects" and "multi-position emitters": neither has shown up in
			// the logs so far; this says what they are if they do.
			info->mShapeLogged = true;
			char chain[200];
			LOG("objects: sound %u is %s (channel mask 0x%X, %u rays, r %.1f m, gain %.3f, point-likeness %.2f), bank %s",
				soundID, rayCount > 1 ? "multi-position" : "not mono", channelMask, rayCount, rays[0].r, gainNext, pointness,
				sounds::FormatChain(*info, chain, sizeof(chain)));
		}

		report.mTheta = rays[0].theta;
		report.mPhi = rays[0].phi;
		report.mDistance = rays[0].r;
		report.mLevel = level;
		report.mSoundID = soundID;
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
			*v = { pbi, playingID, 0, gFrame - kMinRoleFrames, 0.0f, false, false, Role::Bed, -1, reason };
		}
		const bool isNew = v->mLastSeen == 0;
		v->mLastSeen = gFrame;
		v->mCandidate = reason == Reason::Candidate;
		// An object rides out its decay: only a clearly spread pan, a bed rule, or losing its mono single-position
		// shape takes the slot away; louder candidates can still outrank it.
		v->mHoldable = !player && !busFx && !bedRule && rayCount == 1 && mono && (forced || pointness >= kPointLike - kSpreadHysteresis);
		v->mScore = level;
		v->mReason = reason;

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
		ApplyEq(eq, v->mSlot, out, spatial::kBlockFrames);

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
		v->mReason = Reason::Object;
		return true;
	}

	bool OnDryMix(const void* cbx, const void* pbi, const void* mixBus, const AkVPLState* state, AkAudioMix* mix,
		telemetry::Voice& report)
	{
		const bool positioned = Route(cbx, pbi, mixBus, state, mix, report);
		if (positioned) {
			++gStats.mReasons[static_cast<int>(report.mReason)];
		}
		return positioned;
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

		// Rank for the next buffer: the candidates plus the objects allowed to keep their slot.
		int order[kMaxVoices];
		int pool = 0;
		int candidates = 0;
		for (int i = 0; i < gVoiceCount; ++i) {
			const Voice& v = gVoices[i];
			candidates += v.mCandidate;
			if (v.mCandidate || (v.mRole == Role::Object && v.mHoldable)) {
				order[pool++] = i;
			}
		}
		gStats.mCandidatesMax = std::max(gStats.mCandidatesMax, candidates);
		auto rank = [](const Voice& v) { return v.mRole == Role::Object ? v.mScore * kKeepBonus : v.mScore; };
		std::sort(order, order + pool, [&](int a, int b) { return rank(gVoices[a]) > rank(gVoices[b]); });

		bool wanted[kMaxVoices] = {};
		for (int n = 0; n < pool && n < static_cast<int>(gTarget); ++n) {
			wanted[order[n]] = true;
		}

		for (int i = 0; i < gVoiceCount; ++i) {
			Voice& v = gVoices[i];
			if (v.mRole != Role::Object || wanted[i] || v.mSlot >= static_cast<int>(gTarget)) {
				continue;
			}
			if (!v.mHoldable) {
				SetRole(v, Role::FadeOut, v.mSlot);
				++gStats.mDisqualified;
			}
			else if (gFrame - v.mRoleSince >= kMinRoleFrames) {
				SetRole(v, Role::FadeOut, v.mSlot);
				++gStats.mOutranked;
			}
		}
		for (int n = 0; n < pool && n < static_cast<int>(gTarget); ++n) {
			Voice& v = gVoices[order[n]];
			if (v.mRole == Role::Bed && v.mCandidate && gFrame - v.mRoleSince >= kMinRoleFrames) {
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

	int ReasonOf(const void* pbi)
	{
		for (int i = 0; i < gVoiceCount; ++i) {
			if (gVoices[i].mPbi == pbi) {
				return static_cast<int>(gVoices[i].mReason);
			}
		}
		return -1;
	}

	void LogStats()
	{
		LOG("objects: target %u, budget %u, slots in use %d; since last: %u instant, %u promoted; demoted %u outranked, "
			"%u disqualified (spread/player/bus fx), %u unrenderable, %u switched off; %u ended as objects; max %d voices tracked, "
			"max %d candidates%s",
			gTarget, gBudget, std::popcount(gSlotsUsed), gStats.mInstant, gStats.mPromoted, gStats.mOutranked,
			gStats.mDisqualified, gStats.mUnrenderable, gStats.mShrunk, gStats.mEnded, gStats.mVoicesMax,
			gStats.mCandidatesMax, gStats.mTableFull ? " (voice table full!)" : "");
		const uint32_t* r = gStats.mReasons;
		LOG("objects: 3D voice mixes: %u object, %u waiting, %u spread, %u quiet, %u not mono, %u multi-position, %u player, "
			"%u bus fx, %u bed rule",
			r[static_cast<int>(Reason::Object)], r[static_cast<int>(Reason::Candidate)], r[static_cast<int>(Reason::Spread)],
			r[static_cast<int>(Reason::Quiet)], r[static_cast<int>(Reason::NotMono)], r[static_cast<int>(Reason::MultiPosition)],
			r[static_cast<int>(Reason::Player)], r[static_cast<int>(Reason::BusFx)], r[static_cast<int>(Reason::BedRule)]);
		gStats = {};
	}
}
