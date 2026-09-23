#include "wwise_hooks.hh"

#include <Windows.h>

#include <MinHook.h>

#include <bit>
#include <cmath>
#include <cstring>

#include "config.hh"
#include "game.hh"
#include "heights.hh"
#include "log.hh"
#include "objects.hh"
#include "scan.hh"
#include "spatial_out.hh"
#include "telemetry.hh"
#include "wwise.hh"

namespace wwise
{
	// Signatures generated from the legacy exe (IDA, legacy PDB names) and checked unique in the installed one.
	constexpr char kSigSinkInit[] = "48 89 5C 24 ? 48 89 74 24 ? 57 48 81 EC B0 04 00 00";   // CAkSinkXAudio2::Init
	constexpr char kSigPassData[] = "40 55 48 83 EC 20 48 8D 6C 24 ? 0F B7 41";              // CAkSinkXAudio2::PassData
	constexpr char kSigPassSilence[] = "40 53 48 83 EC 20 0F B7 41 ? 48 8B D9";              // CAkSinkXAudio2::PassSilence
	constexpr char kSigRunVPL[] = "40 53 55 56 57 41 54 41 56 41 57 48 81 EC 20 02 00 00";   // CAkLEngine::RunVPL
	constexpr char kSigConsumeBuffer[] =                                                     // CAkVPLMixBusNode::ConsumeBuffer(AkVPLState&, AkAudioMix*)
		"48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 66 83 7A ? ? 49 8B F0 48 8B FA 48 8B D9 76";

	// The bus→bus and bus→final-mix variants of ConsumeBuffer (called from CAkLEngine::TransferBuffer once per
	// active bus per frame, after the bus's effects ran): identical prologues, so the signatures run into the
	// first field access that differs (m_bEffectCreated at +0x540 vs the final node's m_eState at +0x530).
	constexpr char kSigBusConsume[] =                                                        // CAkVPLMixBusNode::ConsumeBuffer(AkAudioBufferBus&, bool, AkAudioMix*)
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 66 83 7A 12 00 49 8B F1 41 0F B6 E8 48 8B FA 48 8B D9 76 5A 80 B9 40 05 00 00 00";
	constexpr char kSigFinalConsume[] =                                                      // CAkVPLFinalMixNode::ConsumeBuffer(AkAudioBufferBus*, bool, AkAudioMix*)
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 66 83 7A 12 00 49 8B F1 41 0F B6 E8 48 8B FA 48 8B D9 76 41 83 B9 30 05 00 00 02";

	// AK::SoundEngine::SetPosition(AkGameObjectID, const AkSoundPosition&): null check, then an AkQueuedMsg of
	// type 0xD (68-byte frame).
	constexpr char kSigSetPosition[] =
		"48 83 EC 68 48 85 C9 75 0A B8 02 00 00 00 48 83 C4 68 C3 0F 10 02 F2 0F 10 4A 10 B8 0D 00 00 00 66 89 44 24 22 48 89 4C 24 28";

	// CAkLEngine::AnalyzeMixingGraph, where a top-level bus looks up its device's final mix:
	//   mov r8d, [rip + m_Devices.m_uLength]; mov eax, ebp; test r8d, r8d; jz; mov r11, [rip + m_Devices.m_pItems]
	constexpr char kSigDevices[] = "44 8B 05 ? ? ? ? 8B C5 45 85 C0 74 ? 4C 8B 1D ? ? ? ? 4C 8B 92 58 05 00 00 49 8D 4B 18 4C 39 11";
	constexpr size_t kDevicesItemsLoad = 14; // offset of the mov r11 (RIP disp at +3)

	// CAkSinkXAudio2::Init + 0x4D: mov r9d, [rip + AkAudioLibSettings::g_pipelineCoreFrequency]
	constexpr size_t kInitRateLoad = 0x4D;

	using SinkInitFn = AKRESULT(__fastcall*)(void* self, void* settings, uint32_t channelMask, uint32_t sinkType);
	using SinkPassFn = AKRESULT(__fastcall*)(void* self);
	using RunVPLFn = void(__fastcall*)(AkRunningVPL* vpl);
	using ConsumeBufferFn = void(__fastcall*)(void* mixBus, AkVPLState* state, AkAudioMix* mix);
	using SetPositionFn = AKRESULT(__fastcall*)(uint64_t gameObj, const AkSoundPosition* position);
	using BusConsumeFn = void(__fastcall*)(void* parentBus, AkAudioBufferBus* buffer, bool pan, AkAudioMix* mix);
	using FinalConsumeFn = AKRESULT(__fastcall*)(void* finalMix, AkAudioBufferBus* buffer, bool pan, AkAudioMix* mix);

	static SinkInitFn gSinkInit = nullptr;
	static SinkPassFn gPassData = nullptr;
	static SinkPassFn gPassSilence = nullptr;
	static RunVPLFn gRunVPL = nullptr;
	static ConsumeBufferFn gConsumeBuffer = nullptr;
	static SetPositionFn gSetPosition = nullptr;
	static BusConsumeFn gBusConsume = nullptr;
	static FinalConsumeFn gFinalConsume = nullptr;

	static const uint32_t* gSampleRate = nullptr;
	static const uint8_t* gDevices = nullptr; // CAkOutputMgr::m_Devices: AkDevice* pItems, uint32 length
	static void* gMainSink = nullptr;

	const void* FinalMixOf(uint64_t deviceID)
	{
		if (!gDevices) {
			return nullptr;
		}
		const uint8_t* items = *reinterpret_cast<const uint8_t* const*>(gDevices);
		const uint32_t count = *reinterpret_cast<const uint32_t*>(gDevices + 8);
		for (uint32_t i = 0; items && i < count; ++i) {
			const uint8_t* dev = items + i * device::kStride;
			if (At<uint64_t>(dev, device::kID) == deviceID) {
				return At<void*>(dev, device::kFinalMix);
			}
		}
		return nullptr;
	}

	const char* PluginName(uint32_t id)
	{
		// UFG::WwiseInterface::RegisterPlugins (legacy PDB): the effects this game links.
		const uint32_t index = PluginIndex(id);
		if (PluginCompany(id) == 0x100) {
			return index == 0x67 ? "McDSP ML1" : index == 0x6E ? "McDSP FutzBox" : "McDSP?";
		}
		switch (index) {
		case 0x69: return "ParametricEQ";
		case 0x6A: return "Delay";
		case 0x6C: return "Compressor";
		case 0x73: return "MatrixReverb";
		case 0x74: return "SoundSeedImpact";
		case 0x76: return "RoomVerb";
		case 0x7D: return "Flanger";
		case 0x7F: return "ConvolutionReverb";
		case 0x81: return "Meter";
		case 0x82: return "TimeStretch";
		case 0x83: return "Tremolo";
		case 0x88: return "PitchShifter";
		case 0x8A: return "Harmonizer";
		case 0x8B: return "Gain";
		default: return "?";
		}
	}
	static bool gBedEnabled = false;
	static bool gVoiceHooks = false;
	static uint64_t gBufferCount = 0;

	// ---- Voice recon: what does Wwise know about each 3D voice at mix time? ----

	namespace voices
	{
		constexpr int kMaxSnapshotLines = 16;

		static thread_local AkRunningVPL* tCurrent = nullptr;
		static uint64_t gFrame = 0;
		static uint64_t gNextSnapshot = 0;
		static bool gSnapshot = false;
		static int gSnapshotLines = 0;

		// Per-frame counts and their maxima over the snapshot period.
		static uint32_t gDry = 0, gDry3D = 0, gAux = 0;
		static uint32_t gMaxDry = 0, gMaxDry3D = 0, gMaxAux = 0;

		static void LogVoice(const void* mixBus, const void* cbx, const void* pbi, const AkVPLState* state, const AkAudioMix* mix)
		{
			const uint32_t channels = static_cast<uint32_t>(std::popcount(state->buffer.uChannelMask));
			const uint32_t frames = state->buffer.uValidFrames;

			const void* sound = At<void*>(pbi, pbi::kSound);
			const void* gameObj = At<void*>(pbi, pbi::kGameObj);
			const uint32_t soundID = sound ? At<uint32_t>(sound, indexable::kID) : 0;
			const unsigned long long objID = gameObj ? At<uint64_t>(gameObj, game_obj::kID) : 0;
			const uint8_t pannerBits = At<uint8_t>(pbi, pbi::kPannerBits);

			const auto* rays = At<AkRayVolumeData*>(cbx, cbx::kVolumeData);
			const uint32_t rayCount = At<uint32_t>(cbx, cbx::kVolumeData + 8);

			// Total gain of channel 0 into the mix (pan is power-preserving, so this is attenuation × volume).
			float power = 0.0f;
			for (float g : mix[0].next) {
				power += g * g;
			}

			float rms = 0.0f;
			if (state->buffer.pData && frames) {
				const float* samples = static_cast<const float*>(state->buffer.pData);
				for (uint32_t i = 0; i < frames; ++i) {
					rms += samples[i] * samples[i];
				}
				rms = std::sqrt(rms / frames);
			}

			// Speaker gains are in Wwise's internal order (FL FR C BL BR SL SR LFE, established from the first
			// in-game log: theta -135° lands on index 3, +134° on 4, ±90° on 5/6).
			const float* g = mix[0].next;
			const int slot = objects::SlotOf(pbi);
			char role[16];
			snprintf(role, sizeof(role), slot >= 0 ? "obj%d" : "bed", slot);
			constexpr float kDeg = 57.2957795f;
			if (rays && rayCount) {
				LOG("  voice %-5s snd=%u obj=%llX pan=%u pos=%u ch=%u rays=%u r=%.1f theta=%.0f phi=%.0f dryMix=%.2f | "
					"gain=%.3f down=%.2f rms=%.3f | FL %.2f FR %.2f C %.2f BL %.2f BR %.2f SL %.2f SR %.2f LFE %.2f",
					role, soundID, objID, pannerBits & 3, (pannerBits >> 2) & 3, channels, rayCount, rays[0].r,
					rays[0].theta * kDeg, rays[0].phi * kDeg, rays[0].fDryMixGain, std::sqrt(power),
					At<float>(mixBus, vpl::kDownstreamGain), rms, g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
			}
			else {
				LOG("  voice %-5s snd=%u obj=%llX pan=%u pos=%u ch=%u rays=0 | gain=%.3f down=%.2f rms=%.3f | "
					"FL %.2f FR %.2f C %.2f BL %.2f BR %.2f SL %.2f SR %.2f LFE %.2f",
					role, soundID, objID, pannerBits & 3, (pannerBits >> 2) & 3, channels, std::sqrt(power),
					At<float>(mixBus, vpl::kDownstreamGain), rms, g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
			}
		}

		static void OnAuxMix()
		{
			++gAux;
		}

		// Before the voice router touches `mix`, so the log shows Wwise's own gains.
		static void OnDryMix(const void* mixBus, const void* cbx, const void* pbi, const AkVPLState* state, const AkAudioMix* mix)
		{
			const bool is3D = pbi && (At<uint8_t>(pbi, pbi::kPannerBits) & 3) != 0;
			++gDry;
			if (is3D) {
				++gDry3D;
			}

			// 2D voices get one line per snapshot too, but only after the 3D ones had their chance.
			if (gSnapshot && pbi && gSnapshotLines < kMaxSnapshotLines && (is3D || gSnapshotLines < kMaxSnapshotLines / 2)) {
				++gSnapshotLines;
				LogVoice(mixBus, cbx, pbi, state, mix);
			}
		}

		// Called at the end of every rendered buffer (from PassData / PassSilence).
		static void OnFrameEnd(uint32_t sampleRate)
		{
			gMaxDry = gDry > gMaxDry ? gDry : gMaxDry;
			gMaxDry3D = gDry3D > gMaxDry3D ? gDry3D : gMaxDry3D;
			gMaxAux = gAux > gMaxAux ? gAux : gMaxAux;
			if (gSnapshot) {
				LOG("voices: frame %llu: %u dry (%u 3D), %u aux sends; max over last period %u dry (%u 3D), %u aux",
					gFrame, gDry, gDry3D, gAux, gMaxDry, gMaxDry3D, gMaxAux);
				objects::LogStats();
				gSnapshot = false;
				gMaxDry = gMaxDry3D = gMaxAux = 0;
			}
			gDry = gDry3D = gAux = 0;

			++gFrame;
			if (gFrame >= gNextSnapshot) {
				// The lines are logged while the next frame renders, then summarized at its end.
				gSnapshot = true;
				gSnapshotLines = 0;
				gNextSnapshot = gFrame + (sampleRate ? sampleRate : 48000) * 5 / kFramesPerBuffer;
				LOG("voices: snapshot of frame %llu", gFrame);
			}
		}
	}

	// ---- Hooks ----

	static void __fastcall RunVPLHook(AkRunningVPL* vpl)
	{
		voices::tCurrent = vpl;
		gRunVPL(vpl);
		voices::tCurrent = nullptr;
	}

	static bool IsDryBus(const void* cbx, const void* mixBus)
	{
		for (const void* device = At<void*>(cbx, cbx::kDevices); device; device = At<void*>(device, device_info::kNext)) {
			if (!At<bool>(device, device_info::kCrossDeviceSend) && At<void*>(device, device_info::kMixBus) == mixBus) {
				return true;
			}
		}
		return false;
	}

	static void __fastcall ConsumeBufferHook(void* mixBus, AkVPLState* state, AkAudioMix* mix)
	{
		// Only mixes made from inside RunVPL are voices; aux sends (reverb) stay untouched and so stay in the bed.
		const AkRunningVPL* vpl = voices::tCurrent;
		if (vpl && !vpl->bFeedbackVPL && vpl->pCbx) {
			const void* cbx = vpl->pCbx;
			if (IsDryBus(cbx, mixBus)) {
				const void* source = At<void*>(cbx, cbx::kSources);
				const void* pbi = source ? At<void*>(source, src_node::kContext) : nullptr;
				if (gConfig.mVoiceLog) {
					voices::OnDryMix(mixBus, cbx, pbi, state, mix);
				}
				telemetry::Voice report;
				if (pbi && objects::OnDryMix(cbx, pbi, mixBus, state, mix, report)) {
					telemetry::Add(report);
				}
				else {
					telemetry::AddUnpositioned();
				}
			}
			else if (gConfig.mVoiceLog) {
				voices::OnAuxMix();
			}
		}
		gConsumeBuffer(mixBus, state, mix);
	}

	// A bus hands its processed output to its parent (or to the device's final mix). The buffer is the source
	// bus's own m_BufferOut, which is how the source AkVPL is recovered; the height bed takes its share here.
	static void __fastcall BusConsumeHook(void* parentBus, AkAudioBufferBus* buffer, bool pan, AkAudioMix* mix)
	{
		const void* source = reinterpret_cast<const uint8_t*>(buffer) - vpl::kBufferOut;
		heights::OnBusTransfer(source, buffer, At<float>(parentBus, vpl::kDownstreamGain));
		gBusConsume(parentBus, buffer, pan, mix);
	}

	static AKRESULT __fastcall FinalConsumeHook(void* finalMix, AkAudioBufferBus* buffer, bool pan, AkAudioMix* mix)
	{
		const void* source = reinterpret_cast<const uint8_t*>(buffer) - vpl::kBufferOut;
		// A top-level bus's remaining chain is the final mix node's own volume (what AnalyzeMixingGraph uses too).
		heights::OnBusTransfer(source, buffer, At<float>(finalMix, vpl::kNextVolume));
		return gFinalConsume(finalMix, buffer, pan, mix);
	}

	// End of a rendered buffer on the main sink: hand bed + heights + objects to the spatial stream.
	static void FinishBuffer(void* self, bool silence)
	{
		const spatial::ObjectBlock* block = objects::FinishFrame();
		const float* const* heightBlock = heights::FinishFrame();
		if (spatial::IsActive()) {
			if (silence) {
				spatial::Push(nullptr, kFramesPerBuffer, heightBlock, block);
			}
			else {
				// Take the mix before PassData hands it to XAudio2 (XAudio2 reads its ring asynchronously, so
				// zeroing afterwards would race), then let XAudio2 play silence.
				AkAudioBuffer& out = At<AkAudioBuffer>(self, sink::kMasterOut);
				const uint32_t channels = At<uint32_t>(self, sink::kNumChannels);
				if (out.pData && out.uValidFrames) {
					spatial::Push(static_cast<const float*>(out.pData), out.uValidFrames, heightBlock, block);
					std::memset(out.pData, 0, static_cast<size_t>(channels) * out.uValidFrames * sizeof(float));
				}
			}
		}
		objects::StartFrame();
		heights::StartFrame();
		telemetry::Publish(++gBufferCount, objects::Target());
		if (gConfig.mVoiceLog) {
			voices::OnFrameEnd(gSampleRate ? *gSampleRate : 0);
		}
	}

	// Game thread. Characters' audio entities sit at the character root (the feet), so NPC speech comes from
	// the ground; as objects with real elevation that is audible, in the 7.1 bed it never was (no height). Lift
	// the position of actor audio components towards head height before Wwise sees it. Only the direction and
	// a slightly larger distance change; the game's own occlusion, distance RTPCs and regions use its own copy
	// of the position.
	static AKRESULT __fastcall SetPositionHook(uint64_t gameObj, const AkSoundPosition* position)
	{
		const float lift = gConfig.mActorLift.load(std::memory_order_relaxed);
		uint32_t typeUID = 0;
		if (lift > 0.0f && position && game::IsPointer(gameObj) &&
			game::ReadU32(gameObj - game::actor_audio::kEntityBase + game::sim_component::kTypeUID, typeUID) &&
			typeUID == game::actor_audio::kTypeUID) {
			AkSoundPosition lifted = *position;
			lifted.position[1] += lift;
			return gSetPosition(gameObj, &lifted);
		}
		return gSetPosition(gameObj, position);
	}

	static AKRESULT __fastcall SinkInitHook(void* self, void* settings, uint32_t channelMask, uint32_t sinkType)
	{
		uint32_t mask = channelMask;
		// Wwise picks its layout from XAudio2's device (the endpoint's mix format), which needn't be 7.1 even when
		// the spatial renderer has a 7.1.4 bed. With spatial audio available, always mix 7.1.
		if (gBedEnabled && mask == 0 && !gMainSink) {
			const bool available = spatial::IsAvailable();
			LOG("sink: spatial audio %s on the default endpoint", available ? "available" : "not available");
			if (available) {
				mask = kSpeakers71;
			}
		}

		const AKRESULT result = gSinkInit(self, settings, mask, sinkType);
		const uint32_t speakers = At<uint32_t>(self, sink::kSpeakersConfig);
		const uint32_t channels = At<uint32_t>(self, sink::kNumChannels);
		const uint32_t rate = gSampleRate ? *gSampleRate : 0;
		LOG("sink: CAkSinkXAudio2::Init(this=%p, mask 0x%X -> 0x%X, type %u) = %d: speakers 0x%X, %u channels, %u Hz",
			self, channelMask, mask, sinkType, result, speakers, channels, rate);

		if (result == AK_Success && !gMainSink) {
			gMainSink = self;
			if (gBedEnabled) {
				if (rate && static_cast<uint32_t>(std::popcount(speakers)) == channels) {
					// Objects and height channels are reserved whenever their hooks can feed them, even if they
					// start switched off (A/B).
					heights::Init(rate);
					spatial::Start(rate, speakers, gVoiceHooks, gBusConsume != nullptr);
				}
				else {
					LOG("sink: unexpected layout, not starting the spatial bed");
				}
			}
		}
		return result;
	}

	static AKRESULT __fastcall PassDataHook(void* self)
	{
		if (self == gMainSink) {
			FinishBuffer(self, false);
		}
		return gPassData(self);
	}

	static AKRESULT __fastcall PassSilenceHook(void* self)
	{
		if (self == gMainSink) {
			FinishBuffer(self, true);
		}
		return gPassSilence(self);
	}

	template <typename T>
	static bool Hook(const char* name, void* target, void* detour, T& original)
	{
		if (!target) {
			return false;
		}
		const MH_STATUS status = MH_CreateHook(target, detour, reinterpret_cast<void**>(&original));
		if (status != MH_OK) {
			LOG("hook: %s: MH_CreateHook failed (%d)", name, status);
			return false;
		}
		return true;
	}

	// The MinHook build shipped with SDmodding (reference\SPatch\external) is a reduced fork: no
	// MH_Initialize / MH_EnableHook, MH_CreateHook enables the hook immediately, MH_RemoveHook frees the
	// trampoline. Fine here: we hook from DllMain before any game thread exists.
	void Install()
	{
		uint8_t* sinkInit = scan::FindUnique("CAkSinkXAudio2::Init", kSigSinkInit);
		uint8_t* passData = scan::FindUnique("CAkSinkXAudio2::PassData", kSigPassData);
		uint8_t* passSilence = scan::FindUnique("CAkSinkXAudio2::PassSilence", kSigPassSilence);

		if (sinkInit) {
			static constexpr uint8_t kMovR9dRip[] = { 0x44, 0x8B, 0x0D };
			if (std::memcmp(sinkInit + kInitRateLoad, kMovR9dRip, sizeof(kMovR9dRip)) == 0) {
				gSampleRate = static_cast<const uint32_t*>(scan::RipTarget(sinkInit + kInitRateLoad + 3));
			}
			else {
				LOG("scan: sample rate load not where expected in CAkSinkXAudio2::Init");
			}
		}

		const bool sinkOk = sinkInit && passData && passSilence && gSampleRate &&
			Hook("SinkInit", sinkInit, &SinkInitHook, gSinkInit) &&
			Hook("PassData", passData, &PassDataHook, gPassData) &&
			Hook("PassSilence", passSilence, &PassSilenceHook, gPassSilence);
		gBedEnabled = sinkOk && gConfig.mSpatialBed;
		LOG("hook: sink hooks %s, spatial bed %s", sinkOk ? "ready" : "MISSING", gBedEnabled ? "on" : "off");

		// Needed whenever the bed runs, even with objects off at start: they can be switched on in-game (A/B),
		// and the HUD shows the voices either way.
		if ((gConfig.mVoiceLog || gBedEnabled) && sinkOk) {
			uint8_t* runVPL = scan::FindUnique("CAkLEngine::RunVPL", kSigRunVPL);
			uint8_t* consume = scan::FindUnique("CAkVPLMixBusNode::ConsumeBuffer", kSigConsumeBuffer);
			if (uint8_t* devices = scan::FindUnique("CAkOutputMgr::m_Devices", kSigDevices)) {
				// Without it only the master bus's effects go unseen; the router then treats it as clean.
				gDevices = static_cast<const uint8_t*>(scan::RipTarget(devices + kDevicesItemsLoad + 3));
			}
			gVoiceHooks = Hook("RunVPL", runVPL, &RunVPLHook, gRunVPL) &&
				Hook("ConsumeBuffer", consume, &ConsumeBufferHook, gConsumeBuffer);
			if (!gVoiceHooks) {
				// Half a pair is useless (ConsumeBuffer needs RunVPL's context); drop whichever was created.
				if (gRunVPL) MH_RemoveHook(runVPL);
				if (gConsumeBuffer) MH_RemoveHook(consume);
			}
			LOG("hook: voice hooks %s, dynamic objects %s at start (max %d, %.1f m)", gVoiceHooks ? "ready" : "MISSING",
				gVoiceHooks && gBedEnabled && gConfig.mObjects ? "on" : "off", gConfig.mMaxObjects.load(), gConfig.mObjectDistance.load());

			uint8_t* setPosition = scan::FindUnique("AK::SoundEngine::SetPosition", kSigSetPosition);
			const bool liftHook = Hook("SetPosition", setPosition, &SetPositionHook, gSetPosition);
			LOG("hook: actor position hook %s, lift %.2f m", liftHook ? "ready" : "MISSING", gConfig.mActorLift.load());

			// Height bed: both bus transfer variants or nothing (a bus whose parent is the final mix would
			// otherwise never be seen).
			uint8_t* busConsume = scan::FindUnique("CAkVPLMixBusNode::ConsumeBuffer(bus)", kSigBusConsume);
			uint8_t* finalConsume = scan::FindUnique("CAkVPLFinalMixNode::ConsumeBuffer", kSigFinalConsume);
			const bool heightHooks = Hook("BusConsume", busConsume, &BusConsumeHook, gBusConsume) &&
				Hook("FinalConsume", finalConsume, &FinalConsumeHook, gFinalConsume);
			if (!heightHooks) {
				if (gBusConsume) MH_RemoveHook(busConsume);
				if (gFinalConsume) MH_RemoveHook(finalConsume);
				gBusConsume = nullptr;
				gFinalConsume = nullptr;
			}
			LOG("hook: height bed hooks %s, heights %s at start", heightHooks ? "ready" : "MISSING", gConfig.mHeights.load() ? "on" : "off");
		}
	}
}
