#pragma once

// Just enough of Wwise 2012.2's internals (statically linked into sdhdship.exe) to observe and redirect its
// output. Layouts come from the legacy build's PDB (reference\SDmodding\game-itself); the Wwise library is
// the same in the installed build, only its code moved by a few dozen bytes.

#include <cstddef>
#include <cstdint>

namespace wwise
{
	using AKRESULT = int32_t;
	constexpr AKRESULT AK_Success = 1;

	// SPEAKER_* layout Wwise uses for the final mix (WAVEFORMATEXTENSIBLE order when interleaved).
	constexpr uint32_t kSpeakers71 = 0x63F; // FL FR FC LFE BL BR SL SR
	constexpr uint32_t kFramesPerBuffer = 1024; // AK_NUM_VOICE_REFILL_FRAMES

	struct AkAudioBuffer
	{
		void* pData;
		uint32_t uChannelMask;
		AKRESULT eState;
		uint16_t uMaxFrames;
		uint16_t uValidFrames;
	};
	static_assert(sizeof(AkAudioBuffer) == 0x18);

	// AkVPLState = AkPipelineBuffer (AkAudioBuffer + markers + position info) + flags.
	struct AkVPLState
	{
		AkAudioBuffer buffer;
		uint8_t pad18[0x38 - 0x18];
		AKRESULT result;
		bool bPause;
		bool bStop;
		bool bIsAuxRoutable;
		bool bAudible;
	};
	static_assert(sizeof(AkVPLState) == 0x40);

	// Per input channel: gain of that channel into each of the 8 output speakers, at the end (Next) and
	// start (Previous) of the buffer; the mixer ramps between them.
	struct AkAudioMix
	{
		float next[8];
		float previous[8];
	};
	static_assert(sizeof(AkAudioMix) == 0x40);

	struct CAkVPLSrcCbxNode;

	struct AkRunningVPL
	{
		AkVPLState state;
		CAkVPLSrcCbxNode* pCbx;
		void* pFeedbackData;
		bool bFeedbackVPL;
	};
	static_assert(offsetof(AkRunningVPL, pCbx) == 0x40 && offsetof(AkRunningVPL, bFeedbackVPL) == 0x50);

	// One emitter position as seen by one listener, in Wwise's spherical coordinates.
	struct AkRayVolumeData
	{
		float r;
		float theta;
		float phi;
		float fEmitterAngle;
		uint8_t flags;
		uint8_t uListenerMask;
		uint8_t pad12[2];
		float fDryMixGain;
		float fGameDefAuxMixGain;
		float fUserDefAuxMixGain;
		float fConeInterp;
	};
	static_assert(sizeof(AkRayVolumeData) == 36);

	// AkVPL: a mix bus instance. Starts with its CAkVPLMixBusNode (CAkBusFX : CAkBusVolumes), so the node
	// pointer passed to CAkVPLMixBusNode::ConsumeBuffer is also the AkVPL pointer. The device's master bus is
	// a CAkVPLFinalMixNode, which is a CAkBusFX too (same FX offsets) but not an AkVPL.
	namespace vpl
	{
		constexpr size_t kBusID = 0x408;          // uint32 (CAkBusVolumes)
		constexpr size_t kParent = 0x410;         // AkVPL*: the bus this one mixes into, null = the device's final mix
		constexpr size_t kFx = 0x480;             // CAkBusFX::FX[4] insert effects
		constexpr size_t kBypassAllFx = 0x520;    // bit 0
		constexpr size_t kDownstreamGain = 0x550; // float
		constexpr size_t kDevice = 0x558;         // uint64 device ID (AkVPL)
		constexpr uint32_t kFxSlots = 4;
	}

	namespace bus_fx // CAkBusFX::FX (40 bytes)
	{
		constexpr size_t kStride = 40;
		constexpr size_t kID = 0x0;     // AkPluginID
		constexpr size_t kEffect = 0x10; // IAkInPlaceEffectPlugin*, null = empty slot
		constexpr size_t kFlags = 0x20;  // bit 0 bBypass
	}

	namespace device // AkDevice (CAkOutputMgr::m_Devices items, 0x50 bytes; the AkArray is pItems*, uint32 length)
	{
		constexpr size_t kStride = 0x50;
		constexpr size_t kFinalMix = 0x0; // CAkVPLFinalMixNode*
		constexpr size_t kID = 0x18;      // uint64
	}

	// AkPluginID = type | company << 4 | plugin << 16 (AKMAKECLASSID); type 3 = effect, company 0 = Audiokinetic.
	constexpr uint32_t PluginType(uint32_t id) { return id & 0xF; }
	constexpr uint32_t PluginCompany(uint32_t id) { return (id >> 4) & 0xFFF; }
	constexpr uint32_t PluginIndex(uint32_t id) { return id >> 16; }
	constexpr uint32_t kMeterFx = 3 | (0x81u << 16);        // Wwise Meter: measures, doesn't change the audio
	constexpr uint32_t kParametricEqFx = 3 | (0x69u << 16); // Wwise Parametric EQ: objects apply it themselves

	// CAkParametricEQFX, the instance behind a Parametric EQ slot. Execute() runs each enabled band as a biquad
	// y = c0 x + c1 x1 + c2 x2 + c3 y1 + c4 y2 (c3/c4 already negated and normalized), recomputing a band's
	// coefficients first when its dirty flag is set, then ramps to the output level.
	namespace eq_fx
	{
		constexpr size_t kCoefs = 0x8;         // float[3][5]
		constexpr size_t kSharedParams = 0x48; // CAkParameterEQFXParams*
		constexpr uint32_t kBands = 3;
	}

	namespace eq_params // CAkParameterEQFXParams
	{
		constexpr size_t kBand = 0x8;        // EQModuleParams[3]: type, gain dB, frequency, Q, on/off
		constexpr size_t kBandStride = 20;
		constexpr size_t kBandOn = 0x10;
		constexpr size_t kOutputLevel = 0x44; // float dB
		constexpr size_t kBandDirty = 0x4C;   // bool[3]
	}

	namespace device_info // AkDeviceInfo, one per output device a voice feeds
	{
		constexpr size_t kNext = 0x248;          // AkDeviceInfo*
		constexpr size_t kMixBus = 0x250;        // AkVPL*: the voice's dry output bus on this device
		constexpr size_t kCrossDeviceSend = 0x264; // bool
	}

	namespace cbx // CAkVPLSrcCbxNode(Base)
	{
		constexpr size_t kSources = 0x10;       // CAkVPLSrcNode*[2]
		constexpr size_t kVolumeData = 0x28;    // AkArray<AkRayVolumeData>: items*, uint32 length
		constexpr size_t kDevices = 0x50;       // AkDeviceInfo* (first), then uint32 count
		constexpr size_t kSampleRate = 0x168;   // uint32
	}

	namespace src_node // CAkVPLSrcNode
	{
		constexpr size_t kContext = 0x18; // CAkPBI*
	}

	namespace pbi // CAkPBI: one playing sound instance
	{
		constexpr size_t kPlayingID = 0x70;  // m_UserParams (0x58) .m_PlayingID (+0x18)
		constexpr size_t kSound = 0x98;      // CAkSoundBase*, a CAkIndexable: ID (sound object ID) at +0x10
		constexpr size_t kGameObj = 0xA8;    // CAkRegisteredObj*: m_GameObjID (uint64) at +0x70
		constexpr size_t kPannerBits = 0x173; // bits 0-1 m_ePannerType (0 = 2D), bits 2-3 m_ePosSourceType
	}

	namespace indexable // CAkIndexable
	{
		constexpr size_t kID = 0x10; // uint32
	}

	namespace game_obj // CAkRegisteredObj
	{
		constexpr size_t kID = 0x70; // uint64 AkGameObjectID
	}

	namespace sink // CAkSink / CAkSinkXAudio2
	{
		constexpr size_t kSpeakersConfig = 0x10; // uint32
		constexpr size_t kMasterOut = 0x18;      // AkAudioBuffer: final interleaved mix before PassData
		constexpr size_t kType = 0x30;           // AkSinkType
		constexpr size_t kNumChannels = 0x50;    // uint32 (XAudio2 sink)
	}

	template <typename T>
	inline T& At(const void* base, size_t offset)
	{
		return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(base) + offset);
	}
}
