#include "config.hh"

#include <Windows.h>

#include <cstdio>
#include <cwchar>

Config gConfig;

namespace config
{
	static constexpr char kDefaultIni[] =
		"; SDAtmos 配置 / configuration\n"
		"; 1 = 开启 (on), 0 = 关闭 (off)\n"
		"; 适用于 Windows 里任意空间音效格式（Dolby Atmos for home theater、DTS:X、Windows Sonic）。\n"
		"; Works with whichever spatial sound format Windows has active (Dolby Atmos, DTS:X, Windows Sonic).\n"
		"\n"
		"[General]\n"
		"; 把游戏的 7.1 混音改走 Windows 空间音频输出。空间音效没开时自动退回原来的输出方式。\n"
		"; Route the game's 7.1 mix through Windows spatial audio (falls back to XAudio2 if it's off).\n"
		"SpatialBed = 1\n"
		"\n"
		"[Objects]\n"
		"; 把最响的点状 3D 声音（枪声、脚步、车辆……）从声道床里拿出来，作为动态对象输出（可以有高度）。\n"
		"; Send the loudest point-like 3D sounds as dynamic objects instead of mixing them into the bed.\n"
		"Enabled = 1\n"
		"\n"
		"; 动态对象数量上限；实际还受空间音效格式限制（HDMI 上的 Atmos 为 20）。\n"
		"; Max dynamic objects (the spatial format may allow fewer; Atmos over HDMI: 20).\n"
		"MaxObjects = 20\n"
		"\n"
		"; 对象放在听者周围多远（米）。距离衰减 Wwise 已经算过了，这里只决定方向感的呈现。\n"
		"; Radius (meters) objects are placed at; Wwise already applied distance attenuation.\n"
		"Distance = 2.0\n"
		"\n"
		"[Debug]\n"
		"; 在 .asi 旁边写 SDAtmos.log。\n"
		"; Write SDAtmos.log.\n"
		"Logging = 1\n"
		"\n"
		"; 每隔几秒把正在播放的 3D 声音（位置、音量、走床还是对象）写进日志。\n"
		"; Periodically log the 3D voices Wwise is mixing.\n"
		"VoiceLog = 1\n";

	static bool ReadBool(const wchar_t* path, const wchar_t* section, const wchar_t* key, bool fallback)
	{
		return GetPrivateProfileIntW(section, key, fallback ? 1 : 0, path) != 0;
	}

	static float ReadFloat(const wchar_t* path, const wchar_t* section, const wchar_t* key, float fallback)
	{
		wchar_t text[64] = {};
		GetPrivateProfileStringW(section, key, L"", text, ARRAYSIZE(text), path);
		wchar_t* end = nullptr;
		const float value = std::wcstof(text, &end);
		return end != text ? value : fallback;
	}

	void Load(const std::wstring& dir)
	{
		const std::wstring path = dir + L"\\SDAtmos.ini";

		if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			FILE* file = nullptr;
			if (_wfopen_s(&file, path.c_str(), L"wb") == 0 && file)
			{
				fwrite(kDefaultIni, 1, sizeof(kDefaultIni) - 1, file);
				fclose(file);
			}
		}

		gConfig.mSpatialBed = ReadBool(path.c_str(), L"General", L"SpatialBed", gConfig.mSpatialBed);
		gConfig.mObjects = ReadBool(path.c_str(), L"Objects", L"Enabled", gConfig.mObjects);
		gConfig.mMaxObjects = static_cast<int>(GetPrivateProfileIntW(L"Objects", L"MaxObjects", gConfig.mMaxObjects, path.c_str()));
		gConfig.mObjectDistance = ReadFloat(path.c_str(), L"Objects", L"Distance", gConfig.mObjectDistance);
		gConfig.mLogging = ReadBool(path.c_str(), L"Debug", L"Logging", gConfig.mLogging);
		gConfig.mVoiceLog = ReadBool(path.c_str(), L"Debug", L"VoiceLog", gConfig.mVoiceLog);

		if (gConfig.mMaxObjects < 0) {
			gConfig.mMaxObjects = 0;
		}
		if (!(gConfig.mObjectDistance > 0.1f && gConfig.mObjectDistance < 100.0f)) {
			gConfig.mObjectDistance = 2.0f;
		}
	}
}
