#include "telemetry.hh"

#include <Windows.h>

#include <cstddef>
#include <cstring>

namespace telemetry
{
	static Frame gStaging;   // audio thread only
	static Frame gPublished; // guarded by gLock
	static bool gHasFrame = false;
	static SRWLOCK gLock = SRWLOCK_INIT;

	void Add(const Voice& voice)
	{
		++gStaging.mDry;
		if (gStaging.mVoiceCount < kMaxVoices) {
			gStaging.mVoices[gStaging.mVoiceCount++] = voice;
		}
	}

	void AddUnpositioned()
	{
		++gStaging.mDry;
		++gStaging.mUnpositioned;
	}

	void Publish(uint64_t frame, uint32_t objectsTarget)
	{
		gStaging.mFrame = frame;
		gStaging.mObjectsTarget = objectsTarget;
		// Never block the audio thread on the renderer; a skipped snapshot is invisible at 47 per second.
		if (TryAcquireSRWLockExclusive(&gLock)) {
			const size_t used = offsetof(Frame, mVoices) + sizeof(Voice) * gStaging.mVoiceCount;
			std::memcpy(&gPublished, &gStaging, used);
			gHasFrame = true;
			ReleaseSRWLockExclusive(&gLock);
		}
		gStaging.mVoiceCount = 0;
		gStaging.mUnpositioned = 0;
		gStaging.mDry = 0;
	}

	bool Read(Frame& out)
	{
		AcquireSRWLockShared(&gLock);
		const bool has = gHasFrame;
		if (has) {
			const size_t used = offsetof(Frame, mVoices) + sizeof(Voice) * gPublished.mVoiceCount;
			std::memcpy(&out, &gPublished, used);
		}
		ReleaseSRWLockShared(&gLock);
		return has;
	}
}
