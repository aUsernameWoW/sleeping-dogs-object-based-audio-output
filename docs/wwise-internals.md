# Wwise 2012.2 inside sdhdship.exe

Wwise is statically linked into the game executable. Version: **2012.2** (every `.bnk` inside
`data\Audio\SD2\*.pck` has BKHD version 88), engine rate 48 kHz, buffers of 1024 frames
(`AK_NUM_VOICE_REFILL_FRAMES`). The Windows sink is **XAudio2 2.7** (created with `CoCreateInstance` and the
June 2010 DirectX SDK CLSID; there is no `xaudio2` import). A `CAkSinkDirectSound` exists in the binary but
is not used.

Symbol names below come from the legacy PDB (`reference\SDmodding\game-itself\sdhdship.pdb`); addresses are
legacy-build virtual addresses. See [reverse-engineering.md](reverse-engineering.md) for how they map to the
installed build.

## Render loop

- Audio thread: `CAkAudioThread::EventMgrThreadFunc` (0x140a76d50) loops `CAkAudioMgr::Perform`, then
  `WaitForSingleObject(event, g_pAkSink->GetThreadWaitTime())`.
- `Perform` renders as many 1024-frame buffers as `g_pAkSink->IsDataNeeded()` asks for. Each buffer ends
  with `g_pAkSink->PassData()` (or `PassSilence()` when nothing plays).
- Before rendering, `CAkLEngine::AnalyzeMixingGraph` (0x140a50a30) computes each voice's rays
  (`ComputeVolumeRays`) and each bus's `m_fDownstreamGain` = its own volume × the parent's downstream gain,
  or × the device's final-mix volume for top-level buses.

## Sink (`CAkSinkXAudio2`)

- `Init` (0x140a94a40): speaker config from the XAudio2 device (`1599` = 0x63F = 7.1 when the device reports
  ≥ 7 channels), sample rate from `AkAudioLibSettings::g_pipelineCoreFrequency` (a global; RVA 0x20F71F4 in
  both builds), interleaved float, an 8-slot ring of 1024-frame buffers.
- `PassData` copies `m_MasterOut` into the ring and increments `m_uNbBuffersRB`;
  `OnVoiceProcessingPassStart` (XAudio2 callback) submits the next buffer to the source voice and wakes the
  audio thread.
- Layout: `+0x10` speaker config (uint32), `+0x18` `m_MasterOut` (`AkAudioBuffer`: pData, channel mask, state,
  uMaxFrames, uValidFrames), `+0x30` sink type, `+0x50` channel count.
- `PassData` clamps samples to ±1 before handing them to XAudio2; the mod clamps the same way.

## Voice pipeline

Per voice, `CAkLEngine::RunVPL(AkRunningVPL&)` (0x140a52c00) pulls source → pitch → filters → LPF, then for
each aux send and finally for the dry path builds `AkAudioMix mixTemp[channels]` and calls
`CAkVPLMixBusNode::ConsumeBuffer(bus, AkVPLState&, AkAudioMix*)` (0x140a7e1b0). That function zero-pads the
voice to `uMaxFrames` and calls `CAkMixer::Mix3D`: per input channel, gain ramps linearly from `Previous`
(sample 0) towards `Next` over the 1024 frames. Input channels are planar (`pData + ch * uMaxFrames`), LFE
handled last.

Structures (see `core/wwise.hh`):

- `AkRunningVPL` = `AkVPLState` (0x40 bytes: `AkAudioBuffer` + pipeline info + result/pause/stop/aux/audible
  flags) + `CAkVPLSrcCbxNode* pCbx` (+0x40) + `pFeedbackData` + `bFeedbackVPL` (+0x50; feedback = rumble, not
  audio).
- `AkAudioMix` = `float next[8]; float previous[8];` **Speaker order is Wwise-internal: FL FR C BL BR SL SR
  LFE**, not WAVEFORMATEX order. Established from the first in-game log: θ = -135° lands on index 3,
  +134° on 4, ±90° on 5/6. 3D panning never uses the center channel.
- `CAkVPLSrcCbxNode(Base)`: `+0x10` sources (`CAkVPLSrcNode*[2]`), `+0x28` `m_arVolumeData`
  (`AkArray<AkRayVolumeData>`: items pointer then uint32 length), `+0x50` device list (`AkDeviceInfo*` then
  count), `+0x168` sample rate.
- `CAkVPLSrcNode`: `+0x18` context = `CAkPBI*`.
- `AkDeviceInfo` (one per output device a voice feeds): `+0x248` next, `+0x250` the voice's dry output bus
  (`AkVPL*`), `+0x264` cross-device send flag. The hook identifies "this ConsumeBuffer is the dry path" by
  matching the bus against this list; every other call from inside `RunVPL` is an aux send.
- `CAkPBI` (one playing sound instance): `+0x70` playing ID, `+0x98` `CAkSoundBase*` (a `CAkIndexable`, ID at
  `+0x10`), `+0xA8` `CAkRegisteredObj*` (game object; its `AkGameObjectID` uint64 at `+0x70`), `+0x173`
  bits 0-1 panner type (0 = 2D, else 3D), bits 2-3 position source type.
- `AkRayVolumeData` (36 bytes): `r`, `theta`, `phi`, emitter angle, flags, listener mask, dry mix gain,
  game-defined aux gain, user-defined aux gain, cone interpolation.

### Positions

`CAkVPLSrcCbxNodeBase::ComputeRay` (0x140a800d0) fills the rays from the listener matrix (row 0 right, row 1
up, row 2 front): `theta = atan2(right, front)` in radians, **positive = right**; `phi = asin(up / r)`,
**positive = up**. Multi-position emitters get one ray per position. Distance attenuation, cone and spread
are already folded into the `AkAudioMix` gains; `r`/`theta`/`phi` are the raw geometry.

### Buses

- `AkVPL` (1392 bytes) is a mix bus instance. It begins with `CAkVPLMixBusNode` (: `CAkBusFX` :
  `CAkBusVolumes`), so the node pointer passed to `ConsumeBuffer` is also the `AkVPL` pointer. Then
  `+0x550` `m_fDownstreamGain`, `+0x558` device ID, `+0x560` flags (bit 1 = HDR bus).
- `CAkBusVolumes`: `+0x0` panning volumes, `+0x200` final volumes (`AkAudioMix[8]` each), `+0x400` channel
  mask, `+0x408` bus ID, **`+0x410` `m_pParent` (`AkVPL*`)**, `+0x444` next volume, `+0x448` previous volume.
  `m_pParent == nullptr` means the bus feeds the device's final mix.
- `CAkBusFX`: **`+0x480` `m_aFX[4]`** (40-byte slots: `+0x0` plugin ID, `+0x8` params, `+0x10` effect
  instance, `+0x18` bus FX context, `+0x20` bit 0 bypass / bit 1 last bypass), `+0x520` bit 0 bypass-all.
  `CAkVPLMixBusNode::ProcessAllFX` (0x140a7e930) runs a slot iff it has an effect and `!(bypass | bypassAll)`;
  a slot that just became bypassed gets `Reset()` instead.
- `CAkVPLFinalMixNode` (the master bus of a device) is a `CAkBusFX` too, with the same offsets, but not an
  `AkVPL`. Found through `CAkOutputMgr::m_Devices` (an `AkArray<AkDevice>`: items pointer, uint32 length;
  `AkDevice` is 0x50 bytes: `+0x0` final mix node, `+0x8` sink, `+0x18` device ID).
- AkVPLs are created and freed as buses become active, so bus pointers recycle.
- HDR: `AkVPL` flag bit 1; `AkHdrBus::ComputeHdrAttenuation` adjusts voice volumes, which end up in the mix
  gains, so objects inherit it.

### Effects

Plugin IDs are `AKMAKECLASSID(type, company, index) = type | company << 4 | index << 16` (type 3 = effect,
company 0 = Audiokinetic, 12-bit company field). The game registers (`UFG::WwiseInterface::RegisterPlugins`,
0x14014b070): ParametricEQ 0x69, Delay 0x6A, Compressor 0x6C, MatrixReverb 0x73, SoundSeedImpact 0x74,
RoomVerb 0x76, Flanger 0x7D, ConvolutionReverb 0x7F, Meter 0x81, TimeStretch 0x82, Tremolo 0x83, PitchShifter
0x88, Harmonizer 0x8A, Gain 0x8B; McDSP (company 0x100) ML1 limiter 0x67, FutzBox 0x6E; sources Silence,
Tone, SoundSeed Wind/Woosh, MP3, a custom GrainPlayer (company 0x40); codec Vorbis. There is no Wwise Peak
Limiter, so a master limiter would be ML1. In-game only one kind of bus insert has been seen so far: a Parametric EQ
on a top-level bus (bus ID 1900298039 in one session, 1667833844 in another; bus IDs are bank-defined, so
these are probably two different buses of the same hierarchy that happened to be active), plus a Meter on
another bus.

`CAkParametricEQFX` (104 bytes): `+0x8` `m_pfFiltCoefs[3][5]`, `+0x48` `m_pSharedParams`, `+0x54` sample
rate, `+0x60` current output gain. `CAkParameterEQFXParams`: `+0x8` `EQModuleParams Band[3]` (20 bytes each:
filter type, gain dB, frequency, Q, on/off at `+0x10`), `+0x44` output level dB, `+0x4C` `m_bBandDirty[3]`.
`Execute` recomputes dirty bands (`ComputeBiquadCoefs`, standard RBJ-style formulas per filter type), runs
each enabled band through `Process` as `y = c0·x + c1·x1 + c2·x2 + c3·y1 + c4·y2` (c3/c4 are already
`-a1/a0`, `-a2/a0`), then ramps the output gain to `10^(level/20)`. The mod reads the coefficients from the
instance rather than recomputing them.

## Game objects and listeners

- `AK::SoundEngine::RegisterGameObj(id, name, listenerMask)` (0x140a42ce0) is called from exactly one place,
  `UFG::AudioEntity::Init`, with `id = this`. The name only feeds the profiler (absent in shipping builds).
- `AK::SoundEngine::SetListenerPosition` (0x140a43130) is called from `UFG::Audio3DListener::Update` with a
  matrix built from the game's `AudioListener` (see [game-audio.md](game-audio.md)).
- `AK::SoundEngine::SetPosition` (0x140a432e0) is called from `AudioEntity::ForcePositionUpdate` and
  `AudioEntity::SetShouldFollowListener`.
