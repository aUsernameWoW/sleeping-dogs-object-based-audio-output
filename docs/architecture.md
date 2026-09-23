# Architecture

## The problem

Sleeping Dogs: Definitive Edition renders its audio with Wwise 2012.2 into a 7.1 channel mix and plays it
through XAudio2 2.7. Over HDMI that reaches a soundbar/receiver as 7.1 PCM (shown as "Dolby Audio" on the
user's device). Windows' "Dolby Atmos for home theater" does not upmix such streams; only applications that
render through `ISpatialAudioClient` (ISAC) are encoded as Dolby MAT with object metadata, which is what
makes the device switch to "Dolby Atmos" and render height and precise positions.

SDAtmos is an `.asi` plugin (loaded by Ultimate ASI Loader from `plugins\`) that

1. takes Wwise's final 7.1 mix before XAudio2 sees it and plays it through ISAC as a **static bed**
   (7 speaker objects + LFE), and
2. takes selected 3D voices out of that mix, before they are panned into speakers, and plays them as
   **dynamic objects** with their listener-relative direction.

Nothing is Dolby-specific: object counts, the native bed layout and the object format are queried at runtime,
so DTS:X for Home Theater or Windows Sonic should work the same way (untested; the user has Atmos).

## Data flow

```
 Wwise audio thread (game)                                   SDAtmos render thread
 ─────────────────────────                                   ─────────────────────
 CAkLEngine::RunVPL(voice)          ┐
   ConsumeBuffer(bus, state, mix) ──┼─ hook: voice router (objects.cc)
     • decides bed / object         │    • copies the voice's pre-pan PCM × gain ramp into an object slot
     • scales `mix` for the bed     │    • runs the bus chain's Parametric EQ on it
     • telemetry for the HUD        ┘
 ... all voices, all buses ...
 CAkSinkXAudio2::PassData(sink) ──── hook: FinishBuffer
     • objects::FinishFrame → block   • copies m_MasterOut (7.1 interleaved) + object samples + positions
       of objects for this buffer       into the SPSC ring (spatial::Push)
     • zeroes m_MasterOut so XAudio2  • telemetry::Publish
       plays silence
 XAudio2 keeps consuming (silent) ────────────── same endpoint clock ──────────────► ISAC event every 10 ms
                                                                                     • bed objects ← ring
                                                                                     • dynamic objects ← ring
                                                                                       (activate / reuse / release)
                                                                                     • fold into bed if Windows
                                                                                       refuses an object
```

Other threads:

- **Hotkey thread** (`overlay.cc`): polls `GetAsyncKeyState` every 25 ms while the game is in the foreground.
  F9 toggles dynamic objects (A/B against the plain bed), F8 the HUD. Polled on our own thread because the
  game reads keyboard through Raw Input and the hotkeys must work without ReShade.
- **ReShade render thread** (`overlay.cc`): if ReShade 6.8.0 with add-on support is present, SDAtmos
  registers as an add-on and draws a menu tab (settings, stream status, voice table, save to ini) and a HUD
  (radar + on-screen markers + A/B banner) with Dear ImGui.
- **Probe thread** (`spatial_out.cc`): `spatial::IsAvailable()` checks for ISAC on the default endpoint from
  a throwaway MTA thread, because it is called from a game thread whose COM state we don't own.

## Files

| File | Role |
|---|---|
| `dllmain.cc` | Loads `SDAtmos.ini`, opens the log, installs the Wwise hooks and the overlay from `DllMain`. Runs before the game's `main` (the ASI loader is reached through `dinput8.dll`, a static import), so the sink hook is in place before Wwise initializes. |
| `core/wwise.hh` | Wwise 2012.2 struct layouts and offsets (from the legacy PDB). |
| `core/game.hh` | Game-side (UFG) layouts: `AudioEntity`, `ActorAudioComponent`, `OneShot`, the player's name hash, `qSymbol` CRC. |
| `core/wwise_hooks.*` | Byte signatures, MinHook hooks (`CAkSinkXAudio2::Init/PassData/PassSilence`, `CAkLEngine::RunVPL`, `CAkVPLMixBusNode::ConsumeBuffer`, `AK::SoundEngine::SetPosition` for the actor lift), voice snapshot logging, `CAkOutputMgr::m_Devices` lookup, plugin names. |
| `core/objects.*` | The voice router: candidate rules, slot assignment, ranking, crossfades, player attribution, bus effect check, Parametric EQ reproduction. |
| `core/spatial_out.*` | The ISAC stream: bed + dynamic objects, SPSC ring with per-block object metadata, render thread, activation/reuse/release, fold-into-bed, reopen on device loss. |
| `core/telemetry.*` | Per-buffer voice snapshot handed from the audio thread to the render thread (try-lock; the audio thread never waits). |
| `core/overlay.*` | Hotkeys, ReShade add-on (menu + HUD). |
| `core/scan.*` | Unique IDA-style pattern search in the exe's `.text`, RIP-relative operand decoding. |
| `core/config.*`, `core/log.*` | `SDAtmos.ini` (written with bilingual comments if missing, saved byte-wise so the UTF-8 comments survive) and `SDAtmos.log`. |
| `tests/` | `load_test.cc` (loads the .asi into a Wwise-less process), `config_save_test.cc`, `spatial_orbit_manual.cc` (standalone ISAC check). |

## Design decisions and their reasons

**XAudio2 stays the clock.** Wwise renders a buffer whenever XAudio2 frees a queue slot
(`CAkSinkXAudio2::IsDataNeeded`). We do not replace the sink; the hook copies each buffer into our ring and
zeroes it, so XAudio2 keeps playing silence at the endpoint's rate. Since ISAC renders on the same endpoint,
the ring fill only jitters and never drifts, and Wwise's pacing logic is untouched. If the ISAC stream cannot
open or dies, `spatial::IsActive()` is false, the hook leaves the buffer alone and the game is audible through
XAudio2 again (automatic fallback, and automatic recovery when the stream reopens).

**Copy/zero `m_MasterOut` before the original `PassData`, not after.** XAudio2 reads its ring asynchronously
once `m_uNbBuffersRB` is incremented inside `PassData`; zeroing afterwards would race with it.

**Force a 7.1 layout** (`Init` channel mask 0 → 0x63F) only when the default endpoint offers ISAC. Wwise
otherwise picks its layout from XAudio2's device format, which need not be 7.1 even when the spatial format
has a 7.1.4 bed. Without spatial audio Wwise keeps the device's layout.

**Objects are carved out of the voice's dry mix, not re-rendered.** The original `ConsumeBuffer` is always
called; for an object voice its `AkAudioMix` gains are scaled to 0 (or to the bed's share during a crossfade),
so bus state, ducking, metering and effect setup stay normal. The object gets the voice's pre-pan PCM × the
same Previous→Next gain ramp `CAkMixer::Mix3D` would apply × `AkVPL::m_fDownstreamGain` (the product of bus
volumes up the chain, recomputed every frame by `CAkLEngine::AnalyzeMixingGraph`). Aux sends (reverb) are
untouched and stay in the bed, which is what Dolby's guidance recommends anyway.

**Ring latency.** Prime at 2048 frames; trim when the slack stays above 3072 frames for 2 s. XAudio2's deeper
queue hides Wwise stalls from the original path but not from ours, hence the generous slack. Measured
latency is about 45 ms of ring fill.

**Objects never get dropped.** If Windows refuses a dynamic object (`ActivateSpatialAudioObject` fails, e.g.
another app holds them), the render thread pans that slot into the bed with constant-power gains between the
two adjacent horizontal speakers, and retries once the slot goes idle.

**Objects are reserved at stream open whenever the router can run**, even if they start switched off, so the
F9 A/B switch works without reopening the stream.

**Runtime settings are atomics in `gConfig`**, read by the audio thread and written by the hotkey thread and
the menu. Switching objects off or lowering the limit crossfades the affected objects back into the bed
first (`gTarget` drops at once, `gBudget` only after the fades).

**The HUD projection assumes the Wwise listener is the camera.** Verified: with a 60° vertical FOV the
markers sit on their sources. The radar needs no FOV at all.

**`reshade_overlay` is registered only while the HUD or the A/B banner is visible**, because a registered
overlay makes ReShade run its ImGui pass every frame.

**MinHook is SDmodding's reduced fork** (`reference\SPatch\external`): `MH_CreateHook` enables the hook
immediately, there is no `MH_Initialize`/`MH_EnableHook`. Fine here since we hook from `DllMain` before any
game thread exists.

**Characters' sounds are lifted off the ground at the game→Wwise boundary.** Their audio entities follow
the character root (feet), which the 7.1 bed never revealed (no height) but objects do. A hook on
`AK::SoundEngine::SetPosition` adds `ActorLift` meters (default 1.5) to actor audio components' positions,
so Wwise's own rays come out at head height and the router needs no listener math; the bed is unchanged
apart from a slightly larger distance. The game's occlusion, distance RTPC and region logic use its own copy
of the position and are unaffected.

Router-specific decisions (player attribution, decay handling, bus effects) are in
[voice-router.md](voice-router.md).

## Configuration

`SDAtmos.ini` next to the `.asi` (a commented default is written if missing; the user's file is never
overwritten by deploys):

| Section / key | Default | Meaning |
|---|---|---|
| `General.SpatialBed` | 1 | Route the 7.1 mix through ISAC. Off = the mod only logs. |
| `Objects.Enabled` | 1 | Dynamic objects on/off (F9). |
| `Objects.MaxObjects` | 20 | Upper bound; the format may allow fewer (Atmos over HDMI: 20). |
| `Objects.Distance` | 2.0 | Radius (m) objects are placed at; Wwise already applied distance attenuation, only the direction is new information. |
| `Objects.PlayerInBed` | 1 | The player's own sounds stay in the bed. |
| `Objects.BusFx` | 1 | Effects other than Parametric EQ on the bus chain: 0 ignore, 1 voice stays in the bed (master excepted), 2 master included. |
| `Objects.ActorLift` | 1.5 | Meters added to characters' audio entity positions (their root is at the feet) before Wwise sees them; 0 = off. |
| `Overlay.*` | | HUD on/off (F8), radar, markers, labels, bed voices, marker FOV, radar range, hotkey codes. |
| `Debug.Logging` | 1 | Write `SDAtmos.log`. |
| `Debug.VoiceLog` | 1 | Periodic 3D voice snapshots in the log. |
