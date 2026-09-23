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

## The bus hierarchy (Init.bnk)

`Init.bnk` (bank ID 0x50C63A23, 94 KB, chunks STMG/HIRC/ENVS) sits inside `data\Audio\SD2\SFX.pck`. The
AKPK header is `AKPK, header size, version 1, language-map length, banks-table length, streams-table
length`; the banks table is `count` then 24-byte entries `id, block size, size, pad, offset (in blocks),
language`. The HIRC chunk holds 2589 objects (`type u8, size u32, id u32, body`): 279 buses (type 8),
28 aux buses (type 19), 31 effect instances (type 18: 27 ConvolutionReverb, Meters, EQs, one
MatrixReverb), 2204 states and a few others. A bus body starts with its parent's ID.

Bus names are FNV-1 (32-bit, lowercase) hashes; a dictionary attack recovered 59 of 307, enough for the
routing decisions. The relevant part of the tree (IDs in decimal, `?` = name unknown):

```
3444197610 ?  (root; "Master Audio Bus" 3803692087 is not used, 805203703 = Master Secondary Bus)
├ 1900298039 master_music      (Parametric EQ; ui_music, ambient_music, radio_car, karaoke_player...)
├ 2640427754 ?
│ └ 1973600711 ?
│   ├ 3627036714 master_dialog
│   ├ 3946296192 master_aux     (env, meter; where the aux buses return)
│   ├ 3995202064 master_hdr
│   │ ├ 1970697714 ? (indoor ...)
│   │ └ 3462011115 master_sfx
│   │   ├ 77978275 ambient       ← height bed "ambience" tier
│   │   │ └ 2276207995 ?
│   │   │   ├ 317282339 weather  ← "sky" tier: 186852181 thunder (distant/close), 1537061107 wind, 2043403999 rain
│   │   │   ├ 352130103 birds    ← "sky" tier
│   │   │   ├ 3888786832 city (Meter) → 3463109076 traffic
│   │   │   ├ crowds (689383231 crowd_market, 1587111019 crowd_club, 1854869158 crowd_restaurant)
│   │   │   ├ 2458178259 water_amb, 1930490682 boat_amb, 1830469890 interior_rain, kitchens...
│   │   └ 393239870 sfx         (fight_foley, fight_impacts, fight_falls, foley, footsteps (Meter),
│   │                             collisions (Meter), glass, ui, gunshot, bullet_impacts, ricochets,
│   │                             police_siren (Meter), 1667833844 (Parametric EQ)...)
│   └ 4167303992 ? (medium_explosion...)
└ 3474110328 ?
28 aux buses (type 19), each running a ConvolutionReverb/MatrixReverb shareset  ← "reverb" tier
```

Observed in-game before the bank was parsed: a top-level bus with a Parametric EQ (1900298039 = master_music,
also 1667833844 under sfx), Meters (side-chain metering for ducking/RTPC; ignored by the router; carving
object voices out of the bed lowers what they measure, no audible consequence found), no effects on the
master. The game sends nothing to LFE (≈ -110 dBFS all session); bass management is the receiver's job.

Voice counts observed: up to ~38 dry voices, ~36 of them 3D, ~30 aux sends in a street fight; 468 of 544
logged 3D voices were mono; many ambience emitters use full spread (gains 0.41-0.45 over 5-6 speakers).
