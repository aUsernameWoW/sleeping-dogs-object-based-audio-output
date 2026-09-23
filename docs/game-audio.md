# The game side: UFG audio

The engine namespace is `UFG` (United Front Games). Class layouts come from the legacy PDB and the SDmodding
SDK headers (`reference\SDmodding\SDK`, e.g. `audio/audioentity.hh`, `tido/actoraudiocomponent.hh`,
`audio/oneshot.hh`, `sim/localplayer.hh`). Offsets used by the mod live in `core/game.hh`.

## AudioEntity: the Wwise game object

`UFG::AudioEntity` (0x140 bytes, 16-byte aligned) wraps one Wwise game object.

- `Init(name, transform)` (0x140146a50) calls `AK::SoundEngine::RegisterGameObj(this, name, 1)`. **Every
  `AkGameObjectID` seen in the voice pipeline is therefore the address of a `UFG::AudioEntity`.**
- `+0x18` `m_name` (`qSymbol`, a 32-bit CRC of the name string), `+0x20` `m_WorldMatrix` (`qMatrix44`; row 3
  at `+0x50` holds the world position), `+0x60` motion data, `+0x68` region data, `+0x128` initialized,
  `+0x12A` following-listener flag.
- The game frees entities (one-shots go back to their pool, the `__SFX` entity is `qFree`d) right after
  unregistering the game object, while Wwise may still render one more buffer of their voices. Any read of an
  entity from the audio thread is therefore SEH-guarded in the mod, and the game object ID is range-checked
  first (the game also uses small integers as IDs for global 2D objects).

## ActorAudioComponent: characters

`UFG::ActorAudioComponent` (0x230 bytes) = `SimComponent` (0x40) + `AudioEntity` base at `+0x40`
(`HkAudioEntityComponent`) + own fields. It is what plays a character's voice, fight impacts, vaults, climbs.

- `CheckInitialize` (0x140597c50) initializes the entity when the character is within 65 m of the listener
  (300 m for the player, 35 m for ambient peds; flag bits at `+0x228/+0x229`, `m_isPlayer` = `+0x229` bit 3),
  naming it with the owning SimObject's name (`m_pSimObject->m_Name`), and allocates a **second
  `AudioEntity`, `m_SFXEntity` (`+0x198`), named "<SimObject name>__SFX"** for effects on another bus. It
  shuts both down again when the character leaves that range.
- `+0x1C0` `m_leftFootstep` and `+0x1C8` `m_rightFootstep` are `OneShotHandle`s (each just a `OneShot*`).
- `SimComponent::m_TypeUID` (`+0x18` of the component) is `0xD2000003` for actor audio components
  (`0xD2000001` for the plain `HkAudioEntityComponent`), which is how the mod recognizes one from its entity
  address (entity − 0x40).
- The entity's world matrix is the character's transform node, i.e. the **root at the feet**; NPC speech
  therefore comes from ground level. `AudioEntityUpdate` (0x140142d80) pushes it to Wwise
  (`ForcePositionUpdate` → `AK::SoundEngine::SetPosition`) only when the character moved more than 0.1 m,
  mapping the Z-up world as Wwise (X, Y, Z) = (−world.y, world.z, world.x), orientation from matrix row 0.
  The mod's `SetPosition` hook adds `ActorLift` (default 1.5 m) to Wwise Y for actor components.
- `PlayFootstep(stepID, handle)` (0x1405a6120) sets switches on the one-shot (footwear type from the
  character or the player's current outfit, surface material from the ground under the character or
  `surface_water`, `loc_footstep_type`) and posts `loc_footsteps_p` **on the one-shot's entity**, not on the
  component's. Per-frame footstep budget `sm_maxPerFrameFootsteps`; the player is exempt.

## OneShot: fire-and-forget sounds

`UFG::OneShot` (0x170 bytes) = `AudioEntity` + `qNode` + `m_controller` (`+0x150`) + **`m_pOwnerHandle`
(`+0x158`)** + event count. Allocated from `gOneShotPool` (a fixed allocator; that is why one-shot IDs in the
log are 0x170 apart) by `OneShotPool::GetOneShot(transform)` / `GetOneShotHandle(handle, position)`.
`OneShot::Init` names it `"OneShot_%3u"` with pool index + 100, and `GetOneShotHandle` points
`m_pOwnerHandle` back at the handle that requested it.

Callers of `GetOneShotHandle` (the systems that fire positional one-shots): `ActorAudioComponent::
PlayFootstepLeft/Right`, action-tree `AudioTask::Begin` / `AudioTaskSimple::PlayOnOneShot` /
`AudioTaskSurfaceDetection` (animation-driven sounds such as the walk↔sprint transition steps),
`StateMachineComponent::DoStateTransition`, `DamageRig::PlayDamageSfx`, `GunshotManager`,
`TidoGame::PlayFightImpact`, `VehicleAudioComponent::HandleImpact`, `PhysicsCar::Update`, `PhysicsBoat`,
`CopSystem`, `DoorStateManager`, `AudioEmitterComponent`, VFX (`AudioVFXNode`, `AudioFXInstance`),
`Bullet::PlayBulletBy`, collision/constraint handlers, and the script method `TSAudio.play_sound`.

## The local player

- The player's SimObject is named **"PlayerOne_Havok"**, `qSymbol` **0x90ECB5FF** (SDK `sim/localplayer.hh`;
  verified by recomputing the CRC). `qSymbol::create_from_string` (0x140180880) is CRC-32 with polynomial
  0x04C11DB7, MSB-first, initial value -1, no final xor (table `sCrcTable32`).
- His sounds therefore come from three kinds of game object:
  1. the `ActorAudioComponent` entity, name 0x90ECB5FF (voice, vaults, climbs, hits taken);
  2. its `__SFX` entity (quiet foley on a bus with dry mix 0.63);
  3. one-shots: footsteps owned by the component's handles, and transition/animation sounds owned by
     action-tree tasks (handle inside the task object), both positioned at the player.
- With the listener on the camera these sit at r ≈ 2.6-4 m, θ ≈ -3…-13° (the over-the-shoulder camera puts
  the character left of center), φ ≈ -20…-31° (feet lower than chest).

## The listener

`UFG::AudioListener` (136 bytes; singleton `sm_pInstance` at RVA 0x2175E30 in the legacy build), updated
every frame by `AudioListener::Update` (0x14014d410):

- **Orientation is always the camera's** (`Director::mCurrentCamera`, or the editor camera).
- **Position** is the camera's when `m_positionListenerAtCamera` (`+0x81`, default true) is set, otherwise
  the local player's transform (`gSim.mpLocalPlayer->m_pTransformNodeComponent->mWorldTransform`). Either
  way it is smoothed by 20 % per frame. `m_lockListenerPosition` (`+0x83`) freezes it;
  `m_triggerPositionAtCamera` (`+0x82`) chooses the same for trigger regions.
- Velocity is clamped to ±27.8 m/s (100 km/h) and a "high speed mode" flag is derived from it (the
  footstep code suppresses footsteps in high speed mode).
- `Audio3DListener::Update` (0x14014d1a0) converts the matrix into an `AkListenerPosition` and calls
  `SetListenerPosition` for each listener ID.
- Script methods on `TSWorld`: `audio_set_listener_at_camera`, `audio_set_listener_at_player`,
  `audio_lock_listener_position`, `audio_unlock_listener_position`, `audio_listener_enable/disable_high_speed`.
  So the engine natively supports the "attenuation from the player, panning from the camera" hybrid, and
  scripts switch to it in specific situations. Flipping the byte from the mod is an untried experiment.

## Buses seen in-game

Only insert effects are logged (`objects: bus ... fx:` lines), so the bus tree is only partially known:

- A top-level bus (parent = null) with a Parametric EQ that most world SFX go through.
- A bus with a Meter (side-chain metering for ducking/RTPC; ignored by the router). Note that carving object
  voices out of the bed lowers what such meters measure; no audible consequence found so far.
- The master (device final mix) has no insert effects.
- The game sends nothing to LFE (≈ -110 dBFS all session); bass management is the receiver's job.

Voice counts observed: up to ~38 dry voices, ~36 of them 3D, ~30 aux sends in a street fight; 468 of 544
logged 3D voices were mono; many ambience emitters use full spread (gains 0.41-0.45 over 5-6 speakers).
