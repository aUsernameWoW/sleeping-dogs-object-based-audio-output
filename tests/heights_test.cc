// The height bed's DSP (core/height_dsp.hh) offline: channel mapping for the layouts the game uses,
// energy-preserving carve, and the decorrelator's behaviour (delay, all-pass flatness, high-pass). No
// engine involved; argv[1] (the .asi path) is unused.

#include "../core/height_dsp.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace height_dsp;

static int gFailures = 0;

#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++gFailures; } } while (0)

static bool Near(float a, float b, float tolerance)
{
	return std::fabs(a - b) <= tolerance;
}

static void TestFloorMap()
{
	FloorMap map;
	// 7.1 in Wwise's mask: FL FR C LFE BL BR SL SR → planar FL FR C BL BR SL SR LFE.
	CHECK(BuildFloorMap(0x63F, map));
	CHECK(map.mChannels == 8);
	CHECK(map.mIndex[FL] == 0 && map.mIndex[FR] == 1 && map.mIndex[C] == 2 && map.mIndex[BL] == 3 && map.mIndex[BR] == 4 && map.mIndex[SL] == 5 && map.mIndex[SR] == 6);
	CHECK(map.mWeight[TFL][FL] == 1.0f && map.mWeight[TFR][FR] == 1.0f);
	CHECK(Near(map.mWeight[TBL][SL], 0.7071f, 1e-3f) && Near(map.mWeight[TBL][BL], 0.7071f, 1e-3f));
	CHECK(Near(map.mWeight[TBR][SR], 0.7071f, 1e-3f) && Near(map.mWeight[TBR][BR], 0.7071f, 1e-3f));
	CHECK(map.mWeight[TFL][C] == 0.0f && map.mWeight[TBL][C] == 0.0f);
	CHECK(Near(map.mSquares[FL], 1.0f, 1e-5f) && Near(map.mSquares[SL], 0.5f, 1e-3f) && map.mSquares[C] == 0.0f);

	// 5.1: surrounds on the back bits only.
	CHECK(BuildFloorMap(0x3F, map));
	CHECK(map.mChannels == 6 && map.mIndex[SL] == -1 && map.mIndex[BL] == 3);
	CHECK(map.mWeight[TBL][BL] == 1.0f && map.mWeight[TBR][BR] == 1.0f);

	// Stereo: the front pair feeds front and back heights.
	CHECK(BuildFloorMap(0x3, map));
	CHECK(map.mChannels == 2);
	CHECK(Near(map.mWeight[TFL][FL], 0.7071f, 1e-3f) && Near(map.mWeight[TBL][FL], 0.7071f, 1e-3f));
	CHECK(Near(map.mSquares[FL], 1.0f, 1e-3f));

	// Mono / center only: nothing to lift.
	CHECK(!BuildFloorMap(0x4, map));

	// The spread map: each side over both of its heights, unit energy per floor channel, no center.
	BuildSpreadMap(map);
	CHECK(Near(map.mWeight[TFL][FL], 0.7071f, 1e-3f) && Near(map.mWeight[TBL][FL], 0.7071f, 1e-3f));
	CHECK(Near(map.mWeight[TFL][SL], 0.7071f, 1e-3f) && Near(map.mWeight[TBL][BL], 0.7071f, 1e-3f));
	CHECK(map.mWeight[TFR][FL] == 0.0f && map.mWeight[TFL][FR] == 0.0f);
	CHECK(map.mWeight[TFL][C] == 0.0f && map.mSquares[C] == 0.0f);
	for (int s : { FL, FR, BL, BR, SL, SR }) {
		CHECK(Near(map.mSquares[s], 1.0f, 1e-3f));
	}
	// Unknown bits (a bed with heights) are refused.
	CHECK(!BuildFloorMap(0x63F | 0x1000, map));
}

static void TestCarve()
{
	FloorMap map;
	BuildFloorMap(0x63F, map);
	constexpr uint32_t frames = 64;
	float floor[8][frames];
	float heights[kChannels][frames] = {};
	float* floorPtr[8];
	float* heightPtr[kChannels] = { heights[0], heights[1], heights[2], heights[3] };
	for (int c = 0; c < 8; ++c) {
		floorPtr[c] = floor[c];
		for (uint32_t i = 0; i < frames; ++i) {
			floor[c][i] = 1.0f + c; // distinct constant per channel
		}
	}
	float gain[frames], keep[frames];
	const float share = 0.5f; // -6 dB
	for (uint32_t i = 0; i < frames; ++i) {
		gain[i] = share * 2.0f; // bus volume 2 × share
		keep[i] = share;
	}
	Carve(map, floorPtr, frames, gain, keep, heightPtr);

	// Heights: TFL = FL × gain, TBL = (BL + SL) × 0.707 × gain.
	CHECK(Near(heights[TFL][0], 1.0f * 1.0f, 1e-4f));
	CHECK(Near(heights[TFR][0], 2.0f * 1.0f, 1e-4f));
	CHECK(Near(heights[TBL][0], (4.0f + 6.0f) * 0.7071f * 1.0f, 1e-3f));
	CHECK(Near(heights[TBR][0], (5.0f + 7.0f) * 0.7071f * 1.0f, 1e-3f));
	// Floor: FL keeps sqrt(1 - 0.25), SL keeps sqrt(1 - 0.125), C and LFE untouched.
	CHECK(Near(floor[0][0], 1.0f * std::sqrt(0.75f), 1e-4f));
	CHECK(Near(floor[5][0], 6.0f * std::sqrt(0.875f), 1e-4f));
	CHECK(floor[2][0] == 3.0f && floor[7][0] == 8.0f);
	// Energy check for FL at unit bus volume: floor² + (height / volume)² == original².
	const float up = heights[TFL][0] / 2.0f;
	CHECK(Near(floor[0][0] * floor[0][0] + up * up, 1.0f, 1e-3f));
}

static void TestDecorrelator()
{
	constexpr uint32_t rate = 48000;
	Decorrelator d;
	const float allPass[Decorrelator::kAllPasses] = { 1.7f, 3.1f, 4.7f };

	// Pre-delay only (no high-pass, all-pass gain 0 still delays by each line): an impulse comes out later.
	d.Setup(rate, 8.0f, allPass, 0.0f, 0.0f);
	constexpr uint32_t n = 4096;
	static float x[n];
	for (uint32_t i = 0; i < n; ++i) x[i] = i == 0 ? 1.0f : 0.0f;
	d.Process(x, n);
	uint32_t first = n;
	for (uint32_t i = 0; i < n; ++i) {
		if (std::fabs(x[i]) > 1e-6f) { first = i; break; }
	}
	const uint32_t expected = 384 + Decorrelator::Samples(rate, 1.7f, 512) + Decorrelator::Samples(rate, 3.1f, 512) + Decorrelator::Samples(rate, 4.7f, 512);
	CHECK(first == expected);

	// All-passes with gain: flat magnitude (energy of an impulse response ≈ 1), spread over time.
	d.Setup(rate, 8.0f, allPass, 0.6f, 0.0f);
	for (uint32_t i = 0; i < n; ++i) x[i] = i == 0 ? 1.0f : 0.0f;
	d.Process(x, n);
	double energy = 0.0;
	uint32_t nonZero = 0;
	for (uint32_t i = 0; i < n; ++i) {
		energy += static_cast<double>(x[i]) * x[i];
		nonZero += std::fabs(x[i]) > 1e-4f;
	}
	CHECK(Near(static_cast<float>(energy), 1.0f, 0.02f));
	CHECK(nonZero > 20);

	// High-pass at 200 Hz: a 50 Hz tone is attenuated by > 20 dB, a 2 kHz tone passes within 1 dB.
	auto toneGain = [&](float hz) {
		Decorrelator f;
		f.Setup(rate, 0.0f, allPass, 0.0f, 200.0f);
		static float y[n];
		for (uint32_t i = 0; i < n; ++i) y[i] = std::sin(2.0f * 3.14159265f * hz * i / rate);
		f.Process(y, n);
		float peak = 0.0f;
		for (uint32_t i = n / 2; i < n; ++i) peak = std::max(peak, std::fabs(y[i]));
		return peak;
	};
	CHECK(toneGain(50.0f) < 0.1f);
	CHECK(toneGain(2000.0f) > 0.89f && toneGain(2000.0f) < 1.05f);

	// The bank: front and back pairs differ, left and right of a pair don't.
	DecorrelatorBank bank;
	bank.Setup(rate, 8.0f, 12.0f, 200.0f);
	static float ch[kChannels][n];
	float* ptr[kChannels] = { ch[0], ch[1], ch[2], ch[3] };
	for (int h = 0; h < kChannels; ++h) {
		for (uint32_t i = 0; i < n; ++i) ch[h][i] = i == 0 ? 1.0f : 0.0f;
	}
	bank.Process(ptr, n);
	bool sameLR = true, sameFB = true;
	for (uint32_t i = 0; i < n; ++i) {
		sameLR &= ch[TFL][i] == ch[TFR][i] && ch[TBL][i] == ch[TBR][i];
		sameFB &= ch[TFL][i] == ch[TBL][i];
	}
	CHECK(sameLR);
	CHECK(!sameFB);
}

int main(int, char**)
{
	TestFloorMap();
	TestCarve();
	TestDecorrelator();
	if (gFailures) {
		std::printf("FAIL: %d checks\n", gFailures);
		return 1;
	}
	std::printf("PASS\n");
	return 0;
}
