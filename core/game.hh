#pragma once

// The little the router needs to know about the game (UFG engine) side of the audio, from the legacy PDB and
// the SDmodding SDK. Layouts are data, not code, so they hold for the installed build too.

#include <excpt.h>

#include <cstddef>
#include <cstdint>

namespace game
{
	// Wwise game object IDs are game pointers that may already be freed (see docs/game-audio.md) or not
	// pointers at all, so every read of game memory goes through these SEH-guarded helpers (no C++ objects in
	// a function with __try).
	inline bool IsPointer(uint64_t value)
	{
		// The game also uses small numbers as IDs for global (2D) objects.
		return value >= 0x10000 && value < 0x00007FFFFFFF0000ull;
	}

	inline bool ReadU32(uint64_t address, uint32_t& value)
	{
		__try {
			value = *reinterpret_cast<const uint32_t*>(address);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	inline bool ReadU64(uint64_t address, uint64_t& value)
	{
		__try {
			value = *reinterpret_cast<const uint64_t*>(address);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	inline bool ReadFloats3(uint64_t address, float* xyz)
	{
		__try {
			const float* p = reinterpret_cast<const float*>(address);
			xyz[0] = p[0];
			xyz[1] = p[1];
			xyz[2] = p[2];
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// qSymbol("PlayerOne_Havok"): the local player's SimObject name (CRC-32, poly 0x04C11DB7, no final xor).
	constexpr uint32_t kPlayerNameUID = 0x90ECB5FF;

	// UFG::AudioEntity::Init registers the entity with Wwise as RegisterGameObj(this, ...): every AkGameObjectID
	// the voices carry is the address of a UFG::AudioEntity.
	namespace audio_entity
	{
		constexpr size_t kName = 0x18;     // qSymbol m_name: the owning SimObject's name for actor components
		constexpr size_t kPosition = 0x50; // qMatrix44 m_WorldMatrix (+0x20) row 3: world x, y, z (meters)
	}

	namespace sim_component // UFG::SimComponent, the base of every component (0x40 bytes)
	{
		constexpr size_t kTypeUID = 0x18; // uint32 m_TypeUID
	}

	// UFG::ActorAudioComponent = SimComponent (0x40) + AudioEntity base + own fields. Its AudioEntity base is
	// the game object the voice, vaults and hits play on; a second, heap-allocated AudioEntity ("<name>__SFX")
	// carries the character's other effects. Its world position is the character's transform, i.e. the root
	// at the feet.
	namespace actor_audio
	{
		constexpr uint32_t kTypeUID = 0xD2000003;
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
