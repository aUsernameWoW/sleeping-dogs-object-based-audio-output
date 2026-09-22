# Sleeping Dogs: Definitive Edition — object-based audio output (SDAtmos)

让《热血无赖：终极版》（2014）通过 Windows 空间音频输出真正的对象音频：回音壁/功放显示 Dolby Atmos，而不是
Dolby Audio。

Sleeping Dogs mixes its audio with Wwise 2012 into a 7.1 channel stream. Windows' Dolby Atmos for home
theater doesn't upmix: channel streams go out as Dolby Audio (DD/DD+), and only apps that use
`ISpatialAudioClient` produce Atmos. This mod hooks the game's Wwise engine and plays the game through
`ISpatialAudioClient` instead:

- **7.1 bed**: the game's final mix goes out as static bed objects. The receiver switches to Atmos and the
  mix sounds as before.
- **Dynamic objects**: each Wwise frame, the loudest point-like 3D sounds (gunshots, footsteps, vehicles,
  voices) come out of the bed. Each one's pre-panning signal is sent as a dynamic object, positioned in the
  direction Wwise computed for it, height included. The 7.1 panner used to flatten that height away.
  Diffuse sounds (ambience, reverb, sounds with wide spread) stay in the bed. When there are more sounds than
  the format allows objects (20 over HDMI), the extra ones fold into the bed, as Dolby's game guidelines
  recommend.
- **Nothing Dolby-specific**: object limits, bed layout and format are queried at runtime, so DTS:X for Home
  Theater and Windows Sonic go through the same path (only Dolby Atmos for home theater has been tested).

Status: **experimental**. It works in-game on the author's setup. The listening tests are still under way.

## How it works

The installed Steam exe has no symbols. The Wwise functions are found by byte signatures, which were
generated from the legacy v1.0 build and its PDB (from the SDmodding project). Wwise is statically linked
and identical in both builds. The hooks:

- `CAkSinkXAudio2::Init/PassData/PassSilence`: take the final mix before XAudio2 sees it. XAudio2 keeps
  running silently as the clock that paces Wwise. If spatial audio isn't available, the game stays on XAudio2
  unchanged.
- `CAkLEngine::RunVPL` + `CAkVPLMixBusNode::ConsumeBuffer`: the point where each voice is mixed into its
  bus. For voices that become objects, the mod takes the voice's PCM with the same gain ramp Wwise would
  apply (including bus volumes). It then scales Wwise's own mix matrix down, so the bed gets only what isn't
  an object. Moves between bed and object crossfade over one 21 ms buffer.

See `CLAUDE.md` for the full design notes and the Wwise internals involved.

## Requirements

- Sleeping Dogs: Definitive Edition (Steam, current build), Windows 10/11 x64.
- A spatial sound format enabled for the output device (Windows Sound settings → Spatial sound): Dolby Atmos
  for home theater (Dolby Access app) for an HDMI receiver/soundbar, or DTS:X / Windows Sonic.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (e.g. as `dinput8.dll`).
- Optional: [ReShade](https://reshade.me) 6.8.0 with add-on support, for the in-game menu and HUD.

## Install

Copy `SDAtmos.asi` into the game's `plugins\` folder. On first start it writes a commented `SDAtmos.ini`
next to itself (bilingual, 中文/English) and logs to `SDAtmos.log`.

In game:

- **F9**: dynamic objects on/off. This is an A/B switch against the plain 7.1 bed.
- **F8**: HUD (needs ReShade). It shows a radar of every positioned sound and markers at their on-screen
  directions. Cyan = object, yellow = qualifies but waiting, gray = stays in the bed.
- ReShade menu → **SDAtmos** tab: stream status, live settings, voice list, save to ini.

## Building

Visual Studio 2022 (v143), Windows SDK 10.0.26100. The project expects to sit at `mods\SDAtmos` in a
workspace that also has:

- `reference\reshade`: ReShade v6.8.0 source, with the `deps\imgui` submodule initialized.
- `reference\SPatch\external`: `MinHook.h` / `MinHook.lib` from SDmodding's MinHook build.

## Credits

- [SDmodding](https://github.com/SDmodding): the legacy build's PDB and SDK, and the MinHook build.
- [ReShade](https://github.com/crosire/reshade) add-on API and Dear ImGui.

Not affiliated with Square Enix, United Front Games, Audiokinetic, Dolby, DTS or Microsoft.
