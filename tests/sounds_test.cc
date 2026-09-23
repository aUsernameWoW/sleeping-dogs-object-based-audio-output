// The per-sound bank chain cache (core/sounds.cc) against fake CAkParameterNodeBase objects: the walk follows
// m_pParentNode up to the first m_pBusOutputNode the way GetControlBus does, then the buses' own output
// buses; lookups are cached per sound ID; a full table degrades to uncached walks without losing the entries
// it has. Compiles sounds.cc directly; argv[1] (the .asi path) is unused.

#include "../core/sounds.cc"

#include <cstdio>
#include <cstring>
#include <vector>

// sounds.cc logs through logger::Write; the test has no log file.
namespace logger
{
	void Write(const char*, ...) {}
}

namespace
{
	// Just enough of CAkParameterNodeBase: ID at indexable::kID, parent and output bus pointers.
	struct alignas(16) Node
	{
		uint8_t mBytes[0x80] = {};

		Node(uint32_t id, const Node* parent, const Node* outputBus)
		{
			wwise::At<uint32_t>(mBytes, wwise::indexable::kID) = id;
			wwise::At<const void*>(mBytes, wwise::node::kParentNode) = parent;
			wwise::At<const void*>(mBytes, wwise::node::kBusOutputNode) = outputBus;
		}
	};

	int gFailures = 0;

	void Check(bool ok, const char* what)
	{
		if (!ok) {
			std::printf("FAIL: %s\n", what);
			++gFailures;
		}
	}
}

int main()
{
	// Buses: gunshot > sfx > master_sfx > master_hdr > 4242 (unnamed) > root.
	Node root(3444197610u, nullptr, nullptr);
	Node unnamed(4242, nullptr, &root);
	Node masterHdr(3995202064u, nullptr, &unnamed);
	Node masterSfx(3462011115u, nullptr, &masterHdr);
	Node sfx(393239870u, nullptr, &masterSfx);
	Node gunshot(1287408361u, nullptr, &sfx);
	// Sound > random container (no output bus of its own) > actor-mixer routed to gunshot.
	Node actorMixer(500, nullptr, &gunshot);
	Node container(501, &actorMixer, nullptr);
	Node sound(777, &container, nullptr);

	sounds::Info* info = sounds::Lookup(&sound, 777);
	Check(info != nullptr, "first lookup caches");
	Check(info && info->mID == 777, "entry carries the sound ID");
	Check(info && info->mChainLen == 6, "chain has six buses");
	Check(info && info->mChain[0] == 1287408361u && info->mChain[5] == 3444197610u, "chain runs from the output bus to the root");

	char text[200];
	sounds::FormatChain(*info, text, sizeof(text));
	Check(std::strcmp(text, "gunshot>sfx>master_sfx>master_hdr>4242>root") == 0, "FormatChain names known buses, prints unknown IDs");

	int depth = -1;
	const uint32_t list[] = { 12345, 3462011115u };
	Check(sounds::Listed(*info, list, 2, &depth) && depth == 2, "Listed finds master_sfx at depth 2");
	Check(!sounds::Listed(*info, list, 1), "Listed misses an unrelated ID");

	info->mShapeLogged = true;
	Check(sounds::Lookup(&sound, 777) == info && info->mShapeLogged, "second lookup returns the cached entry with its flags");

	// A sound with no bus at all: empty chain, formatted as "-".
	Node loose(778, nullptr, nullptr);
	sounds::Info* looseInfo = sounds::Lookup(&loose, 778);
	Check(looseInfo && looseInfo->mChainLen == 0, "no output bus: empty chain");
	Check(std::strcmp(sounds::FormatChain(*looseInfo, text, sizeof(text)), "-") == 0, "empty chain formats as -");

	Check(sounds::Lookup(&sound, 0) == nullptr, "sound ID 0 is never cached");

	// A chain deeper than kMaxChain is cut, not overrun.
	std::vector<Node> deep;
	deep.reserve(sounds::kMaxChain + 5);
	deep.emplace_back(9000, nullptr, nullptr);
	for (int i = 1; i < sounds::kMaxChain + 5; ++i) {
		deep.emplace_back(9000 + i, nullptr, &deep[i - 1]);
	}
	Node deepSound(779, nullptr, &deep.back());
	sounds::Info* deepInfo = sounds::Lookup(&deepSound, 779);
	Check(deepInfo && deepInfo->mChainLen == sounds::kMaxChain, "chain is capped at kMaxChain");

	// Fill the table: entries beyond the capacity are refused, earlier ones stay.
	int cached = 3;
	for (uint32_t id = 100000; id < 100000 + static_cast<uint32_t>(sounds::kCapacity) + 200; ++id) {
		cached += sounds::Lookup(&loose, id) != nullptr;
	}
	Check(cached == sounds::kCapacity, "table caches exactly kCapacity sounds");
	Check(sounds::Lookup(&sound, 777) == info, "the first entry survives a full table");
	Check(sounds::Lookup(&loose, 100000 + static_cast<uint32_t>(sounds::kCapacity) + 500) == nullptr, "a full table refuses new sounds");
	sounds::Info local;
	sounds::Walk(&sound, 777, local);
	Check(local.mChainLen == 6 && local.mChain[0] == 1287408361u, "Walk still works without the table");

	if (gFailures) {
		return 1;
	}
	std::printf("sounds_test: ok\n");
	return 0;
}
