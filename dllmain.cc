#include <Windows.h>

#include <string>

#include "core/config.hh"
#include "core/log.hh"
#include "core/overlay.hh"
#include "core/wwise_hooks.hh"

static std::wstring GetModuleDirectory(HMODULE module)
{
	wchar_t path[MAX_PATH] = {};
	const DWORD length = GetModuleFileNameW(module, path, ARRAYSIZE(path));
	if (length == 0 || length >= ARRAYSIZE(path)) {
		return L".";
	}

	std::wstring dir(path, length);
	const size_t slash = dir.find_last_of(L"\\/");
	return slash == std::wstring::npos ? L"." : dir.substr(0, slash);
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);

		// Pin ourselves: the hooks and the render thread point into this module.
		HMODULE pinned;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&DllMain), &pinned);

		const std::wstring dir = GetModuleDirectory(module);
		config::Load(dir);

		if (gConfig.mLogging) {
			logger::Open(dir + L"\\SDAtmos.log");
		}

		LOG("SDAtmos loaded (SpatialBed=%d VoiceLog=%d)", gConfig.mSpatialBed, gConfig.mVoiceLog);

		// Before the game's main runs (the ASI loader loads us from dinput8.dll, a static import), so the sink
		// hook is in place before Wwise initializes.
		wwise::Install();
		overlay::Install(module);
	}

	return TRUE;
}
