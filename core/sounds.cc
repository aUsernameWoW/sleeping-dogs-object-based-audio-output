#include "sounds.hh"

#include <cstdio>

#include "log.hh"
#include "wwise.hh"

namespace sounds
{
	using namespace wwise;

	// Open addressing, filled to at most 3/4. The banks hold a few thousand sound objects in total (Init.bnk
	// alone has 2589 HIRC objects, the SFX banks more); far fewer play in one session.
	constexpr int kTable = 8192;
	constexpr int kCapacity = kTable * 3 / 4;
	static Info gTable[kTable];
	static int gCount = 0;
	static bool gFullLogged = false;

	void Walk(const void* sound, uint32_t soundID, Info& out)
	{
		out = {};
		out.mID = soundID;
		// CAkParameterNodeBase::GetControlBus: the first output bus up the parent chain (sound → containers →
		// actor-mixers)...
		const void* bus = nullptr;
		const void* node = sound;
		for (int depth = 0; node && depth < 32 && !bus; ++depth) {
			bus = At<void*>(node, node::kBusOutputNode);
			node = At<void*>(node, node::kParentNode);
		}
		// ...then the bus's own output buses up to the root.
		for (int depth = 0; bus && depth < kMaxChain; ++depth) {
			out.mChain[out.mChainLen++] = At<uint32_t>(bus, indexable::kID);
			bus = At<void*>(bus, node::kBusOutputNode);
		}
	}

	Info* Lookup(const void* sound, uint32_t soundID)
	{
		if (!soundID || !sound) {
			return nullptr;
		}
		uint32_t slot = (soundID * 2654435761u) % kTable;
		for (int probe = 0; probe < kTable; ++probe, slot = (slot + 1) % kTable) {
			Info& entry = gTable[slot];
			if (entry.mID == soundID) {
				return &entry;
			}
			if (entry.mID == 0) {
				if (gCount >= kCapacity) {
					if (!gFullLogged) {
						gFullLogged = true;
						LOG("sounds: table full (%d sounds); later sounds are classified without caching", gCount);
					}
					return nullptr;
				}
				Walk(sound, soundID, entry);
				++gCount;
				return &entry;
			}
		}
		return nullptr;
	}

	bool Listed(const Info& info, const uint32_t* list, int count, int* depth)
	{
		for (int i = 0; i < info.mChainLen; ++i) {
			for (int k = 0; k < count; ++k) {
				if (list[k] == info.mChain[i]) {
					if (depth) {
						*depth = i;
					}
					return true;
				}
			}
		}
		return false;
	}

	const char* BusName(uint32_t id)
	{
		switch (id) {
		case 3444197610: return "root";
		case 3803692087: return "master_audio_bus";
		case 805203703: return "master_secondary_bus";
		case 1900298039: return "master_music";
		case 3627036714: return "master_dialog";
		case 3946296192: return "master_aux";
		case 3995202064: return "master_hdr";
		case 3462011115: return "master_sfx";
		case 77978275: return "ambient";
		case 317282339: return "weather";
		case 186852181: return "thunder";
		case 1537061107: return "wind";
		case 2043403999: return "rain";
		case 352130103: return "birds";
		case 3888786832: return "city";
		case 3463109076: return "traffic";
		case 689383231: return "crowd_market";
		case 1587111019: return "crowd_club";
		case 1854869158: return "crowd_restaurant";
		case 2458178259: return "water_amb";
		case 1930490682: return "boat_amb";
		case 1830469890: return "interior_rain";
		case 393239870: return "sfx";
		case 2923970681: return "sfx_misc";
		case 640021946: return "efforts";
		case 556887514: return "locomotion";
		case 2385628198: return "footsteps";
		case 247557814: return "foley";
		case 168610243: return "fighting";
		case 143479525: return "fight_foley";
		case 1399234913: return "fight_impacts";
		case 3543689402: return "fight_falls";
		case 3001040443: return "gunplay";
		case 1287408361: return "gunshot";
		case 1431829326: return "gunshot_ai";
		case 1312752045: return "gunshot_close_ai";
		case 3524362703: return "gunshot_close_mid";
		case 3928849794: return "gunshot_close_far";
		case 3936260057: return "bullet_impacts";
		case 3968426781: return "ricochets";
		case 134114496: return "shells";
		case 824618274: return "collisions";
		case 3634272999: return "collisions_light";
		case 2449969375: return "glass";
		case 1189545500: return "car_glass";
		case 3713103246: return "veh_player";
		case 2010610765: return "veh_engine_player";
		case 4047272965: return "veh_skids_player";
		case 2340005816: return "veh_traffic";
		case 447211353: return "veh_engine_traffic";
		case 3317730855: return "veh_horns_traffic";
		case 987980167: return "veh_ai";
		case 1667833844: return "veh_engine_ai";
		case 1683900444: return "veh_skids_ai";
		case 1935586686: return "veh_horns_ai";
		case 127263653: return "veh_misc";
		case 2102979017: return "police_siren";
		case 900703796: return "pipe_steam";
		case 1564565979: return "ambient_music";
		case 1430098981: return "amb_mus_int";
		case 0: return "-";
		default: return "?";
		}
	}

	const char* FormatChain(const Info& info, char* buffer, size_t size)
	{
		size_t used = 0;
		buffer[0] = '\0';
		for (int i = 0; i < info.mChainLen && used + 1 < size; ++i) {
			const uint32_t id = info.mChain[i];
			const char* name = BusName(id);
			int written = 0;
			if (name[0] == '?') {
				written = snprintf(buffer + used, size - used, "%s%u", i ? ">" : "", id);
			}
			else {
				written = snprintf(buffer + used, size - used, "%s%s", i ? ">" : "", name);
			}
			if (written < 0) {
				break;
			}
			used += static_cast<size_t>(written);
			if (used >= size) {
				used = size - 1;
				break;
			}
		}
		if (!info.mChainLen) {
			snprintf(buffer, size, "-");
		}
		return buffer;
	}
}
