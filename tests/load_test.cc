// Loads SDAtmos.asi into a process that has no Wwise: it must not crash, must report the missing sink
// functions in its log and must leave audio alone. argv[1] = path to the .asi (build.ps1 passes a sandbox copy).

#include <Windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::printf("usage: load_test <SDAtmos.asi>\n");
		return 2;
	}

	HMODULE module = LoadLibraryA(argv[1]);
	if (!module) {
		std::printf("FAIL: LoadLibrary error %lu\n", GetLastError());
		return 1;
	}

	std::string dir = argv[1];
	dir = dir.substr(0, dir.find_last_of("\\/") + 1);

	std::ifstream ini(dir + "SDAtmos.ini");
	if (!ini) {
		std::printf("FAIL: no default SDAtmos.ini written\n");
		return 1;
	}

	std::ifstream log(dir + "SDAtmos.log");
	std::stringstream text;
	text << log.rdbuf();
	const std::string contents = text.str();
	std::printf("%s", contents.c_str());

	const char* expected[] = { "SDAtmos loaded", "CAkSinkXAudio2::Init: 0 matches", "sink hooks MISSING, spatial bed off" };
	for (const char* line : expected) {
		if (contents.find(line) == std::string::npos) {
			std::printf("FAIL: log lacks \"%s\"\n", line);
			return 1;
		}
	}

	std::printf("PASS\n");
	return 0;
}
