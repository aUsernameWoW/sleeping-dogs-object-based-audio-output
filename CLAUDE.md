# SDAtmos

Goal: Sleeping Dogs DE outputs real object audio over HDMI so the soundbar shows Dolby Atmos instead of
Dolby Audio. Windows' "Dolby Atmos for home theater" doesn't upmix; only apps that use
`ISpatialAudioClient` (ISAC) get sent as Dolby MAT with object metadata. So the mod routes the game's audio
through ISAC itself: the 7.1 mix as a static bed, and selected 3D voices as dynamic objects.

Nothing is Dolby-specific: ISAC is format-agnostic, and object count, native bed layout and format are
queried at runtime, so DTS:X for Home Theater / Windows Sonic should work too (untested: the user has Atmos).

Status (2026-09-22):
- Standalone test passed (soundbar shows Atmos, height audible).
- Milestones 2+3 verified in-game (North Point safehouse → night market fight): bed through ISAC, soundbar
  on Atmos, zero underruns/overflows over ~4 min, ring fill 1408..2400 frames.
- Milestone 4 (dynamic objects) verified in-game: avg 5-11 objects, peaks at the 20 limit in fights, zero
  activation failures, elevation reaching the renderer (e.g. phi 62°). Not yet judged by ear.
- Debug overlay (ReShade menu tab + HUD radar/markers, F9 A/B, F8 HUD) works in-game. The default marker FOV
  (60° vertical) lines up with the sources, which confirms the Wwise listener is the camera. Cyan (object)
  voices are the ones the user expected. Radar not looked at yet.
- Seen on the HUD: NPC voices are positioned at the NPC's feet: `ActorAudioComponent`'s entity follows the
  character root. Since 2026-09-23 a `SetPosition` hook lifts actor entities by `ActorLift` (default 1.5 m,
  Wwise Y = world up) before Wwise sees them; awaiting the in-game check (NPC speech markers at head
  height, no audible level change).
- 2026-09-23: the user heard Wei Shen's own footsteps as a sharp point ~3 m ahead, slightly left (the
  over-the-shoulder camera). His voices are now identified by game object and kept in the bed
  (`PlayerInBed`, HUD green). First test: vault/climb sounds went green (the component's own entity), the
  footsteps stayed cyan: they play on pooled `OneShot` entities, now matched through their owner handle.
  Second test: "near perfect"; only the 2-3 transition steps when walking↔sprinting (L-Shift) stayed cyan.
  Those one-shots are fired by action-tree audio tasks (handle inside the task), so one-shots within 1.5 m
  of the player's audio entity now count as his too. Third test: all of Wei's sounds green; F9 A/B judged
  by ear for the first time: the objects make the whole mix clearly directional, even from an off-center
  seat. The user is happy with the state as of 2026-09-23.
- Same day, first log with the router changes: demotions per 5 s fell from ~47 to 0-4. Only one bus carries
  an effect: a top-level bus (id 1900298039, feeds the final mix) with a Parametric EQ; the master has none.
  `BusFx = 1` had pushed everything under it into the bed (3/20 objects on the HUD), so objects now run that
  EQ themselves instead.
- 2026-09-23, later: **height bed** built (`docs\height-bed.md`): the bed is 7.1.4, the four top channels are
  fed from the ambient/weather/birds buses and the reverb aux buses at their bus→parent transfer, decorrelated.
  First in-game hour: stream has the 12-channel bed, the reverb tier carves constantly (peaks -13..-40
  dBFS), but **the ambience tier never fired** (`0 ambience` all session): the `ambient` bus 77978275 never
  passed the transfer hook, so the runtime routing of ambience differs from the Init.bnk tree as parsed.
  Diagnostics added: every bus is logged once at its first transfer (`heights: bus <id> (<name>) -> <parent>
  ..., mask ..., tier`), and voice snapshots print the dry bus chain (`| bus a>b>c`). No rain fell in that
  hour either, so a debug hotkey **F6** forces rain via `UFG::TimeOfDayManager` (`core/weather.*`).
- Second session: the runtime bus tree has ~12 buses; nearly all SFX voices mix straight into `master_hdr`
  (3995202064). Cause: Wwise 2012 only instantiates *mixing* buses (`CAkBus::IsMixingBus`: FX, aux, channel
  config, positioning, HDR or master); `ambient`/`weather` have none and fold away. **Sky/ambience are now
  per voice**: the sound's bank-side bus chain (`m_pParentNode`/`m_pBusOutputNode`) is walked once per sound
  ID, the nearest listed bus decides, and the carve scales the voice's `AkAudioMix` floor gains (after the
  object router). Reverb stays bus-level. Third session **confirmed it mechanically**: on a street ~3500
  ambience voice mixes per 10 s (traffic 3463109076, city, crowds, water), F6 rain → ~1200-1500 sky mixes
  per 10 s (rain 2043403999, distant thunder 426107819, 1131612005 under weather), reverb 470-1000 bus
  transfers; height peaks -19..-30 dBFS in rain, floor unchanged elsewhere, zero underruns. Back heights
  peak ~-30 vs front ~-21 in rain (the rain loops are front-weighted). Listening verdict (user, same day):
  "absolutely incredible", a big difference, rain clearly audible overhead. No finer tuning requests yet;
  the user may try Dolby Atmos for Headphones next to judge elevation/precision.
- Then, at the user's request: 2D sky voices (rain/wind loops) now use a spread map (each side over both of
  its heights) so the ceiling is even front-to-back; bed voices above the horizon are lifted by sin(phi) ×
  `Elevation` (default 0 dB) through the directional map, objects excluded. Built, tests pass, deployed;
  **awaiting the in-game check** (log: `lifted by elevation`, `E elevated voice mixes`; rain TBL/TBR peaks
  should now match TFL/TFR).

Long-form documentation for humans is in `docs\` (architecture, Wwise internals, game audio, spatial output,
voice router, reverse-engineering workflow, testing/logs). Keep both in sync: this file is the summary,
`docs\` the detail.

## Files

- `dllmain.cc` — config, log, `wwise::Install()` from DllMain (before the game initializes Wwise).
- `core/wwise.hh` — Wwise 2012.2 struct layouts/offsets from the legacy PDB.
- `core/game.hh` — the game (UFG) side: AudioEntity/ActorAudioComponent offsets, the player's name hash.
- `core/wwise_hooks.*` — signatures + hooks: `CAkSinkXAudio2::Init/PassData/PassSilence`,
  `CAkLEngine::RunVPL`, `CAkVPLMixBusNode::ConsumeBuffer` (voice variant), the bus→parent variants
  `CAkVPLMixBusNode::ConsumeBuffer(AkAudioBufferBus&,...)` + `CAkVPLFinalMixNode::ConsumeBuffer` (height bed),
  `AK::SoundEngine::SetPosition` (actor lift); voice snapshot logging.
- `core/objects.*` — voice router: which voices become objects, sample capture, bed/object crossfades,
  per-voice report (position, level, role, why it stays in the bed).
- `core/heights.*`, `core/height_dsp.hh` — height bed: tiers (sky / ambience by bus ID, reverb by bus FX),
  energy-preserving carve at the bus transfer, decorrelator (pre-delay + all-passes + high-pass); the DSP
  header has no engine dependencies (tested offline).
- `core/telemetry.*` — per-buffer voice snapshot, audio thread → render thread (try-lock, never blocks audio).
- `core/overlay.*` — hotkey thread (F9 objects A/B, F7 heights A/B, F8 HUD), ReShade add-on: "SDAtmos" menu
  tab (settings, stream status, voice table, save to ini) and HUD (radar, on-screen markers, A/B banner).
- `core/spatial_out.*` — ISAC stream (bed incl. heights + dynamic objects), SPSC ring with per-block object
  metadata, render thread, object activation/reuse/release, fold-into-bed fallback, reopen on device loss.
- `core/weather.*` — debug: F6 forces rain/clear through the game's `UFG::TimeOfDayManager` (found via the
  `weather_set_amount` script atomic's call to `GetInstance`; state/target/next at +0x34/38/3C, random
  interval +0x40 zeroed to lock, as `weather_lock` does).
- `core/scan.*` — unique pattern search in the exe's `.text`, RIP-relative decoding.
- `core/config.*`, `core/log.*` — `SDAtmos.ini` / `SDAtmos.log`.
- `tests/load_test.cc` (automated: loads into a Wwise-less process), `tests/config_save_test.cc` (automated:
  `config::Save` keeps UTF-8 comments/other keys, adds missing ones), `tests/heights_test.cc` (automated:
  height DSP maps, carve energy, decorrelator), `tests/spatial_orbit_manual.cc` (standalone ISAC check:
  `--probe` prints limits, otherwise plays a circling object).
- `.github/workflows/build.yml` — CI on GitHub Actions (`windows-2025-vs2026`): recreates the workspace
  layout from pinned commits (`env:` `RESHADE_REF` = v6.8.0 + `deps/imgui`, `SPATCH_REF` for MinHook; sparse
  checkouts, cached under the pins), builds Release x64 with `-warnAsError`, runs `tests\*_test.cc`, uploads
  `.asi` + `.pdb`; on `main` a second job publishes them as prerelease `build-<N>` (N = commit count). Bump
  the pins when `reference\` moves; `.github/dependabot.yml` proposes updates for the SHA-pinned actions
  monthly.

## Design decisions (don't undo without reason)

- **XAudio2 stays the clock.** Wwise renders when XAudio2 frees a queue slot; the hook copies each buffer
  into our ring and zeroes it, so XAudio2 plays silence. Same endpoint clock → no drift, and Wwise's pacing
  logic is untouched. If the ISAC stream can't open or dies, `spatial::IsActive()` is false and the hook
  leaves XAudio2 audible (automatic fallback).
- **Copy/zero `m_MasterOut` before the original `PassData`**, not after: XAudio2 reads the ring
  asynchronously once `m_uNbBuffersRB` is incremented.
- **Force 7.1** (`Init` channel mask 0 → 0x63F) only when the default endpoint offers ISAC; otherwise Wwise
  keeps the device's layout.
- Ring latency: prime 2048 frames, trim when the slack stays above 3072 for 2 s. XAudio2's deeper queue hides
  Wwise stalls from the original path, not from ours, hence the generous slack.
- **Objects are carved out of the voice's dry mix, not re-rendered.** The original `ConsumeBuffer` is always
  called; for an object voice its `AkAudioMix` is scaled to 0 (or to the bed's share during a crossfade), so bus
  state/FX setup stay normal. The object gets the voice's pre-pan PCM × the same Previous→Next gain ramp
  `CAkMixer::Mix3D` uses × `AkVPL::m_fDownstreamGain` (bus-chain volume: sliders, ducking; recomputed every
  frame in `CAkLEngine::AnalyzeMixingGraph`). Aux sends (reverb) are untouched → stay in the bed.
- **Voice router policy** (matches Dolby/Wwise practice: point sources → objects, diffuse → bed, overflow folds
  into the bed): candidates are mono, single-ray, 3D, point-like (max speaker gain / total ≥ 0.6; Wwise spread
  gives ~0.41-0.45). New candidates take a free slot immediately (attack is an object from sample 0, no
  crossfade needed); every frame candidates are re-ranked by gain × RMS × downstream, current objects ×2,
  roles held ≥ 12 frames; bed↔object moves crossfade over one 1024-frame buffer.
- **Objects ride out their decay** (2026-09-23): the first log showed ~47 demotions per 5 s in fights, mostly
  tails dropping under -70 dB or spread hovering at the 0.6 threshold, each a 21 ms position jump. Now a
  current object is only demoted when outranked by louder candidates, when its pan gets clearly spread
  (point-likeness < 0.5), when a bed rule applies (player, bus fx), or when it stops being mono/single-position.
  `kMinLevel` only gates new objects.
- **Bus insert effects**: objects skip the bus chain, so bus EQ/compression/limiting/slow-motion filters
  would be missing from them. Checked per buffer by walking `AkVPL::m_pParent` up from the voice's dry bus
  and reading `CAkBusFX::m_aFX` (effect present, not bypassed, Meter ignored). **Parametric EQs are
  reproduced on the object**: `CAkParametricEQFX` keeps the per-band biquad coefficients (already normalized,
  a1/a2 negated) and its params hold band on/off, dirty flags and the output level, so `ApplyEq` runs the
  same filters on the object's samples with per-slot memories (a dirty band is skipped for one buffer).
  Effects that can't be reproduced keep the voice in the bed under `BusFx` (default 1; the device's final
  mix, looked up via `CAkOutputMgr::m_Devices`, only counts under policy 2 since a master limiter would rule
  out every object). The log lists each bus's active effects when first seen/changed.
- **Objects never get dropped**: if Windows refuses a dynamic object, the render thread pans that slot into the
  bed (constant power between adjacent bed speakers).
- **Height bed: reverb per bus, sky/ambience per voice.** Reverb at the bus→parent transfer
  (`CAkLEngine::TransferBuffer` → the parent's `ConsumeBuffer(AkAudioBufferBus&)` or
  `CAkVPLFinalMixNode::ConsumeBuffer`; source AkVPL = buffer − 0x420): the only place a reverb *return*
  (post-effect) is a separate signal, with the gain left to the output (`parent.m_fDownstreamGain`, or the
  final mix's `m_fNextVolume` +0x444 for top-level buses); a bus qualifies by its own reverb FX (-6 dB).
  Sky (weather 317282339 + birds 352130103, -3 dB) and ambience (ambient 77978275, -6 dB) can't be bus-level:
  those buses aren't mixing buses and never exist at runtime (see facts), so the voice hook walks the
  sound's bank-side bus chain (nearest listed ancestor wins, cached per sound ID) and carves from the
  voice's `AkAudioMix`: floor gains of FL FR BL BR SL SR scaled by sqrt(1 − s²Σw²), the same share × PCM ×
  downstream into the heights. C/LFE untouched. Map: TFL←FL, TFR←FR, TBL←0.707(SL+BL), TBR likewise;
  stereo buses spread front over both; 2D sky voices use the spread map (left floor → TFL+TBL at -3 dB,
  right likewise). Elevation: bed voices with phi > 0 (max over rays, objects excluded) lift by sin(phi) ×
  `Elevation`, taking the larger of that and the tier share. Research-backed decorrelation (Dolby: beds are for diffuse content,
  PLIIz: rain/wind up; Lee: identical copies must be 7.5-9.5 dB down or the image lifts, nothing < 250 Hz
  localizes overhead; DTS upmix patent: 5-20 ms Haas delay + nested all-passes + LF shelf; Atmos guides:
  front/back heights must differ): 8/12 ms pre-delay, 3 Schroeder all-passes per pair (different sets
  front/back, same L/R), 200 Hz Butterworth HP, run once on the summed accumulators (linear). Height channels
  only when the native static mask has all four; F7 crossfades the carve over one buffer.
- **Actor entities are lifted towards head height** in `AK::SoundEngine::SetPosition` (`ActorLift`): the
  entity is recognized by `SimComponent::m_TypeUID` (+0x18 of the component, entity − 0x40) ==
  `ActorAudioComponent::_TypeUID` 0xD2000003. Done at the game→Wwise boundary rather than in the router so
  the rays come out right by themselves; the bed ignores elevation, so only objects change (plus a few
  percent of distance). Positions only update when a character moved > 0.1 m (`AudioEntityUpdate`), so
  changing the value takes effect as characters move.
- Object position = Wwise's direction on a sphere of `Distance` m (default 2): distance attenuation is already
  in the gain.
- **The player's own voices stay in the bed** (`PlayerInBed`, default on). With the listener at the camera his
  footsteps/foley sit 2.6-4 m ahead, 20-30° below, a few degrees left; as objects they collapse to a precise
  point in front of the viewer (the soundbar can't render "below"), while the bed keeps the game's close-range
  spread (FL/FR + SL/SR), i.e. the original mix. Industry practice agrees: player sounds are listener-relative
  (2D/spread), objects are for things that move around the listener. Identification is by game object, not
  position (see below), so NPCs in melee range are unaffected.
- **Runtime settings are atomics in `gConfig`** (objects on/off, max objects, distance, HUD options): read by the
  audio thread, written by the hotkey thread and the menu. Objects on/off and lowering the limit crossfade the
  affected objects back into the bed first (`gTarget` drops at once, `gBudget` only after the fades). Dynamic
  objects are reserved at stream open whenever the router can run, so switching them on later works.
- **HUD projection assumes the Wwise listener is the camera** (the log shows the player at r≈3.3 m below/in
  front): marker = direction projected with a vertical FOV slider; the radar needs no FOV at all.
- `reshade_overlay` is registered only while the HUD or the A/B banner is visible (it makes ReShade run its
  ImGui pass every frame). Hotkeys are polled with `GetAsyncKeyState` on our own thread (foreground check) so
  they work without ReShade.
- `config::Save` edits the ini byte-wise: `WritePrivateProfileString` treats BOM-less files as ANSI and would
  mangle the UTF-8 comments.
- MinHook is SDmodding's reduced fork (`reference\SPatch\external`): `MH_CreateHook` enables immediately,
  there is no `MH_Initialize`/`MH_EnableHook`.
- The installed exe differs from the legacy one only in rel32/RIP displacement bytes inside these functions;
  data addresses (e.g. `g_pipelineCoreFrequency` RVA 0x20F71F4) are identical.

## Facts established

**User's audio endpoint** (`--probe`): `BD-HTS -2 (HD Audio Driver for Display Audio)`, spatial audio active.
20 dynamic objects, native statics = 7.1.4 (+ StereoLeft/Right), object format float mono 48 kHz, 480 frames
per update (10 ms). Industry numbers agree: 32 objects over HDMI, a 7.1.4 bed takes 12 → 20 dynamic.

**Wwise** is statically linked, version **2012.2** (all `.bnk` in `data\Audio\SD2\*.pck` have BKHD
version 88), 48 kHz. Windows sink is **XAudio2 2.7** (`CoCreateInstance` with the June 2010 SDK CLSID; no
xaudio2 import). `CAkSinkDirectSound` exists but isn't used. With the hook the sink runs 7.1 (0x63F).

**Game audio content** (first in-game log): the game sends nothing to LFE (≈ -110 dBFS all session).
Concurrent voices: up to ~38 dry, ~36 of them 3D, ~30 aux sends in a street fight — more than 20 objects.
Most 3D voices are mono (468 of 544 logged), panner 3D + game-defined positions; many ambience emitters use
full spread (gains 0.41-0.45 on 5-6 speakers).

**Legacy PDB has all Wwise internals** (`reference\SDmodding\game-itself`, IDA MCP). The installed exe is a
near-identical build (same size, `.text` +80 bytes): Wwise functions are found in it by the legacy bytes with
small shifts (e.g. `CAkMixer::Mix3D` -0x20, `CAkSinkXAudio2::PassData` -0x10, `RunVPL`/`ConsumeBuffer` +0x50).

**Runtime buses ≠ bank buses**: `CAkBus::IsMixingBus` (0x140a7c4f0) gives an AkVPL only to buses with FX,
aux buses, own channel config, positioning/HDR (+0x53 bits 2/3) or the master; the rest fold into the nearest
mixing ancestor. Seen at runtime: root, master_music, master_hdr 3995202064 → 2640427754 → final, EQ bus
1667833844, bus 447211353, the reverb aux buses (parent 2640427754), and a bus with ID 0. Bank-side chain in
memory: `CAkParameterNodeBase` +0x38 `m_pParentNode`, +0x40 `m_pBusOutputNode` (`GetControlBus`).

**Bus hierarchy** (`Init.bnk` inside `SFX.pck`, parsed 2026-09-23, tree in `docs\game-audio.md`): 279 buses,
28 aux buses (each with a ConvolutionReverb/MatrixReverb shareset), names = FNV-1 32-bit of the lowercase
name (59 of 307 recovered by dictionary: ambient 77978275 ⊃ weather 317282339 {thunder, wind, rain}, birds
352130103, city 3888786832, traffic, crowd_*, water_amb, boat_amb, interior_rain; master_sfx 3462011115,
master_hdr, master_aux, master_dialog, master_music 1900298039 (the EQ bus), sfx 393239870 {footsteps,
gunshot, fight_*, collisions...}). AKPK v1 bank entries are 24 bytes (id, block, size, pad, offset, lang);
the Python used is in the session scratchpad, not the repo. The user has `reference\wwiseutil-SDDE` (Go)
for the same job once Go is installed.

## How Wwise 2012 renders (legacy addresses)

- Audio thread `CAkAudioThread::EventMgrThreadFunc` (0x140a76d50): loop `CAkAudioMgr::Perform`, then
  `WaitForSingleObject(event, g_pAkSink->GetThreadWaitTime())`. Perform renders as many 1024-frame buffers
  as `g_pAkSink->IsDataNeeded` asks for; each buffer ends with `g_pAkSink->PassData`.
- `CAkSinkXAudio2::Init` (0x140a94a40): speaker config from the XAudio2 device (`1599` = 0x63F = 7.1 when the
  device has >= 7 channels), sample rate `AkAudioLibSettings::g_pipelineCoreFrequency`, interleaved float,
  8-slot ring of 1024-frame buffers. `PassData` copies `m_MasterOut` into the ring;
  `OnVoiceProcessingPassStart` submits to the source voice and wakes the audio thread.
- Per voice: `CAkLEngine::RunVPL(AkRunningVPL&)` (0x140a52c00) pulls source → pitch → filters → LPF, then
  for each aux send and finally for the dry path builds `AkAudioMix mixTemp[channels]` and calls
  `CAkVPLMixBusNode::ConsumeBuffer(bus, AkVPLState&, AkAudioMix*)` (0x140a7e1b0), which zero-pads the voice to
  `uMaxFrames` and calls `CAkMixer::Mix3D`: per input channel, gain ramps linearly from `Previous` (sample 0)
  towards `Next` over 1024 frames. Input channels are planar (`pData + ch * uMaxFrames`), LFE handled last.
- **`AkAudioMix` speaker order is Wwise-internal: FL FR C BL BR SL SR LFE** (not WAVEFORMATEX order). 3D
  panning never uses C.
- Per-voice position: `CAkVPLSrcCbxNodeBase::m_arVolumeData` (cbx+0x28, `AkRayVolumeData[]`) filled by
  `ComputeRay` (0x140a800d0): `theta = atan2(right, front)` (radians, **positive = right**),
  `phi = asin(up / r)` (**positive = up**), from the listener matrix (row 0 right, row 1 up, row 2 front).
  The listener is the camera, so the player's own sounds sit at r≈3.8 m, phi≈-23°.
- `CAkPBI` +0x173 bits 0-1 = panner type (0 = 2D). Layouts in `core/wwise.hh`.
- **Bus chain**: `AkVPL` = `CAkVPLMixBusNode` (: `CAkBusFX` : `CAkBusVolumes`) + downstream gain (+0x550) +
  device ID (+0x558). `CAkBusVolumes::m_pParent` (+0x410) is the parent AkVPL, null at the top, which feeds the
  device's `CAkVPLFinalMixNode` (`AkDevice::pFinalMix`, `CAkOutputMgr::m_Devices` items of 0x50 bytes, ID at
  +0x18). `CAkBusFX::m_aFX[4]` at +0x480 (40 bytes each: plugin ID, +0x10 effect, +0x20 bit 0 bypass),
  `m_bBypassAllFX` +0x520 bit 0. `ProcessAllFX` runs a slot iff effect && !(bypass | bypassAll).
  AkVPLs are created/freed as buses become active, so pointers recycle.
- **Plugins the game registers** (`UFG::WwiseInterface::RegisterPlugins`, index part of the ID):
  Audiokinetic effects ParametricEQ 0x69, Delay 0x6A, Compressor 0x6C, MatrixReverb 0x73,
  SoundSeedImpact 0x74, RoomVerb 0x76, Flanger 0x7D, ConvolutionReverb 0x7F, Meter 0x81, TimeStretch 0x82,
  Tremolo 0x83, PitchShifter 0x88, Harmonizer 0x8A, Gain 0x8B; McDSP (company 0x100) ML1 limiter 0x67,
  FutzBox 0x6E. No Wwise Peak Limiter, so a master limiter would be ML1.

## How the game feeds Wwise (legacy addresses, SDmodding SDK names)

- **Game object ID = `UFG::AudioEntity*`**: `AudioEntity::Init` (0x140146a50) calls `RegisterGameObj(this,
  name, 1)`; the name goes only to the (absent) profiler. `AudioEntity::m_name` (+0x18) is the qSymbol of the
  owning SimObject for actor components. `UFG::ActorAudioComponent` (0x230 bytes; SimComponent 0x40 +
  AudioEntity base at +0x40) plays footsteps, voice and fight impacts on its own entity and allocates a second
  `AudioEntity` (`m_SFXEntity`, +0x198, named "<name>__SFX") in `CheckInitialize` (0x140597c50) when the
  character is within 65 m (300 m for the player, `m_isPlayer` = +0x229 bit 3).
- **Local player**: SimObject named "PlayerOne_Havok", qSymbol 0x90ECB5FF (CRC-32 poly 0x04C11DB7, init -1, no
  final xor; SDK `sim/localplayer.hh`). His sounds come from three kinds of game object: the component's own
  entity (vaults, climbs, voice), the SFX entity (quiet foley on another bus, dryMix 0.63), and **pooled
  `UFG::OneShot` entities** (0x170 bytes, `gOneShotPool`, named "OneShot_%3u" with pool index + 100) that
  `ActorAudioComponent::PlayFootstep` fires through the component's `m_leftFootstep`/`m_rightFootstep`
  handles (+0x1C0/+0x1C8); `OneShot::m_pOwnerHandle` (+0x158) points back at that handle, which is how the
  router attributes them (footsteps at r 2.6-3.1 m, phi -24..-31°, gain 0.249). Other one-shot users
  (`OneShotPool::GetOneShotHandle` xrefs): action-tree `AudioTask`/`AudioTaskSimple::PlayOnOneShot`
  (animation-driven sounds such as the sprint transition steps), `StateMachineComponent`, `DamageRig`,
  gunshots, fight impacts, vehicle impacts, VFX; their handles live in those systems, so the router falls
  back to distance from the player's entity (`AudioEntity::m_WorldMatrix` row 3 at +0x50).
- **Plugin IDs** are `type | company << 4 | index << 16` (company is 12 bits). The one bus effect seen so far
  is 0x690003 = Parametric EQ on a top-level bus.
- **Listener** (`UFG::AudioListener`, singleton `sm_pInstance` RVA 0x2175E30; `Update` at 0x14014d410):
  orientation is always the camera's; position is the camera when `m_positionListenerAtCamera` (+0x81, default
  1) else the local player's transform, both lerped 0.2/frame. Script methods
  `audio_set_listener_at_player/camera` and `audio_lock/unlock_listener_position` flip these, so the game
  already supports the "attenuation from the player, panning from the camera" hybrid (untried from the mod).
  `Audio3DListener::Update` pushes the matrix through `AK::SoundEngine::SetListenerPosition`.

## Plan

1. Standalone test — **passed**.
2. Recon `.asi` — **done**, conventions above.
3. Bed via ISAC — **verified in-game**.
4. Dynamic objects — built, awaiting in-game test. Watch: object loudness vs original (objects skip bus FX,
   e.g. a master limiter or slow-motion filters on buses), audible jumps on promotion/demotion, activation
   failures, whether 20 objects are enough in fights.
5. Player attribution, churn and bus EQ: **verified in-game** (2026-09-23).
6. Height bed — built 2026-09-23, **awaiting the in-game check** (see status). Tuning knobs if it sounds
   wrong: shares (menu sliders), pre-delay/high-pass (ini), the bus lists.
7. Next: verify the actor lift in-game, radar check, offline tests for the router (fake PBI/cbx). Possible
   experiment: flip the game's own `m_positionListenerAtCamera` to hear the listener-at-player hybrid.
8. Later: stereo 3D voices (two objects), multi-position emitters, per-category rules (e.g. always objects for
   gunshots/vehicles by sound ID).

Why not hook `PostEvent`/`SetPosition` as first planned: those give IDs and positions but no audio samples.
The PCM only exists inside the Wwise pipeline, and Wwise already has the listener-relative direction there.
