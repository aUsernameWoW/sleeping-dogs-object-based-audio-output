// config::Save must update values in place without touching anything else in the user's ini: the UTF-8
// comments (WritePrivateProfileString would re-encode them as ANSI), other keys, keys that merely share a
// prefix (Radar / RadarRange), and it must add keys missing from an ini written by an older version.
// Compiles config.cc directly; argv[1] (the .asi path) is only used to find a scratch directory.

#include "../core/config.cc"

#include <cstdio>
#include <fstream>
#include <sstream>

static std::string ReadAll(const std::wstring& path)
{
	std::ifstream file(path, std::ios::binary);
	std::stringstream text;
	text << file.rdbuf();
	return text.str();
}

static int Fail(const char* what, const std::string& text)
{
	std::printf("FAIL: %s\n----\n%s\n----\n", what, text.c_str());
	return 1;
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		return 2;
	}
	std::string dirA = argv[1];
	dirA = dirA.substr(0, dirA.find_last_of("\\/"));
	const std::wstring dir(dirA.begin(), dirA.end());
	const std::wstring path = dir + L"\\SDAtmos.ini";

	// An ini from the previous version: no [Overlay] section, no Objects keys, CRLF line endings, a comment
	// with Chinese, and a key whose name is a prefix of another.
	const std::string old =
		"; SDAtmos 配置 / configuration\r\n"
		"[General]\r\n"
		"SpatialBed = 1\r\n"
		"\r\n"
		"[Objects]\r\n"
		"; 动态对象数量上限\r\n"
		"MaxObjects = 20\r\n"
		"\r\n"
		"[Debug]\r\n"
		"Logging = 1\r\n";
	{
		std::ofstream file(path, std::ios::binary);
		file << old;
	}

	config::Load(dir);
	if (gConfig.mMaxObjects != 20 || !gConfig.mObjects) {
		return Fail("Load", ReadAll(path));
	}

	gConfig.mMaxObjects = 12;
	gConfig.mObjects = false;
	gConfig.mHud = true;
	gConfig.mHudRadar = false;
	gConfig.mHudRadarRange = 80.0f;
	config::Save();
	const std::string saved = ReadAll(path);

	const char* expected[] = {
		"; SDAtmos 配置 / configuration\r\n",
		"; 动态对象数量上限\r\n",
		"MaxObjects = 12\r\n",
		"Enabled = 0\n",
		"[Debug]\r\nLogging = 1\r\n",
		"[Overlay]\n",
		"Radar = 0\n",
		"RadarRange = 80\n",
		"Hud = 1\n",
	};
	for (const char* piece : expected) {
		if (saved.find(piece) == std::string::npos) {
			std::printf("missing: %s\n", piece);
			return Fail("Save", saved);
		}
	}
	// Objects keys must land in [Objects], before [Debug].
	if (saved.find("Enabled = 0") > saved.find("[Debug]")) {
		return Fail("Enabled not inside [Objects]", saved);
	}

	// Round trip: loading the saved file gives the saved values.
	gConfig.mMaxObjects = 1;
	gConfig.mHudRadar = true;
	config::Load(dir);
	if (gConfig.mMaxObjects != 12 || gConfig.mHudRadar || gConfig.mObjects || !gConfig.mHud) {
		return Fail("reload", saved);
	}

	// Saving again changes nothing.
	config::Save();
	if (ReadAll(path) != saved) {
		return Fail("second save differs", ReadAll(path));
	}

	std::printf("PASS\n");
	return 0;
}
