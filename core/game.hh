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
		constexpr size_t kEntityBase = 0x40;  // AudioEntity subobject inside the component
		constexpr size_t kSfxEntity = 0x198;  // AudioEntity* m_SFXEntity (component-relative)
	}
}
