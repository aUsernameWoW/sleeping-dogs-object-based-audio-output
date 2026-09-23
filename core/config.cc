#include "config.hh"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string_view>

Config gConfig;

namespace config
{
	static std::wstring gPath;

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
		"; 游戏里按 F9 可以随时开关（A/B 对比）。\n"
		"; Send the loudest point-like 3D sounds as dynamic objects. F9 toggles it in-game (A/B).\n"
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
		"; 沈威自己的声音（脚步、衣物、格斗动作）留在声道床里，按原版的方式混音。\n"
		"; 游戏的听者在摄像机上，沈威在它前方约 3 米；做成对象会变成画面前方一个很准的点。\n"
		"; Keep the player's own sounds (footsteps, foley) in the bed as the original mix does; the listener is\n"
		"; the camera, so as objects they'd be a sharp point ~3 m ahead of the viewer.\n"
		"PlayerInBed = 1\n"
		"\n"
		"; 对象绕过总线上的插入效果。总线上的 Parametric EQ 会由对象自己套用；其他效果（压缩、限幅、延迟……）\n"
		"; 没法复现：1 = 经过这类总线的声音留在床里（主总线除外），2 = 主总线也算，0 = 不管。日志会列出带效果的总线。\n"
		"; Objects skip the insert effects on their bus chain. Parametric EQs are applied to the objects themselves;\n"
		"; for anything else (compressor, limiter, delay...): 1 = such voices stay in the bed (master bus excepted),\n"
		"; 2 = master bus included, 0 = ignore. The log lists the buses with effects.\n"
		"BusFx = 1\n"
		"\n"
		"; 游戏把角色的发声位置放在脚底；这里把角色（NPC 语音等）抬高多少米到头部附近，0 = 不抬。\n"
		"; 只影响方向和一点点距离衰减，游戏自己的遮挡/距离计算不受影响。\n"
		"; The game positions characters' sounds at their feet; lift them this many meters (0 = off).\n"
		"; Only the direction and a little distance attenuation change.\n"
		"ActorLift = 1.5\n"
		"\n"
		"[Heights]\n"
		"; 7.1.4 声道床的四个顶部声道。游戏本身只有 7.1，这里把扩散类声音的一部分抬到头顶：天气（雨、雷、风）、鸟叫、\n"
		"; 城市/人群等环境声，以及混响返回。抬上去的部分会做去相关处理（短延迟 + 全通 + 高通），地面声道相应减少同样的\n"
		"; 能量。这是 Dolby 对游戏 bed 的用法建议，也是 DTS/Dolby 上混器的做法。游戏里按 F7 开关（A/B 对比）。\n"
		"; The four top channels of a 7.1.4 bed. The game only mixes 7.1; a share of the diffuse content goes up:\n"
		"; weather (rain, thunder, wind), birds, city/crowd ambience and reverb returns, decorrelated (short\n"
		"; delay + all-passes + high-pass); the floor loses the same energy. F7 toggles it in-game (A/B).\n"
		"Enabled = 1\n"
		"\n"
		"; 各类来源抬到顶部的比例（dB，相对该总线的输出；-60 以下 = 不抬）。\n"
		"; Share of each kind moved up, in dB of the bus's output (-60 or below = none).\n"
		"Sky = -3\n"
		"Ambience = -6\n"
		"Reverb = -6\n"
		"\n"
		"; 去相关参数：前方顶部声道的预延迟（毫秒，后方再加 4 ms），高通截止频率（Hz，0 = 关）。改动需重启游戏。\n"
		"; Decorrelation: pre-delay of the front heights in ms (the back pair gets 4 ms more), high-pass in Hz\n"
		"; (0 = off). Changes need a game restart.\n"
		"Delay = 8\n"
		"HighPass = 200\n"
		"\n"
		"; 哪些 Wwise 总线算“天空”和“环境声”（Init.bnk 里的总线 ID，逗号分隔，最多 8 个）：声音的输出总线或其任一\n"
		"; 上级总线在列表里就算，最近的一级说了算。混响总线按其效果器自动识别。\n"
		"; 默认：weather 317282339、birds 352130103；ambient 77978275（整个环境声子树）。\n"
		"; Wwise bus IDs (from Init.bnk, comma-separated, up to 8) treated as sky / ambience: a sound whose output\n"
		"; bus or any bus above it is listed counts, the nearest one decides. Reverb buses are recognized by\n"
		"; their effects.\n"
		"SkyBuses = 317282339, 352130103\n"
		"AmbienceBuses = 77978275\n"
		"\n"
		"[Overlay]\n"
		"; 需要 ReShade（支持插件的版本）。设置也可以在 ReShade 菜单的 SDAtmos 标签页里改。\n"
		"; Needs ReShade with add-on support; everything here is also in the SDAtmos tab of the ReShade menu.\n"
		"; 游戏里按 F8 开关 HUD（雷达 + 画面上的声音标记）。\n"
		"; F8 toggles the HUD (radar + on-screen sound markers).\n"
		"Hud = 0\n"
		"Radar = 1\n"
		"Markers = 1\n"
		"Labels = 0\n"
		"BedVoices = 1\n"
		"; 画面标记用的垂直视野角（度），标记和画面对不上时调它。\n"
		"; Vertical camera FOV (degrees) for on-screen markers; adjust until markers sit on their sources.\n"
		"Fov = 60\n"
		"; 雷达边缘代表多少米。 / Meters at the radar's edge.\n"
		"RadarRange = 60\n"
		"; 热键（虚拟键码，十六进制）。0x78 = F9, 0x77 = F8, 0x76 = F7。\n"
		"; Hotkeys (virtual-key codes).\n"
		"ToggleObjectsKey = 0x78\n"
		"ToggleHudKey = 0x77\n"
		"ToggleHeightsKey = 0x76\n"
		"\n"
		"[Debug]\n"
		"; 在 .asi 旁边写 SDAtmos.log。\n"
		"; Write SDAtmos.log.\n"
		"Logging = 1\n"
		"\n"
		"; 每隔几秒把正在播放的 3D 声音（位置、音量、走床还是对象）写进日志。\n"
		"; Periodically log the 3D voices Wwise is mixing.\n"
		"VoiceLog = 1\n"
		"\n"
		"; 调试热键：强制游戏下雨/放晴（测试高度声道的天气档用），0 = 关。0x75 = F6。\n"
		"; Debug hotkey: force the game's weather to rain / clear (to test the height bed's sky tier); 0 = off.\n"
		"ToggleRainKey = 0x75\n";

	static std::wstring ReadString(const wchar_t* section, const wchar_t* key)
	{
		wchar_t text[64] = {};
		GetPrivateProfileStringW(section, key, L"", text, ARRAYSIZE(text), gPath.c_str());
		return text;
	}

	static bool ReadBool(const wchar_t* section, const wchar_t* key, bool fallback)
	{
		return GetPrivateProfileIntW(section, key, fallback ? 1 : 0, gPath.c_str()) != 0;
	}

	static float ReadFloat(const wchar_t* section, const wchar_t* key, float fallback)
	{
		const std::wstring text = ReadString(section, key);
		wchar_t* end = nullptr;
		const float value = std::wcstof(text.c_str(), &end);
		return end != text.c_str() ? value : fallback;
	}

	static int ReadInt(const wchar_t* section, const wchar_t* key, int fallback)
	{
		// Accepts decimal and 0x-prefixed hex (GetPrivateProfileInt only does decimal).
		const std::wstring text = ReadString(section, key);
		wchar_t* end = nullptr;
		const long value = std::wcstol(text.c_str(), &end, 0);
		return end != text.c_str() ? static_cast<int>(value) : fallback;
	}

	// "1, 2,3" → up to `max` unsigned IDs; anything unparsable ends the list. Returns the count.
	static int ReadIdList(const wchar_t* section, const wchar_t* key, uint32_t* out, int max, int fallbackCount)
	{
		// An absent key keeps the defaults; an empty value means an empty list.
		wchar_t text[256] = {};
		GetPrivateProfileStringW(section, key, L"\x01", text, ARRAYSIZE(text), gPath.c_str());
		if (text[0] == L'\x01') {
			return fallbackCount;
		}
		const wchar_t* p = text;
		int count = 0;
		while (count < max) {
			while (*p == L' ' || *p == L',' || *p == L'\t') {
				++p;
			}
			wchar_t* end = nullptr;
			const unsigned long value = std::wcstoul(p, &end, 0);
			if (end == p) {
				break;
			}
			out[count++] = static_cast<uint32_t>(value);
			p = end;
		}
		return count;
	}

	void Load(const std::wstring& dir)
	{
		gPath = dir + L"\\SDAtmos.ini";

		if (GetFileAttributesW(gPath.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			FILE* file = nullptr;
			if (_wfopen_s(&file, gPath.c_str(), L"wb") == 0 && file)
			{
				fwrite(kDefaultIni, 1, sizeof(kDefaultIni) - 1, file);
				fclose(file);
			}
		}

		gConfig.mSpatialBed = ReadBool(L"General", L"SpatialBed", gConfig.mSpatialBed);
		gConfig.mObjects = ReadBool(L"Objects", L"Enabled", gConfig.mObjects);
		gConfig.mMaxObjects = ReadInt(L"Objects", L"MaxObjects", gConfig.mMaxObjects);
		gConfig.mObjectDistance = ReadFloat(L"Objects", L"Distance", gConfig.mObjectDistance);
		gConfig.mPlayerInBed = ReadBool(L"Objects", L"PlayerInBed", gConfig.mPlayerInBed);
		gConfig.mBusFx = ReadInt(L"Objects", L"BusFx", gConfig.mBusFx);
		gConfig.mActorLift = ReadFloat(L"Objects", L"ActorLift", gConfig.mActorLift);
		gConfig.mHeights = ReadBool(L"Heights", L"Enabled", gConfig.mHeights);
		gConfig.mHeightSky = ReadFloat(L"Heights", L"Sky", gConfig.mHeightSky);
		gConfig.mHeightAmbience = ReadFloat(L"Heights", L"Ambience", gConfig.mHeightAmbience);
		gConfig.mHeightReverb = ReadFloat(L"Heights", L"Reverb", gConfig.mHeightReverb);
		gConfig.mHeightDelay = ReadFloat(L"Heights", L"Delay", gConfig.mHeightDelay);
		gConfig.mHeightHighPass = ReadFloat(L"Heights", L"HighPass", gConfig.mHeightHighPass);
		gConfig.mSkyBusCount = ReadIdList(L"Heights", L"SkyBuses", gConfig.mSkyBuses, Config::kMaxBusIds, gConfig.mSkyBusCount);
		gConfig.mAmbienceBusCount = ReadIdList(L"Heights", L"AmbienceBuses", gConfig.mAmbienceBuses, Config::kMaxBusIds, gConfig.mAmbienceBusCount);
		gConfig.mHud =ReadBool(L"Overlay", L"Hud", gConfig.mHud);
		gConfig.mHudRadar = ReadBool(L"Overlay", L"Radar", gConfig.mHudRadar);
		gConfig.mHudMarkers = ReadBool(L"Overlay", L"Markers", gConfig.mHudMarkers);
		gConfig.mHudLabels = ReadBool(L"Overlay", L"Labels", gConfig.mHudLabels);
		gConfig.mHudBedVoices = ReadBool(L"Overlay", L"BedVoices", gConfig.mHudBedVoices);
		gConfig.mHudFov = ReadFloat(L"Overlay", L"Fov", gConfig.mHudFov);
		gConfig.mHudRadarRange = ReadFloat(L"Overlay", L"RadarRange", gConfig.mHudRadarRange);
		gConfig.mToggleObjectsKey = ReadInt(L"Overlay", L"ToggleObjectsKey", gConfig.mToggleObjectsKey);
		gConfig.mToggleHudKey = ReadInt(L"Overlay", L"ToggleHudKey", gConfig.mToggleHudKey);
		gConfig.mToggleHeightsKey = ReadInt(L"Overlay", L"ToggleHeightsKey", gConfig.mToggleHeightsKey);
		gConfig.mLogging = ReadBool(L"Debug", L"Logging", gConfig.mLogging);
		gConfig.mVoiceLog = ReadBool(L"Debug", L"VoiceLog", gConfig.mVoiceLog);
		gConfig.mToggleRainKey = ReadInt(L"Debug", L"ToggleRainKey", gConfig.mToggleRainKey);

		if (gConfig.mMaxObjects < 0) {
			gConfig.mMaxObjects = 0;
		}
		if (gConfig.mBusFx < 0 || gConfig.mBusFx > 2) {
			gConfig.mBusFx = 1;
		}
		if (!(gConfig.mActorLift >= 0.0f && gConfig.mActorLift <= 3.0f)) {
			gConfig.mActorLift = 1.5f;
		}
		if (!(gConfig.mObjectDistance > 0.1f && gConfig.mObjectDistance < 100.0f)) {
			gConfig.mObjectDistance = 2.0f;
		}
		for (std::atomic<float>* share : { &gConfig.mHeightSky, &gConfig.mHeightAmbience, &gConfig.mHeightReverb }) {
			// A share above 0 dB would leave the floor with nothing (sqrt of a negative energy).
			if (!(share->load() <= 0.0f)) {
				*share = 0.0f;
			}
		}
		if (!(gConfig.mHeightDelay >= 0.0f && gConfig.mHeightDelay <= 30.0f)) {
			gConfig.mHeightDelay = 8.0f;
		}
		if (!(gConfig.mHeightHighPass >= 0.0f && gConfig.mHeightHighPass <= 2000.0f)) {
			gConfig.mHeightHighPass = 200.0f;
		}
		if (!(gConfig.mHudFov >= 20.0f && gConfig.mHudFov <= 120.0f)) {
			gConfig.mHudFov = 60.0f;
		}
		if (!(gConfig.mHudRadarRange >= 5.0f && gConfig.mHudRadarRange <= 500.0f)) {
			gConfig.mHudRadarRange = 60.0f;
		}
	}

	// Sets `key` in `[section]` of the ini text: replaces the value on the key's line (keeping its "key = "
	// prefix), or adds the line at the end of the section. Byte-level on purpose: WritePrivateProfileString
	// treats a BOM-less file as ANSI and would re-encode the UTF-8 comments.
	static void SetValue(std::string& text, const char* section, const char* key, const std::string& value)
	{
		const std::string header = std::string("[") + section + "]";
		size_t sectionStart = std::string::npos;
		size_t pos = 0;
		while (pos < text.size()) {
			size_t end = text.find('\n', pos);
			if (end == std::string::npos) {
				end = text.size();
			}
			std::string_view line(text.data() + pos, end - pos);
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
				line.remove_suffix(1);
			}
			if (!line.empty() && line.front() == '[') {
				if (sectionStart != std::string::npos) {
					// Next section: key missing, insert before this header.
					text.insert(pos, std::string(key) + " = " + value + "\n");
					return;
				}
				if (line == header) {
					sectionStart = end;
				}
			}
			else if (sectionStart != std::string::npos && line.rfind(key, 0) == 0) {
				const size_t equals = line.find('=');
				const std::string_view name = line.substr(0, equals == std::string_view::npos ? line.size() : equals);
				if (equals != std::string_view::npos && name.find_first_not_of(' ', std::strlen(key)) == std::string_view::npos) {
					size_t valueStart = pos + equals + 1;
					while (valueStart < pos + line.size() && text[valueStart] == ' ') {
						++valueStart;
					}
					text.replace(valueStart, pos + line.size() - valueStart, value);
					return;
				}
			}
			pos = end + 1;
		}
		if (sectionStart == std::string::npos) {
			text += "\n" + header + "\n";
		}
		else if (!text.empty() && text.back() != '\n') {
			text += "\n";
		}
		text += std::string(key) + " = " + value + "\n";
	}

	static std::string FormatFloat(float value)
	{
		char text[32];
		snprintf(text, sizeof(text), "%g", value);
		return text;
	}

	void Save()
	{
		if (gPath.empty()) {
			return;
		}
		std::string text;
		if (FILE* file = nullptr; _wfopen_s(&file, gPath.c_str(), L"rb") == 0 && file) {
			char buffer[4096];
			size_t read;
			while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
				text.append(buffer, read);
			}
			fclose(file);
		}

		SetValue(text, "Objects", "Enabled", gConfig.mObjects ? "1" : "0");
		SetValue(text, "Objects", "MaxObjects", std::to_string(gConfig.mMaxObjects.load()));
		SetValue(text, "Objects", "Distance", FormatFloat(gConfig.mObjectDistance));
		SetValue(text, "Objects", "PlayerInBed", gConfig.mPlayerInBed ? "1" : "0");
		SetValue(text, "Objects", "BusFx", std::to_string(gConfig.mBusFx.load()));
		SetValue(text, "Objects", "ActorLift", FormatFloat(gConfig.mActorLift));
		SetValue(text, "Heights", "Enabled", gConfig.mHeights ? "1" : "0");
		SetValue(text, "Heights", "Sky", FormatFloat(gConfig.mHeightSky));
		SetValue(text, "Heights", "Ambience", FormatFloat(gConfig.mHeightAmbience));
		SetValue(text, "Heights", "Reverb", FormatFloat(gConfig.mHeightReverb));
		SetValue(text, "Overlay", "Hud", gConfig.mHud ? "1" : "0");
		SetValue(text, "Overlay", "Radar", gConfig.mHudRadar ? "1" : "0");
		SetValue(text, "Overlay", "Markers", gConfig.mHudMarkers ? "1" : "0");
		SetValue(text, "Overlay", "Labels", gConfig.mHudLabels ? "1" : "0");
		SetValue(text, "Overlay", "BedVoices", gConfig.mHudBedVoices ? "1" : "0");
		SetValue(text, "Overlay", "Fov", FormatFloat(gConfig.mHudFov));
		SetValue(text, "Overlay", "RadarRange", FormatFloat(gConfig.mHudRadarRange));

		if (FILE* file = nullptr; _wfopen_s(&file, gPath.c_str(), L"wb") == 0 && file) {
			fwrite(text.data(), 1, text.size(), file);
			fclose(file);
		}
	}
}
