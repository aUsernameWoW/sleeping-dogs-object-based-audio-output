#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

// The DSP behind the height bed, free of engine dependencies so tests can include it directly.
//
// Height channels are derived from a bus's floor channels (see FloorMap), then decorrelated per height
// channel (Decorrelator). The decorrelation is what the literature agrees on for putting floor-derived
// material overhead without pulling the image up to a phantom between floor and ceiling: a short Haas
// pre-delay (5-20 ms, so transients stay localized on the floor), an all-pass chain (random-phase
// decorrelation without the comb of a bare delay) and a high-pass (low frequencies can't be localized
// overhead anyway; the DTS height upmixer shelves them, Lee's elevation experiments put the useful cues
// above ~250 Hz). Front and back pairs get different delays/all-pass sets so they never carry identical
// content (a common Atmos mixing mistake: identical front/back heights collapse and cancel in fold-downs).
namespace height_dsp
{
	constexpr int kChannels = 4; // TFL TFR TBL TBR
	enum : int { TFL, TFR, TBL, TBR };

	// Wwise's floor speakers in pipeline order (LFE is last in the buffer and never used here).
	enum : int { FL, FR, C, BL, BR, SL, SR, kFloor };
	constexpr uint32_t kFloorBits[kFloor] = { 0x1, 0x2, 0x4, 0x10, 0x20, 0x200, 0x400 };
	constexpr uint32_t kLfeBit = 0x8;

	// How a bus's floor channels feed the height channels. Front heights take the front pair; back heights
	// take the side and back pairs (each at -3 dB when both exist); a bus without surrounds spreads its
	// front pair over front and back heights. The center never goes up (dialogue/2D center content isn't
	// diffuse), and the LFE neither.
	struct FloorMap
	{
		int mIndex[kFloor];               // planar channel index, -1 if the mask lacks the speaker
		int mChannels;                    // channels in the buffer (incl. LFE)
		float mWeight[kChannels][kFloor]; // height <- floor gains at unit height gain
		float mSquares[kFloor];           // sum over heights of mWeight², for the energy-preserving floor scale
		bool mAny;                        // at least one weight is non-zero
	};

	inline bool BuildFloorMap(uint32_t mask, FloorMap& map)
	{
		std::memset(&map, 0, sizeof(map));
		int index = 0;
		for (int s = 0; s < kFloor; ++s) {
			map.mIndex[s] = (mask & kFloorBits[s]) ? index++ : -1;
		}
		if (mask & kLfeBit) {
			++index;
		}
		map.mChannels = index;
		if (mask & ~(kLfeBit | 0x1 | 0x2 | 0x4 | 0x10 | 0x20 | 0x200 | 0x400)) {
			return false; // a layout we don't know (heights of a 7.1.4 bus, ...)
		}
		const bool sides = map.mIndex[SL] >= 0 && map.mIndex[SR] >= 0;
		const bool backs = map.mIndex[BL] >= 0 && map.mIndex[BR] >= 0;
		const bool fronts = map.mIndex[FL] >= 0 && map.mIndex[FR] >= 0;
		if (!fronts) {
			return false;
		}
		if (sides || backs) {
			map.mWeight[TFL][FL] = map.mWeight[TFR][FR] = 1.0f;
			const float w = sides && backs ? 0.70710678f : 1.0f;
			if (sides) {
				map.mWeight[TBL][SL] = map.mWeight[TBR][SR] = w;
			}
			if (backs) {
				map.mWeight[TBL][BL] = map.mWeight[TBR][BR] = w;
			}
		}
		else {
			map.mWeight[TFL][FL] = map.mWeight[TBL][FL] = 0.70710678f;
			map.mWeight[TFR][FR] = map.mWeight[TBR][FR] = 0.70710678f;
		}
		for (int s = 0; s < kFloor; ++s) {
			for (int h = 0; h < kChannels; ++h) {
				map.mSquares[s] += map.mWeight[h][s] * map.mWeight[h][s];
			}
			map.mAny |= map.mSquares[s] > 0.0f;
		}
		return map.mAny;
	}

	// The 7.1 map for content that belongs everywhere overhead (2D weather loops: rain, wind): left floor
	// channels feed both left heights, right ones both right heights, so the ceiling is as wide front-to-back
	// as the floor loop was left-to-right. Left/right stays, front/back evens out.
	inline void BuildSpreadMap(FloorMap& map)
	{
		BuildFloorMap(0x63F, map);
		std::memset(map.mWeight, 0, sizeof(map.mWeight));
		std::memset(map.mSquares, 0, sizeof(map.mSquares));
		for (int s : { FL, BL, SL }) {
			map.mWeight[TFL][s] = map.mWeight[TBL][s] = 0.70710678f;
		}
		for (int s : { FR, BR, SR }) {
			map.mWeight[TFR][s] = map.mWeight[TBR][s] = 0.70710678f;
		}
		for (int s = 0; s < kFloor; ++s) {
			for (int h = 0; h < kChannels; ++h) {
				map.mSquares[s] += map.mWeight[h][s] * map.mWeight[h][s];
			}
		}
		map.mAny = true;
	}

	// Moves a share of a bus's floor output into height accumulators, energy-preserving: a floor channel
	// that sends g² of its power up keeps sqrt(1 - g²). `gain[i]` is the per-sample height gain (bus volume
	// ramp × downstream × share × enable ramp); `keep[i]` is the same ramp used for the floor scale, i.e. the
	// share × enable ramp only (the bus's own ramp is applied by Wwise afterwards). Planar in/out.
	inline void Carve(const FloorMap& map, float* const* floor, uint32_t frames, const float* gain, const float* keep,
		float* const* heights)
	{
		for (int h = 0; h < kChannels; ++h) {
			float* out = heights[h];
			for (int s = 0; s < kFloor; ++s) {
				const float w = map.mWeight[h][s];
				if (w == 0.0f) {
					continue;
				}
				const float* in = floor[map.mIndex[s]];
				for (uint32_t i = 0; i < frames; ++i) {
					out[i] += in[i] * gain[i] * w;
				}
			}
		}
		for (int s = 0; s < kFloor; ++s) {
			if (map.mSquares[s] == 0.0f) {
				continue;
			}
			float* io = floor[map.mIndex[s]];
			const float sq = map.mSquares[s];
			for (uint32_t i = 0; i < frames; ++i) {
				const float k = keep[i] * keep[i] * sq;
				io[i] *= k < 1.0f ? std::sqrt(1.0f - k) : 0.0f;
			}
		}
	}

	// Pre-delay → 3 Schroeder all-passes → 2nd-order Butterworth high-pass. Per height channel.
	struct Decorrelator
	{
		static constexpr uint32_t kMaxDelay = 2048; // 42 ms at 48 kHz
		static constexpr uint32_t kMaxAllPass = 512; // 10 ms
		static constexpr int kAllPasses = 3;

		float mDelay[kMaxDelay];
		uint32_t mDelayLen = 0;
		uint32_t mDelayPos = 0;
		struct AllPass
		{
			float mLine[kMaxAllPass];
			uint32_t mLen;
			uint32_t mPos;
			float mGain;
		} mAllPass[kAllPasses];
		float mHp[5] = {};     // b0 b1 b2 a1 a2 (normalized)
		float mHpState[4] = {}; // x1 x2 y1 y2
		bool mBypassHp = false;

		static uint32_t Samples(uint32_t sampleRate, float ms, uint32_t max)
		{
			const uint32_t n = static_cast<uint32_t>(ms * sampleRate / 1000.0f + 0.5f);
			return n < 1 ? 1 : n > max ? max : n;
		}

		void Setup(uint32_t sampleRate, float delayMs, const float allPassMs[kAllPasses], float allPassGain, float highPassHz)
		{
			std::memset(mDelay, 0, sizeof(mDelay));
			mDelayLen = Samples(sampleRate, delayMs, kMaxDelay);
			mDelayPos = 0;
			for (int a = 0; a < kAllPasses; ++a) {
				std::memset(mAllPass[a].mLine, 0, sizeof(mAllPass[a].mLine));
				mAllPass[a].mLen = Samples(sampleRate, allPassMs[a], kMaxAllPass);
				mAllPass[a].mPos = 0;
				mAllPass[a].mGain = allPassGain;
			}
			std::memset(mHpState, 0, sizeof(mHpState));
			mBypassHp = highPassHz <= 0.0f;
			if (!mBypassHp) {
				// RBJ cookbook high-pass, Q = 1/sqrt(2).
				const float w0 = 2.0f * 3.14159265f * highPassHz / static_cast<float>(sampleRate);
				const float cosw = std::cos(w0);
				const float alpha = std::sin(w0) / (2.0f * 0.70710678f);
				const float a0 = 1.0f + alpha;
				mHp[0] = (1.0f + cosw) * 0.5f / a0;
				mHp[1] = -(1.0f + cosw) / a0;
				mHp[2] = mHp[0];
				mHp[3] = -2.0f * cosw / a0;
				mHp[4] = (1.0f - alpha) / a0;
			}
		}

		void Process(float* samples, uint32_t frames)
		{
			for (uint32_t i = 0; i < frames; ++i) {
				float x = samples[i];
				// Pre-delay.
				if (mDelayLen > 0) {
					const float delayed = mDelay[mDelayPos];
					mDelay[mDelayPos] = x;
					mDelayPos = (mDelayPos + 1) % mDelayLen;
					x = delayed;
				}
				// All-passes: v[n] = x[n] + g·v[n-D]; y[n] = v[n-D] - g·v[n].
				for (int a = 0; a < kAllPasses; ++a) {
					AllPass& ap = mAllPass[a];
					const float vd = ap.mLine[ap.mPos];
					const float v = x + ap.mGain * vd;
					ap.mLine[ap.mPos] = v;
					ap.mPos = (ap.mPos + 1) % ap.mLen;
					x = vd - ap.mGain * v;
				}
				// High-pass.
				if (!mBypassHp) {
					const float y = mHp[0] * x + mHp[1] * mHpState[0] + mHp[2] * mHpState[1] - mHp[3] * mHpState[2] - mHp[4] * mHpState[3];
					mHpState[1] = mHpState[0];
					mHpState[0] = x;
					mHpState[3] = mHpState[2];
					mHpState[2] = y;
					x = y;
				}
				samples[i] = x;
			}
		}
	};

	// The four decorrelators with per-pair settings: the same delay/all-pass set for left and right of a
	// pair (a source centered on the floor stays centered overhead), different sets front vs back.
	struct DecorrelatorBank
	{
		Decorrelator mChannel[kChannels];

		void Setup(uint32_t sampleRate, float frontDelayMs, float backDelayMs, float highPassHz)
		{
			static constexpr float kFrontAllPass[Decorrelator::kAllPasses] = { 1.7f, 3.1f, 4.7f };
			static constexpr float kBackAllPass[Decorrelator::kAllPasses] = { 2.3f, 3.7f, 5.3f };
			constexpr float kGain = 0.6f;
			mChannel[TFL].Setup(sampleRate, frontDelayMs, kFrontAllPass, kGain, highPassHz);
			mChannel[TFR].Setup(sampleRate, frontDelayMs, kFrontAllPass, kGain, highPassHz);
			mChannel[TBL].Setup(sampleRate, backDelayMs, kBackAllPass, kGain, highPassHz);
			mChannel[TBR].Setup(sampleRate, backDelayMs, kBackAllPass, kGain, highPassHz);
		}

		void Process(float* const* heights, uint32_t frames)
		{
			for (int h = 0; h < kChannels; ++h) {
				mChannel[h].Process(heights[h], frames);
			}
		}
	};

	inline float DbToGain(float db)
	{
		return db <= -60.0f ? 0.0f : std::pow(10.0f, db * 0.05f);
	}
}
