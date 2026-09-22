// Standalone ISpatialAudioClient check, independent of the game (milestone: "does the Windows -> soundbar
// chain light up Atmos at all?").
//
//   spatial_orbit_manual.exe --probe        report object limits / formats, play nothing
//   spatial_orbit_manual.exe [seconds]      bed + one dynamic object circling overhead (default 30 s)
//
// While it plays, the soundbar should switch from "Dolby Audio" to "Dolby Atmos". The object is a clicky
// noise burst every 250 ms so its movement is easy to follow; it circles at ear height for the first half,
// then 45 degrees above you for the second half. A quiet 60 Hz hum goes to the front-center bed channel so
// the static-object path is exercised too.
//
// Build: tools\build.ps1 -Mod SDAtmos -Test (compiled, not run automatically).

#define NOMINMAX
#include <Windows.h>
#include <mmdeviceapi.h>
#include <SpatialAudioClient.h>
#include <Functiondiscoverykeys_devpkey.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "ole32.lib")

#define CHECK(expr)                                                                    \
	do {                                                                               \
		const HRESULT hr_ = (expr);                                                    \
		if (FAILED(hr_)) {                                                             \
			std::printf("FAILED 0x%08lX: %s (line %d)\n", hr_, #expr, __LINE__);       \
			return 1;                                                                  \
		}                                                                              \
	} while (0)

template <typename T>
static void Release(T*& p)
{
	if (p) {
		p->Release();
		p = nullptr;
	}
}

static const char* FormatTag(WORD tag)
{
	switch (tag) {
	case WAVE_FORMAT_PCM: return "PCM";
	case WAVE_FORMAT_IEEE_FLOAT: return "float";
	case WAVE_FORMAT_EXTENSIBLE: return "extensible";
	default: return "?";
	}
}

static const char* StaticName(int bit)
{
	static const char* names[] = { "FrontLeft", "FrontRight", "FrontCenter", "LFE", "SideLeft", "SideRight",
		"BackLeft", "BackRight", "TopFrontLeft", "TopFrontRight", "TopBackLeft", "TopBackRight",
		"BottomFrontLeft", "BottomFrontRight", "BottomBackLeft", "BottomBackRight", "BackCenter",
		"StereoLeft", "StereoRight" };
	// AudioObjectType_FrontLeft is bit 1 (bit 0 is Dynamic).
	return bit >= 1 && bit <= 19 ? names[bit - 1] : "?";
}

int main(int argc, char** argv)
{
	const bool probeOnly = argc > 1 && std::strcmp(argv[1], "--probe") == 0;
	const double seconds = (!probeOnly && argc > 1) ? std::atof(argv[1]) : 30.0;

	CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

	IMMDeviceEnumerator* enumerator = nullptr;
	IMMDevice* device = nullptr;
	CHECK(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
	CHECK(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device));

	IPropertyStore* props = nullptr;
	if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
		PROPVARIANT name;
		PropVariantInit(&name);
		if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR) {
			std::printf("Endpoint: %ls\n", name.pwszVal);
		}
		PropVariantClear(&name);
		Release(props);
	}

	ISpatialAudioClient* client = nullptr;
	const HRESULT activated = device->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void**>(&client));
	if (FAILED(activated)) {
		std::printf("ISpatialAudioClient not available on this endpoint (0x%08lX): no spatial sound format enabled?\n", activated);
		return 1;
	}

	UINT32 maxDynamic = 0;
	CHECK(client->GetMaxDynamicObjectCount(&maxDynamic));
	AudioObjectType staticMask = AudioObjectType_None;
	CHECK(client->GetNativeStaticObjectTypeMask(&staticMask));
	std::printf("Max dynamic objects: %u\n", maxDynamic);
	std::printf("Native static objects (0x%05X):", static_cast<unsigned>(staticMask));
	for (int bit = 1; bit <= 19; ++bit) {
		if (staticMask & (1u << bit)) {
			std::printf(" %s", StaticName(bit));
		}
	}
	std::printf("\n");

	IAudioFormatEnumerator* formats = nullptr;
	CHECK(client->GetSupportedAudioObjectFormatEnumerator(&formats));
	UINT32 formatCount = 0;
	CHECK(formats->GetCount(&formatCount));
	WAVEFORMATEX* objectFormat = nullptr;
	for (UINT32 i = 0; i < formatCount; ++i) {
		WAVEFORMATEX* f = nullptr;
		if (SUCCEEDED(formats->GetFormat(i, &f))) {
			std::printf("Object format %u: %s %u ch %lu Hz %u bit\n", i, FormatTag(f->wFormatTag), f->nChannels, f->nSamplesPerSec, f->wBitsPerSample);
			if (i == 0) {
				objectFormat = f;
			}
		}
	}
	if (!objectFormat) {
		std::printf("No object format\n");
		return 1;
	}
	if (objectFormat->wFormatTag != WAVE_FORMAT_IEEE_FLOAT) {
		std::printf("Expected float objects\n");
		return 1;
	}

	// Ask for a 7.1 bed; the rest of the native mask (heights) stays unused so all dynamic objects remain available.
	const AudioObjectType bedMask = static_cast<AudioObjectType>(AudioObjectType_FrontLeft | AudioObjectType_FrontRight |
		AudioObjectType_FrontCenter | AudioObjectType_LowFrequency | AudioObjectType_SideLeft | AudioObjectType_SideRight |
		AudioObjectType_BackLeft | AudioObjectType_BackRight);

	UINT32 frameMax = 0;
	CHECK(client->GetMaxFrameCount(objectFormat, &frameMax));
	std::printf("Max frame count per update: %u\n", frameMax);
	std::printf("IsAudioObjectFormatSupported: 0x%08lX\n", client->IsAudioObjectFormatSupported(objectFormat));

	const HRESULT available = client->IsSpatialAudioStreamAvailable(__uuidof(ISpatialAudioObjectRenderStream), nullptr);
	std::printf("IsSpatialAudioStreamAvailable: 0x%08lX%s\n", available,
		available == SPTLAUDCLNT_E_RESOURCES_INVALIDATED ? " (resources invalidated)" :
		available == SPTLAUDCLNT_E_NO_MORE_OBJECTS ? " (another app holds the objects)" : "");

	HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	SpatialAudioObjectRenderStreamActivationParams params = {};
	params.ObjectFormat = objectFormat;
	params.StaticObjectTypeMask = bedMask;
	params.MinDynamicObjectCount = 0;
	params.MaxDynamicObjectCount = maxDynamic;
	params.Category = AudioCategory_GameEffects;
	params.EventHandle = event;

	PROPVARIANT blob;
	PropVariantInit(&blob);
	blob.vt = VT_BLOB;
	blob.blob.cbSize = sizeof(params);
	blob.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

	ISpatialAudioObjectRenderStream* stream = nullptr;
	CHECK(client->ActivateSpatialAudioStream(&blob, __uuidof(ISpatialAudioObjectRenderStream), reinterpret_cast<void**>(&stream)));
	std::printf("Stream activated (7.1 bed + up to %u dynamic objects)\n", maxDynamic);

	if (probeOnly) {
		Release(stream);
		std::printf("Probe done.\n");
		return 0;
	}

	ISpatialAudioObject* center = nullptr;
	ISpatialAudioObject* orbiter = nullptr;
	CHECK(stream->ActivateSpatialAudioObject(AudioObjectType_FrontCenter, &center));
	CHECK(stream->Start());

	const double rate = objectFormat->nSamplesPerSec;
	const UINT64 totalFrames = static_cast<UINT64>(seconds * rate);
	UINT64 frame = 0;
	UINT32 seed = 12345;
	int lastReport = -1;
	std::printf("Playing %.0f s. Watch the soundbar's format indicator.\n", seconds);

	while (frame < totalFrames) {
		if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0) {
			std::printf("Timed out waiting for the stream event\n");
			break;
		}

		UINT32 availableObjects = 0, frameCount = 0;
		CHECK(stream->BeginUpdatingAudioObjects(&availableObjects, &frameCount));

		if (!orbiter && availableObjects > 0) {
			const HRESULT hr = stream->ActivateSpatialAudioObject(AudioObjectType_Dynamic, &orbiter);
			if (FAILED(hr)) {
				std::printf("Dynamic object activation failed 0x%08lX\n", hr);
			}
		}

		BYTE* buffer = nullptr;
		UINT32 bytes = 0;
		if (SUCCEEDED(center->GetBuffer(&buffer, &bytes))) {
			float* out = reinterpret_cast<float*>(buffer);
			for (UINT32 i = 0; i < frameCount; ++i) {
				out[i] = 0.05f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 60.0 * (frame + i) / rate));
			}
		}

		if (orbiter && SUCCEEDED(orbiter->GetBuffer(&buffer, &bytes))) {
			float* out = reinterpret_cast<float*>(buffer);
			for (UINT32 i = 0; i < frameCount; ++i) {
				const UINT64 n = frame + i;
				const double inPulse = std::fmod(n / rate, 0.25);
				float sample = 0.0f;
				if (inPulse < 0.06) {
					seed = seed * 1664525u + 1013904223u;
					const float noise = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
					sample = 0.5f * noise * static_cast<float>(1.0 - inPulse / 0.06);
				}
				out[i] = sample;
			}

			// Windows spatial coordinates: +x right, +y up, +z behind the listener; meters.
			const double t = frame / rate;
			const double angle = 2.0 * 3.14159265358979 * t / 6.0;
			const bool high = frame > totalFrames / 2;
			const double elevation = high ? 3.14159265358979 / 4.0 : 0.0;
			const double r = 2.0;
			const float x = static_cast<float>(r * std::cos(elevation) * std::sin(angle));
			const float y = static_cast<float>(r * std::sin(elevation));
			const float z = static_cast<float>(-r * std::cos(elevation) * std::cos(angle));
			orbiter->SetPosition(x, y, z);
			orbiter->SetVolume(1.0f);

			const int second = static_cast<int>(t);
			if (second != lastReport) {
				lastReport = second;
				std::printf("\r%3d s  %s  pos (%5.2f, %5.2f, %5.2f)  ", second, high ? "overhead " : "ear level", x, y, z);
			}
		}

		CHECK(stream->EndUpdatingAudioObjects());
		frame += frameCount;
	}

	std::printf("\nDone.\n");
	if (orbiter) {
		orbiter->SetEndOfStream(0);
	}
	stream->Stop();
	Release(orbiter);
	Release(center);
	Release(stream);
	Release(client);
	Release(formats);
	Release(device);
	Release(enumerator);
	CoUninitialize();
	return 0;
}
