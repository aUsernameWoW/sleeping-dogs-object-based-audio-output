#include "weather.hh"

#include <Windows.h>

#include <cstdint>

#include "log.hh"
#include "scan.hh"

namespace weather
{
	// UFG::TSWorld::MthdC_weather_set_amount: reads the float argument, calls TimeOfDayManager::GetInstance()
	// (the E8 at +0x19), clamps to [0, 2] and stores state/target/next target at +0x34/+0x38/+0x3C.
	constexpr char kSigSetAmount[] =
		"48 83 EC 38 48 8B 41 60 0F 29 74 24 20 48 8B 08 48 8B 41 08 F3 0F 10 70 20 E8 ? ? ? ? 0F 57 C0 0F 2F F0 76 0F "
		"F3 0F 10 05 ? ? ? ? 0F 2F F0 72 05 EB 06 0F 28 F0 0F 28 C6 0F 28 74 24 20 F3 0F 11 40 34 F3 0F 11 40 38 F3 0F 11 40 3C";
	constexpr size_t kGetInstanceCall = 0x19;

	// UFG::TimeOfDayManager (legacy PDB): weather state 0 = sunny .. 2 = full rain (is_raining = state > 1).
	namespace tod
	{
		constexpr size_t kWeatherState = 0x34;       // float
		constexpr size_t kWeatherTarget = 0x38;      // float
		constexpr size_t kNextWeatherTarget = 0x3C;  // float
		constexpr size_t kRandomInterval = 0x40;     // int; LockWeather moves it to PreLock and zeroes it
		constexpr size_t kRandomIntervalPreLock = 0x44;
		constexpr size_t kChanceOfPrecipitation = 0x4C; // float 0..1
	}

	using GetInstanceFn = uint8_t* (*)();
	static GetInstanceFn gGetInstance = nullptr;

	void Install()
	{
		uint8_t* setAmount = scan::FindUnique("TSWorld::MthdC_weather_set_amount", kSigSetAmount);
		if (setAmount && setAmount[kGetInstanceCall] == 0xE8) {
			gGetInstance = reinterpret_cast<GetInstanceFn>(scan::RipTarget(setAmount + kGetInstanceCall + 1));
		}
		LOG("weather: TimeOfDayManager::GetInstance %s", gGetInstance ? "found" : "MISSING (rain hotkey off)");
	}

	template <typename T>
	static T& Field(uint8_t* manager, size_t offset)
	{
		return *reinterpret_cast<T*>(manager + offset);
	}

	bool IsRaining()
	{
		if (!gGetInstance) {
			return false;
		}
		return Field<float>(gGetInstance(), tod::kWeatherState) > 1.0f;
	}

	bool ToggleRain()
	{
		if (!gGetInstance) {
			return false;
		}
		uint8_t* manager = gGetInstance();
		const bool rain = !(Field<float>(manager, tod::kWeatherState) > 1.0f);
		const float amount = rain ? 2.0f : 0.0f;
		// weather_set_amount + weather_lock: jump straight to the amount and stop the randomizer from undoing
		// it; unlock restores the interval the game had.
		Field<float>(manager, tod::kWeatherState) = amount;
		Field<float>(manager, tod::kWeatherTarget) = amount;
		Field<float>(manager, tod::kNextWeatherTarget) = amount;
		int& interval = Field<int>(manager, tod::kRandomInterval);
		int& preLock = Field<int>(manager, tod::kRandomIntervalPreLock);
		if (rain) {
			if (interval) {
				preLock = interval;
				interval = 0;
			}
		}
		else if (!interval) {
			interval = preLock;
		}
		LOG("weather: rain %s (state %.1f, random interval %d, chance of precipitation %.2f)", rain ? "ON" : "OFF",
			amount, interval, Field<float>(manager, tod::kChanceOfPrecipitation));
		return rain;
	}
}
