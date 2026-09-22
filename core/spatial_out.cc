#include "spatial_out.hh"

#include <Windows.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <SpatialAudioClient.h>
#include <Functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

#include "log.hh"

namespace spatial
{
	constexpr uint32_t kMaxChannels = 8;
	constexpr uint32_t kRingFrames = 16384; // ~340 ms at 48 kHz
	constexpr uint32_t kBlockRing = 64;     // block metadata entries; the ring holds at most 16 blocks

	// Fill levels in frames. Wwise renders 1024-frame buffers whenever XAudio2 frees a queue slot, so our fill
	// sawtooths by ~1024; the slack below that is what absorbs Wwise stalls (XAudio2's deeper queue hides them
	// from the original path, but not from ours).
	constexpr uint32_t kPrimeFrames = 2048;
	constexpr uint32_t kTargetSlack = 2048;
	constexpr uint32_t kTrimAboveSlack = 3072;
	constexpr uint32_t kTrimWindowPasses = 200; // ~2 s of 10 ms passes

	// A dynamic object whose slot stays empty this many passes is handed back to Windows. Short gaps (a voice
	// ending and the router reusing the slot) keep the object, so it isn't re-activated every few frames.
	constexpr uint32_t kObjectIdlePasses = 30;

	static uint32_t gSampleRate = 0;
	static uint32_t gChannelMask = 0;
	static uint32_t gChannels = 0;
	static uint32_t gStride = 0; // floats per ring frame: bed channels, then kMaxObjects object samples
	static bool gObjectsWanted = false;
	static AudioObjectType gChannelTypes[kMaxChannels] = {};

	// Horizontal speakers of the bed, for folding an object back into it when Windows won't give us one.
	struct PanSpeaker
	{
		float mAzimuth; // degrees, positive = right
		uint32_t mChannel;
	};
	static PanSpeaker gPanSpeakers[kMaxChannels] = {};
	static uint32_t gPanSpeakerCount = 0;

	struct BlockMeta
	{
		uint64_t mStart; // ring frame index of the block's first frame
		uint32_t mFrames;
		uint32_t mActiveMask;
		float mPosition[kMaxObjects][3];
	};

	static std::unique_ptr<float[]> gRing;
	static BlockMeta gBlocks[kBlockRing] = {};
	static std::atomic<uint64_t> gBlockCount{ 0 }; // producer
	static std::atomic<uint64_t> gWritten{ 0 };    // frames; producer: Wwise audio thread
	static std::atomic<uint64_t> gRead{ 0 };       // frames; consumer: render thread
	static std::atomic<bool> gActive{ false };
	static std::atomic<uint32_t> gObjectSlots{ 0 };
	static std::atomic<uint64_t> gOverflowFrames{ 0 };

	static Status gStatus;
	static SRWLOCK gStatusLock = SRWLOCK_INIT;

	void GetStatus(Status& out)
	{
		AcquireSRWLockShared(&gStatusLock);
		out = gStatus;
		ReleaseSRWLockShared(&gStatusLock);
	}

	template <typename F>
	static void UpdateStatus(F&& update)
	{
		AcquireSRWLockExclusive(&gStatusLock);
		update(gStatus);
		ReleaseSRWLockExclusive(&gStatusLock);
	}

	template <typename T>
	static void Release(T*& p)
	{
		if (p) {
			p->Release();
			p = nullptr;
		}
	}

	bool IsActive()
	{
		return gActive.load(std::memory_order_acquire);
	}

	uint32_t ObjectSlots()
	{
		return gObjectSlots.load(std::memory_order_acquire);
	}

	void Push(const float* interleaved, uint32_t frames, const ObjectBlock* objects)
	{
		const uint64_t written = gWritten.load(std::memory_order_relaxed);
		const uint64_t read = gRead.load(std::memory_order_acquire);
		if (frames > kBlockFrames || written - read + frames > kRingFrames) {
			gOverflowFrames.fetch_add(frames, std::memory_order_relaxed);
			return;
		}

		const uint32_t mask = objects ? objects->mActiveMask : 0;
		for (uint32_t i = 0; i < frames; ++i) {
			float* dst = &gRing[((written + i) % kRingFrames) * gStride];
			if (interleaved) {
				const float* src = interleaved + static_cast<size_t>(i) * gChannels;
				for (uint32_t c = 0; c < gChannels; ++c) {
					// PassData clamps the same way before handing the mix to XAudio2.
					dst[c] = std::clamp(src[c], -1.0f, 1.0f);
				}
			}
			else {
				std::memset(dst, 0, gChannels * sizeof(float));
			}
			float* objectDst = dst + gChannels;
			for (uint32_t s = 0; s < kMaxObjects; ++s) {
				objectDst[s] = (mask & (1u << s)) ? objects->mSamples[s][i] : 0.0f;
			}
		}

		const uint64_t blockIndex = gBlockCount.load(std::memory_order_relaxed);
		BlockMeta& block = gBlocks[blockIndex % kBlockRing];
		block.mStart = written;
		block.mFrames = frames;
		block.mActiveMask = mask;
		if (mask) {
			std::memcpy(block.mPosition, objects->mPosition, sizeof(block.mPosition));
		}
		gBlockCount.store(blockIndex + 1, std::memory_order_release);
		gWritten.store(written + frames, std::memory_order_release);
	}

	static bool BuildChannelMap(uint32_t mask)
	{
		// A 5.1 mix puts its surrounds on the BACK bits; the Atmos bed calls those Side.
		const bool hasSide = (mask & (SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT)) != 0;
		gChannels = 0;
		gPanSpeakerCount = 0;
		for (uint32_t bit = 1; bit && bit <= mask; bit <<= 1) {
			if (!(mask & bit)) {
				continue;
			}
			AudioObjectType type = AudioObjectType_None;
			float azimuth = NAN;
			switch (bit) {
			case SPEAKER_FRONT_LEFT: type = AudioObjectType_FrontLeft; azimuth = -30.0f; break;
			case SPEAKER_FRONT_RIGHT: type = AudioObjectType_FrontRight; azimuth = 30.0f; break;
			case SPEAKER_FRONT_CENTER: type = AudioObjectType_FrontCenter; break; // Wwise never pans 3D to center
			case SPEAKER_LOW_FREQUENCY: type = AudioObjectType_LowFrequency; break;
			case SPEAKER_BACK_LEFT:
				type = hasSide ? AudioObjectType_BackLeft : AudioObjectType_SideLeft;
				azimuth = hasSide ? -150.0f : -110.0f;
				break;
			case SPEAKER_BACK_RIGHT:
				type = hasSide ? AudioObjectType_BackRight : AudioObjectType_SideRight;
				azimuth = hasSide ? 150.0f : 110.0f;
				break;
			case SPEAKER_SIDE_LEFT: type = AudioObjectType_SideLeft; azimuth = -90.0f; break;
			case SPEAKER_SIDE_RIGHT: type = AudioObjectType_SideRight; azimuth = 90.0f; break;
			default: break;
			}
			if (type == AudioObjectType_None || gChannels == kMaxChannels) {
				LOG("spatial: unsupported speaker mask 0x%X", mask);
				return false;
			}
			if (!std::isnan(azimuth)) {
				gPanSpeakers[gPanSpeakerCount++] = { azimuth, gChannels };
			}
			gChannelTypes[gChannels++] = type;
		}
		std::sort(gPanSpeakers, gPanSpeakers + gPanSpeakerCount,
			[](const PanSpeaker& a, const PanSpeaker& b) { return a.mAzimuth < b.mAzimuth; });
		return gChannels > 0;
	}

	// Constant-power pan between the two bed speakers around `azimuth` (degrees, positive = right).
	static void PanGains(float azimuth, float* gains)
	{
		std::fill(gains, gains + kMaxChannels, 0.0f);
		if (gPanSpeakerCount == 0) {
			return;
		}
		if (gPanSpeakerCount == 1) {
			gains[gPanSpeakers[0].mChannel] = 1.0f;
			return;
		}
		// Find the pair (a, b) with a <= azimuth < b going round the circle.
		uint32_t b = 0;
		while (b < gPanSpeakerCount && gPanSpeakers[b].mAzimuth <= azimuth) {
			++b;
		}
		const PanSpeaker& lo = gPanSpeakers[(b + gPanSpeakerCount - 1) % gPanSpeakerCount];
		const PanSpeaker& hi = gPanSpeakers[b % gPanSpeakerCount];
		float span = hi.mAzimuth - lo.mAzimuth;
		float offset = azimuth - lo.mAzimuth;
		if (span <= 0.0f) {
			span += 360.0f;
		}
		if (offset < 0.0f) {
			offset += 360.0f;
		}
		const float t = std::clamp(offset / span, 0.0f, 1.0f) * 1.5707964f;
		gains[lo.mChannel] += std::cos(t);
		gains[hi.mChannel] += std::sin(t);
	}

	struct Slot
	{
		ISpatialAudioObject* mObject = nullptr;
		uint32_t mIdlePasses = 0;
		bool mActivationFailed = false; // folded into the bed until the slot goes idle, then retried
	};

	struct Stream
	{
		IMMDevice* mDevice = nullptr;
		ISpatialAudioClient* mClient = nullptr;
		ISpatialAudioObjectRenderStream* mStream = nullptr;
		ISpatialAudioObject* mBed[kMaxChannels] = {};
		Slot mSlots[kMaxObjects] = {};
		uint32_t mSlotCount = 0;
		HANDLE mEvent = nullptr;

		~Stream()
		{
			if (mStream) {
				mStream->Stop();
			}
			for (Slot& slot : mSlots) {
				Release(slot.mObject);
			}
			for (auto*& object : mBed) {
				Release(object);
			}
			Release(mStream);
			Release(mClient);
			Release(mDevice);
			if (mEvent) {
				CloseHandle(mEvent);
			}
		}
	};

	static std::string DescribeMask(AudioObjectType mask)
	{
		static const char* names[] = { "FL", "FR", "C", "LFE", "SL", "SR", "BL", "BR", "TFL", "TFR", "TBL", "TBR",
			"BFL", "BFR", "BBL", "BBR", "BC", "StereoL", "StereoR" };
		std::string text;
		for (int bit = 1; bit <= 19; ++bit) {
			if (mask & (1u << bit)) {
				text += text.empty() ? "" : " ";
				text += names[bit - 1];
			}
		}
		return text;
	}

	// Returns S_OK with a started stream, or a failure HRESULT (logged when `verbose`).
	static HRESULT Open(Stream& s, bool verbose)
	{
		IMMDeviceEnumerator* enumerator = nullptr;
		HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
		if (SUCCEEDED(hr)) {
			hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &s.mDevice);
			Release(enumerator);
		}
		if (FAILED(hr)) {
			if (verbose) LOG("spatial: no default render endpoint (0x%08lX)", hr);
			return hr;
		}

		std::string endpointName = "?";
		IPropertyStore* props = nullptr;
		if (SUCCEEDED(s.mDevice->OpenPropertyStore(STGM_READ, &props))) {
			PROPVARIANT name;
			PropVariantInit(&name);
			if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR) {
				endpointName = logger::ToUtf8(name.pwszVal);
			}
			PropVariantClear(&name);
			Release(props);
		}

		hr = s.mDevice->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void**>(&s.mClient));
		if (FAILED(hr)) {
			if (verbose) LOG("spatial: %s has no spatial audio (0x%08lX); game stays on XAudio2", endpointName.c_str(), hr);
			return hr;
		}

		// Everything below depends on the active spatial format (Atmos, DTS:X, Sonic...), so it's queried, not assumed.
		UINT32 maxDynamic = 0;
		s.mClient->GetMaxDynamicObjectCount(&maxDynamic);
		AudioObjectType nativeMask = AudioObjectType_None;
		s.mClient->GetNativeStaticObjectTypeMask(&nativeMask);

		// Object format: mono float at the engine rate. We don't resample, so it has to match Wwise's rate.
		WAVEFORMATEX format = {};
		format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
		format.nChannels = 1;
		format.nSamplesPerSec = gSampleRate;
		format.wBitsPerSample = 32;
		format.nBlockAlign = 4;
		format.nAvgBytesPerSec = gSampleRate * 4;
		hr = s.mClient->IsAudioObjectFormatSupported(&format);
		if (FAILED(hr)) {
			if (verbose) LOG("spatial: %s doesn't take %u Hz float objects (0x%08lX)", endpointName.c_str(), gSampleRate, hr);
			return hr;
		}

		AudioObjectType mask = AudioObjectType_None;
		for (uint32_t c = 0; c < gChannels; ++c) {
			mask = static_cast<AudioObjectType>(mask | gChannelTypes[c]);
		}
		if ((mask & ~nativeMask) != 0) {
			// Allowed (Windows folds non-native statics), but worth knowing when a new format behaves oddly.
			LOG("spatial: bed channels [%s] aren't all native to this format [%s]",
				DescribeMask(mask).c_str(), DescribeMask(nativeMask).c_str());
		}

		s.mSlotCount = gObjectsWanted ? std::min(maxDynamic, kMaxObjects) : 0;

		s.mEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		SpatialAudioObjectRenderStreamActivationParams params = {};
		params.ObjectFormat = &format;
		params.StaticObjectTypeMask = mask;
		params.MinDynamicObjectCount = 0;
		params.MaxDynamicObjectCount = s.mSlotCount;
		params.Category = AudioCategory_GameEffects;
		params.EventHandle = s.mEvent;

		PROPVARIANT blob;
		PropVariantInit(&blob);
		blob.vt = VT_BLOB;
		blob.blob.cbSize = sizeof(params);
		blob.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
		hr = s.mClient->ActivateSpatialAudioStream(&blob, __uuidof(ISpatialAudioObjectRenderStream), reinterpret_cast<void**>(&s.mStream));
		if (FAILED(hr)) {
			if (verbose) LOG("spatial: ActivateSpatialAudioStream failed (0x%08lX)", hr);
			return hr;
		}

		for (uint32_t c = 0; c < gChannels; ++c) {
			hr = s.mStream->ActivateSpatialAudioObject(gChannelTypes[c], &s.mBed[c]);
			if (FAILED(hr)) {
				LOG("spatial: bed object 0x%X failed (0x%08lX)", static_cast<unsigned>(gChannelTypes[c]), hr);
				return hr;
			}
		}

		hr = s.mStream->Start();
		if (FAILED(hr)) {
			LOG("spatial: Start failed (0x%08lX)", hr);
			return hr;
		}

		LOG("spatial: stream started on %s: bed [%s] (%u ch), %u Hz, %u dynamic objects (format allows %u, native statics [%s])",
			endpointName.c_str(), DescribeMask(mask).c_str(), gChannels, gSampleRate, s.mSlotCount, maxDynamic,
			DescribeMask(nativeMask).c_str());
		UpdateStatus([&](Status& status) {
			status = {};
			strncpy_s(status.mEndpoint, endpointName.c_str(), _TRUNCATE);
			strncpy_s(status.mBed, DescribeMask(mask).c_str(), _TRUNCATE);
			status.mSlots = s.mSlotCount;
			status.mFormatMax = maxDynamic;
		});
		return S_OK;
	}

	struct Stats
	{
		uint64_t mPasses = 0;
		uint64_t mFrames = 0;
		uint64_t mUnderrunFrames = 0;
		uint64_t mUnderruns = 0;
		uint64_t mTrimmedFrames = 0;
		uint32_t mFillMin = UINT32_MAX;
		uint32_t mFillMax = 0;
		float mPeak[kMaxChannels] = {};
		uint32_t mObjectsMax = 0;       // slots with sound in one pass
		uint64_t mObjectPasses = 0;     // sum over passes of slots with sound (for the average)
		uint32_t mWindowsObjectsMax = 0; // Windows objects held at once
		uint64_t mActivations = 0;
		uint64_t mReleases = 0;
		uint64_t mActivationFailures = 0;
		uint64_t mFoldedPasses = 0;     // slot-passes folded into the bed
		float mObjectPeak = 0.0f;
	};

	static void LogStats(Stats& stats)
	{
		char peaks[160] = {};
		size_t used = 0;
		for (uint32_t c = 0; c < gChannels && used < sizeof(peaks); ++c) {
			const float db = stats.mPeak[c] > 0.0f ? 20.0f * std::log10(stats.mPeak[c]) : -99.0f;
			used += static_cast<size_t>(snprintf(peaks + used, sizeof(peaks) - used, " %.0f", db));
		}
		LOG("spatial: %llu passes, underruns %llu (%llu frames), trimmed %llu, overflow %llu, fill %u..%u, bed peak dBFS:%s",
			stats.mPasses, stats.mUnderruns, stats.mUnderrunFrames, stats.mTrimmedFrames, gOverflowFrames.exchange(0),
			stats.mFillMin == UINT32_MAX ? 0 : stats.mFillMin, stats.mFillMax, peaks);
		LOG("spatial: objects: avg %.1f max %u sounding, max %u held, %llu activations, %llu released, %llu failed, "
			"%llu slot-passes folded into bed, peak %.0f dBFS",
			stats.mPasses ? static_cast<double>(stats.mObjectPasses) / stats.mPasses : 0.0, stats.mObjectsMax,
			stats.mWindowsObjectsMax, stats.mActivations, stats.mReleases, stats.mActivationFailures, stats.mFoldedPasses,
			stats.mObjectPeak > 0.0f ? 20.0f * std::log10(stats.mObjectPeak) : -99.0f);
		stats = {};
	}

	// Finds the blocks covering ring frames [first, first + count): at most two, since a pass (480 frames) is
	// shorter than a block (1024). `cursor` is the consumer's block index, advanced monotonically.
	static void FindBlocks(uint64_t& cursor, uint64_t first, uint32_t count, const BlockMeta*& a, const BlockMeta*& b)
	{
		a = b = nullptr;
		const uint64_t blocks = gBlockCount.load(std::memory_order_acquire);
		if (blocks > kBlockRing && cursor < blocks - kBlockRing) {
			cursor = blocks - kBlockRing;
		}
		while (cursor < blocks) {
			const BlockMeta& block = gBlocks[cursor % kBlockRing];
			if (block.mStart + block.mFrames > first) {
				break;
			}
			++cursor;
		}
		if (cursor < blocks && gBlocks[cursor % kBlockRing].mStart < first + count) {
			a = &gBlocks[cursor % kBlockRing];
			if (cursor + 1 < blocks && gBlocks[(cursor + 1) % kBlockRing].mStart < first + count) {
				b = &gBlocks[(cursor + 1) % kBlockRing];
			}
		}
	}

	// Renders until the stream fails. Returns the failing HRESULT.
	static HRESULT Run(Stream& s)
	{
		// The ring may hold frames pushed before the stream existed; start from "now".
		gRead.store(gWritten.load(std::memory_order_acquire), std::memory_order_release);
		uint64_t blockCursor = gBlockCount.load(std::memory_order_acquire);
		gActive.store(true, std::memory_order_release);
		gObjectSlots.store(s.mSlotCount, std::memory_order_release);

		bool primed = false;
		uint32_t windowMinSlack = UINT32_MAX;
		uint32_t windowPasses = 0;
		uint32_t timeouts = 0;
		uint32_t loggedFailures = 0;
		Stats stats;
		// Stats are per 10 s log period; the menu shows totals since the stream opened.
		uint64_t totalUnderruns = 0, totalFailures = 0, totalFolded = 0;
		uint32_t statusPasses = 0;
		ULONGLONG lastStats = GetTickCount64();

		for (;;) {
			if (WaitForSingleObject(s.mEvent, 100) != WAIT_OBJECT_0) {
				// The event stops when the stream dies without Begin/End ever failing (device removed).
				if (++timeouts >= 20) {
					return AUDCLNT_E_DEVICE_INVALIDATED;
				}
				continue;
			}
			timeouts = 0;

			UINT32 availableDynamic = 0, frameCount = 0;
			HRESULT hr = s.mStream->BeginUpdatingAudioObjects(&availableDynamic, &frameCount);
			if (FAILED(hr)) {
				return hr;
			}

			float* out[kMaxChannels] = {};
			for (uint32_t c = 0; c < gChannels; ++c) {
				BYTE* buffer = nullptr;
				UINT32 bytes = 0;
				hr = s.mBed[c]->GetBuffer(&buffer, &bytes);
				if (FAILED(hr)) {
					s.mStream->EndUpdatingAudioObjects();
					return hr;
				}
				out[c] = reinterpret_cast<float*>(buffer);
			}

			uint64_t read = gRead.load(std::memory_order_relaxed);
			const uint64_t written = gWritten.load(std::memory_order_acquire);
			const uint32_t fill = static_cast<uint32_t>(written - read);
			stats.mFillMin = std::min(stats.mFillMin, fill);
			stats.mFillMax = std::max(stats.mFillMax, fill);

			if (!primed && fill >= kPrimeFrames) {
				primed = true;
			}

			uint32_t copied = 0;
			if (primed) {
				copied = std::min(fill, frameCount);
				for (uint32_t i = 0; i < copied; ++i) {
					const float* frame = &gRing[((read + i) % kRingFrames) * gStride];
					for (uint32_t c = 0; c < gChannels; ++c) {
						out[c][i] = frame[c];
						stats.mPeak[c] = std::max(stats.mPeak[c], std::fabs(frame[c]));
					}
				}
				if (copied < frameCount) {
					// Ran dry (Wwise stalled or stopped rendering): wait for a full prime again instead of
					// stuttering on every buffer that trickles in.
					++stats.mUnderruns;
					stats.mUnderrunFrames += frameCount - copied;
					primed = false;
				}
				else {
					windowMinSlack = std::min(windowMinSlack, fill - copied);
				}
			}
			for (uint32_t c = 0; c < gChannels; ++c) {
				std::fill(out[c] + copied, out[c] + frameCount, 0.0f);
			}

			// Dynamic objects for the same frames.
			const BlockMeta* blockA = nullptr;
			const BlockMeta* blockB = nullptr;
			if (copied) {
				FindBlocks(blockCursor, read, copied, blockA, blockB);
			}
			const uint32_t activeMask = (blockA ? blockA->mActiveMask : 0) | (blockB ? blockB->mActiveMask : 0);
			uint32_t sounding = 0;
			uint32_t held = 0;
			for (uint32_t slotIndex = 0; slotIndex < s.mSlotCount; ++slotIndex) {
				Slot& slot = s.mSlots[slotIndex];
				const uint32_t bit = 1u << slotIndex;

				if (!(activeMask & bit)) {
					slot.mActivationFailed = false;
					if (slot.mObject) {
						BYTE* buffer = nullptr;
						UINT32 bytes = 0;
						if (SUCCEEDED(slot.mObject->GetBuffer(&buffer, &bytes))) {
							std::memset(buffer, 0, static_cast<size_t>(frameCount) * sizeof(float));
						}
						if (++slot.mIdlePasses >= kObjectIdlePasses) {
							slot.mObject->SetEndOfStream(0);
							Release(slot.mObject);
							++stats.mReleases;
						}
						else {
							++held;
						}
					}
					continue;
				}

				++sounding;
				slot.mIdlePasses = 0;
				// The block that holds the start of this pass decides the position (it moves per 21 ms block).
				const BlockMeta* posBlock = (blockA && (blockA->mActiveMask & bit)) ? blockA : blockB;
				const float* pos = posBlock->mPosition[slotIndex];

				if (!slot.mObject && !slot.mActivationFailed) {
					hr = s.mStream->ActivateSpatialAudioObject(AudioObjectType_Dynamic, &slot.mObject);
					if (FAILED(hr)) {
						slot.mObject = nullptr;
						slot.mActivationFailed = true;
						++stats.mActivationFailures;
						if (loggedFailures++ < 10) {
							LOG("spatial: dynamic object for slot %u failed (0x%08lX, %u available), folding it into the bed",
								slotIndex, hr, availableDynamic);
						}
					}
					else {
						++stats.mActivations;
					}
				}

				if (slot.mObject) {
					++held;
					BYTE* buffer = nullptr;
					UINT32 bytes = 0;
					if (SUCCEEDED(slot.mObject->GetBuffer(&buffer, &bytes))) {
						float* dst = reinterpret_cast<float*>(buffer);
						for (uint32_t i = 0; i < copied; ++i) {
							dst[i] = gRing[((read + i) % kRingFrames) * gStride + gChannels + slotIndex];
							stats.mObjectPeak = std::max(stats.mObjectPeak, std::fabs(dst[i]));
						}
						std::fill(dst + copied, dst + frameCount, 0.0f);
					}
					slot.mObject->SetPosition(pos[0], pos[1], pos[2]);
				}
				else {
					// Windows won't give us an object: never drop the sound, pan it into the bed ourselves.
					float gains[kMaxChannels];
					PanGains(std::atan2(pos[0], -pos[2]) * 57.29578f, gains);
					for (uint32_t i = 0; i < copied; ++i) {
						const float sample = gRing[((read + i) % kRingFrames) * gStride + gChannels + slotIndex];
						for (uint32_t c = 0; c < gChannels; ++c) {
							out[c][i] += sample * gains[c];
						}
					}
					++stats.mFoldedPasses;
				}
			}
			stats.mObjectsMax = std::max(stats.mObjectsMax, sounding);
			stats.mObjectPasses += sounding;
			stats.mWindowsObjectsMax = std::max(stats.mWindowsObjectsMax, held);
			if (++statusPasses >= 10) {
				statusPasses = 0;
				UpdateStatus([&](Status& status) {
					status.mActive = true;
					status.mSounding = sounding;
					status.mHeld = held;
					status.mFill = fill;
					status.mUnderruns = totalUnderruns + stats.mUnderruns;
					status.mActivationFailures = totalFailures + stats.mActivationFailures;
					status.mFoldedPasses = totalFolded + stats.mFoldedPasses;
				});
			}

			read += copied;
			if (++windowPasses >= kTrimWindowPasses) {
				// Slack never went below this for ~2 s: it's pure latency (usually the burst Wwise renders to
				// fill XAudio2's queue at startup). Drop it in one go.
				if (primed && windowMinSlack != UINT32_MAX && windowMinSlack > kTrimAboveSlack) {
					const uint32_t drop = windowMinSlack - kTargetSlack;
					read += drop;
					stats.mTrimmedFrames += drop;
				}
				windowMinSlack = UINT32_MAX;
				windowPasses = 0;
			}

			gRead.store(read, std::memory_order_release);

			hr = s.mStream->EndUpdatingAudioObjects();
			if (FAILED(hr)) {
				return hr;
			}

			++stats.mPasses;
			stats.mFrames += frameCount;
			const ULONGLONG now = GetTickCount64();
			if (now - lastStats >= 10000) {
				totalUnderruns += stats.mUnderruns;
				totalFailures += stats.mActivationFailures;
				totalFolded += stats.mFoldedPasses;
				LogStats(stats);
				lastStats = now;
			}
		}
	}

	static DWORD WINAPI ThreadMain(void*)
	{
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		DWORD taskIndex = 0;
		AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);

		// Retries forever (spatial sound may be switched on later, devices come and go), but only logs the
		// first failure after each working period so a machine without spatial sound doesn't fill the log.
		bool verbose = true;
		for (;;) {
			HRESULT hr;
			{
				Stream stream;
				hr = Open(stream, verbose);
				verbose = false;
				if (SUCCEEDED(hr)) {
					hr = Run(stream);
					gObjectSlots.store(0, std::memory_order_release);
					UpdateStatus([](Status& status) { status.mActive = false; status.mSounding = status.mHeld = 0; });
					gActive.store(false, std::memory_order_release);
					LOG("spatial: stream stopped (0x%08lX), game back on XAudio2 until it reopens", hr);
					verbose = true;
				}
			}
			Sleep(hr == SPTLAUDCLNT_E_RESOURCES_INVALIDATED || hr == AUDCLNT_E_DEVICE_INVALIDATED ? 500 : 3000);
		}
	}

	static DWORD WINAPI ProbeMain(void* result)
	{
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		IMMDeviceEnumerator* enumerator = nullptr;
		IMMDevice* device = nullptr;
		ISpatialAudioClient* client = nullptr;
		if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
			SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
			SUCCEEDED(device->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void**>(&client)))) {
			*static_cast<bool*>(result) = true;
		}
		Release(client);
		Release(device);
		Release(enumerator);
		CoUninitialize();
		return 0;
	}

	bool IsAvailable()
	{
		// On its own MTA thread: the caller is a game thread whose COM state we don't own.
		bool* available = new bool(false);
		HANDLE thread = CreateThread(nullptr, 0, ProbeMain, available, 0, nullptr);
		if (!thread) {
			delete available;
			return false;
		}
		if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) {
			// Hung audio stack: give up and leak the flag, the thread may still write to it.
			CloseHandle(thread);
			return false;
		}
		CloseHandle(thread);
		const bool result = *available;
		delete available;
		return result;
	}

	void Start(uint32_t sampleRate, uint32_t channelMask, bool objects)
	{
		static bool started = false;
		if (started) {
			return;
		}
		if (!BuildChannelMap(channelMask)) {
			return;
		}
		started = true;
		gSampleRate = sampleRate;
		gChannelMask = channelMask;
		gObjectsWanted = objects;
		gStride = gChannels + kMaxObjects;
		gRing = std::make_unique<float[]>(static_cast<size_t>(kRingFrames) * gStride);

		HANDLE thread = CreateThread(nullptr, 0, ThreadMain, nullptr, 0, nullptr);
		if (thread) {
			CloseHandle(thread);
		}
		else {
			LOG("spatial: CreateThread failed (%lu)", GetLastError());
		}
	}
}
