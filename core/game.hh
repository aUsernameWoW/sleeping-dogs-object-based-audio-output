#pragma once

// The little the router needs to know about the game (UFG engine) side of the audio, from the legacy PDB and
// the SDmodding SDK. Layouts are data, not code, so they hold for the installed build too.

#include <cstddef>
#include <cstdint>

namespace game
{
	// qSymbol("PlayerOne_Havok"): the local player's SimObject name (CRC-32, poly 0x04C11DB7, no final xor).
	constexpr uint32_t kPlayerNameUID = 0x90ECB5FF;

	// UFG::AudioEntity::Init registers the entity with Wwise as RegisterGameObj(this, ...): every AkGameObjectID
	// the voices carry is the address of a UFG::AudioEntity.
	namespace audio_entity
	{
		constexpr size_t kName = 0x18; // qSymbol m_name: the owning SimObject's name for actor components
	}

	// UFG::ActorAudioComponent = SimComponent (0x40) + AudioEntity base + own fields. Its AudioEntity base is
	// the game object the footsteps and voice play on; a second, heap-allocated AudioEntity ("<name>__SFX")
	// carries the character's other effects.
	namespace actor_audio
	{
		constexpr size_t kSize = 0x230;
		constexpr size_t kEntityBase = 0x40;    // AudioEntity subobject inside the component
		constexpr size_t kSfxEntity = 0x198;    // AudioEntity* m_SFXEntity (component-relative)
		constexpr size_t kLeftFootstep = 0x1C0; // OneShotHandle m_leftFootstep: { OneShot* }
		constexpr size_t kRightFootstep = 0x1C8;
	}

	// UFG::OneShot (0x170 bytes, pooled) is an AudioEntity for fire-and-forget sounds: footsteps, impacts.
	// OneShotPool::GetOneShotHandle names it "OneShot_%3u" (pool index + 100) and points m_pOwnerHandle back
	// at the handle that owns it, which for footsteps lives inside the actor's audio component.
	namespace one_shot
	{
		constexpr size_t kOwnerHandle = 0x158; // OneShotHandle*
		constexpr uint32_t kFirstIndex = 100;
		constexpr uint32_t kMaxIndex = 999;    // "%3u" of anything larger would be 4 digits; pools are far smaller
	}

	// qSymbol::create_from_string: CRC-32 (poly 0x04C11DB7, MSB first, init -1, no final xor).
	inline uint32_t SymbolUID(const char* text)
	{
		static uint32_t table[256];
		static bool ready = false;
		if (!ready) {
			for (uint32_t i = 0; i < 256; ++i) {
				uint32_t c = i << 24;
				for (int bit = 0; bit < 8; ++bit) {
					c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
				}
				table[i] = c;
			}
			ready = true;
		}
		uint32_t crc = 0xFFFFFFFFu;
		for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
			crc = (crc << 8) ^ table[(*p ^ (crc >> 24)) & 0xFF];
		}
		return crc;
	}
}
