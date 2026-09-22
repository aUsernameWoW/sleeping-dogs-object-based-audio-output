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
- Milestone 4 (dynamic objects, voice router) built and deployed, **not yet tested in-game**.

## Files

- `dllmain.cc` — config, log, `wwise::Install()` from DllMain (before the game initializes Wwise).
- `core/wwise.hh` — Wwise 2012.2 struct layouts/offsets from the legacy PDB.
- `core/wwise_hooks.*` — signatures + hooks: `CAkSinkXAudio2::Init/PassData/PassSilence`,
  `CAkLEngine::RunVPL`, `CAkVPLMixBusNode::ConsumeBuffer`; voice snapshot logging.
- `core/objects.*` — voice router: which voices become objects, sample capture, bed/object crossfades.
- `core/spatial_out.*` — ISAC stream (bed + dynamic objects), SPSC ring with per-block object metadata,
  render thread, object activation/reuse/release, fold-into-bed fallback, reopen on device loss.
- `core/scan.*` — unique pattern search in the exe's `.text`, RIP-relative decoding.
- `core/config.*`, `core/log.*` — `SDAtmos.ini` / `SDAtmos.log`.
- `tests/load_test.cc` (automated: loads into a Wwise-less process), `tests/spatial_orbit_manual.cc`
  (standalone ISAC check: `--probe` prints limits, otherwise plays a circling object).

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
- **Objects never get dropped**: if Windows refuses a dynamic object, the render thread pans that slot into the
  bed (constant power between adjacent bed speakers).
- Object position = Wwise's direction on a sphere of `Distance` m (default 2): distance attenuation is already
  in the gain.
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

## Plan

1. Standalone test — **passed**.
2. Recon `.asi` — **done**, conventions above.
3. Bed via ISAC — **verified in-game**.
4. Dynamic objects — built, awaiting in-game test. Watch: object loudness vs original (objects skip bus FX,
   e.g. a master limiter or slow-motion filters on buses), audible jumps on promotion/demotion, activation
   failures, whether 20 objects are enough in fights.
5. Later: stereo 3D voices (two objects), multi-position emitters, per-category rules (e.g. always objects for
   gunshots/vehicles by sound ID), maybe a ReShade overlay showing objects.

Why not hook `PostEvent`/`SetPosition` as first planned: those give IDs and positions but no audio samples.
The PCM only exists inside the Wwise pipeline, and Wwise already has the listener-relative direction there.
