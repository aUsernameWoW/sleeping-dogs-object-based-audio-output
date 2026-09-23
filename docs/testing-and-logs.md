# Testing and reading the logs

## Build, test, deploy

From the workspace root (the project references `..\..\reference\...`, so it only builds inside the
workspace):

```powershell
.\tools\build.ps1 -Mod SDAtmos            # Release x64 → mods\SDAtmos\build\Release\SDAtmos.asi
.\tools\build.ps1 -Mod SDAtmos -Test      # + compiles tests\*.cc, runs tests\*_test.cc (exit 0 = pass)
.\tools\build.ps1 -Mod SDAtmos -Deploy    # + copies the .asi into the game's plugins\ (refuses while the game runs)
```

Toolchain: VS 2022 (v143), Windows SDK 10.0.26100, C++20, `/utf-8`, static CRT, warning level 4 (the build
is warning-free; keep it so). Tests are standalone programs that get the `.asi` path in `argv[1]` and run
next to a private copy so their ini/log land in `build\tests\<name>\`.

- `load_test`: loads the .asi into a process without Wwise. Must not crash, must write a default ini, must
  log the missing sink functions and leave audio alone.
- `config_save_test`: `config::Save` keeps UTF-8 comments and unrelated keys, adds missing keys.
- `spatial_orbit_manual`: compiled, not run; see spatial-output.md.

The router has no offline tests yet; its inputs are plain memory with known offsets, so fake `CAkPBI`/cbx
blocks would make one feasible.

CI: `.github/workflows/build.yml` runs the same build and tests on GitHub Actions for pushes to main and for
pull requests (changes that only touch Markdown or `docs\` are skipped). The runner image is
`windows-2025-vs2026`: Visual Studio 2026 with the v143 toolset (MSVC 14.44) installed alongside, the same
compiler as a local VS 2022 17.14. The job recreates the workspace layout from pinned commits (ReShade v6.8.0
with its `deps/imgui` submodule, SPatch for `external\MinHook.*`), builds with `-warnAsError` so a new
warning fails the run, and uploads `SDAtmos.asi` + `SDAtmos.pdb` as the run's artifact. When something the
project uses in `reference\` moves, bump the matching pin in the workflow. Actions are pinned by commit SHA;
Dependabot (`.github/dependabot.yml`) proposes updates monthly, a week after each release.

## In-game procedure

1. Deploy, start the game, open `plugins\SDAtmos.log`. Expect, in order: `scan:` lines for each function,
   `hook: sink hooks ready`, `hook: voice hooks ready`, `sink: spatial audio available`, `sink:
   CAkSinkXAudio2::Init(... mask 0x0 -> 0x63F ...)`, `spatial: stream started on <endpoint>: bed [...] (8
   ch), 48000 Hz, N dynamic objects`.
2. The soundbar/receiver should now show the spatial format's name (Dolby Atmos) instead of Dolby Audio.
3. **F8** shows the HUD: radar (log-distance rings, a dot per positioned voice, stick up/down = elevation),
   on-screen markers at each voice's projected direction, a status line (objects on/off, N/limit, voice
   counts, latency). Colors: cyan = object (label = slot number), yellow = qualifies but waiting, gray =
   spread/quiet (bed), purple = not mono / multi-position, green = player (bed), orange = bus effects (bed).
4. **F9** toggles objects (A/B). A banner confirms it even with the HUD off.
5. ReShade menu → SDAtmos tab: stream status, live sliders/checkboxes, the voice table (role, azimuth,
   elevation, distance, level, sound ID, slot), "Save to SDAtmos.ini".
6. Walk, sprint (L-Shift), vault over things, start a fight, drive: the player's sounds should be green
   throughout, fights should push objects toward the limit, driving should show the car's sounds behind the
   camera.

## Log lines

`HH:MM:SS.mmm [thread] message`. Threads: the loader thread (scan/hook lines), the Wwise audio thread
(`voices:`, `objects:`, `sink:` lines), the render thread (`spatial:`), the hotkey thread.

- `voices: snapshot of frame N` then up to 16 `voice` lines (3D first), every 5 s when `VoiceLog` is on:
  `voice <role> snd=<soundID> obj=<gameObjectID hex> pan=<panner> pos=<posSource> ch=<channels> rays=<n>
  r=<m> theta=<deg> phi=<deg> dryMix=<g> | gain=<total> down=<downstream> rms=<rms> | FL .. FR .. C .. BL
  .. BR .. SL .. SR .. LFE ..` — `role` is `objN` or `bed`; the eight gains are Wwise's own Next gains,
  before the router touched them. `obj=` is a `UFG::AudioEntity` address (one-shots are 0x170 apart).
- `voices: frame N: D dry (T 3D), A aux sends; max over last period ...`
- `objects: target T, budget B, slots in use S; since last: I instant, P promoted; demoted O outranked, D
  disqualified (spread/player/bus fx), U unrenderable, X switched off; E ended as objects; max V voices
  tracked, max C candidates` — the churn line. Healthy fight: instant 30-60, promoted < 20, demoted single
  digits.
- `objects: player audio component <ptr> (entity <ptr>), SFX entity <ptr>` — the player was identified;
  appears again after loads.
- `objects: bus <ptr> (id <busID>, parent <ptr>) fx: <name> (0x<id>) [applied to objects|ignored]` — a bus
  with insert effects, once per bus and change.
- `spatial: N passes, underruns U (F frames), trimmed T, overflow O, fill min..max, bed peak dBFS: ...` every
  10 s. Expect underruns 0, overflow 0, fill ~1400..2400.
- `spatial: objects: avg A max M sounding, max H held, X activations, R released, F failed, P slot-passes
  folded into bed, peak dBFS` — `failed`/`folded` > 0 means Windows ran out of objects (another app?).
- `spatial: stream stopped (0x...)`, then `stream started` again when it reopened (device change, spatial
  format toggled).
- `hotkey: dynamic objects ON/OFF`, `hotkey: HUD on/off`, `menu: settings saved`.

## Baselines (user's system, 2026-09-23)

- Bed through ISAC: zero underruns/overflows over minutes, fill 1408..2400, latency ~45 ms.
- Fights: 5-11 objects on average, peaks at the 20 limit, zero activation failures.
- Churn after the decay change: 0-4 demotions per 5 s (was ~47).
- Bus effects: one top-level Parametric EQ, one Meter, none on the master.
- Listening: with objects on, the mix is clearly directional even from an off-center seat; the player's
  own sounds are enveloping rather than a point ahead.

## Troubleshooting

| Symptom | Look at |
|---|---|
| Receiver stays on Dolby Audio | `sink: spatial audio not available` → enable a spatial format on the endpoint; `spatial: ... has no spatial audio` → same; `stream started` missing → the HRESULT in the log. |
| Game silent | `spatial: stream stopped` without a restart: the render thread died; XAudio2 should be audible again unless `IsActive` stayed true (bug). |
| Crackle | `underruns` or `overflow` > 0 in the spatial stats; check ring constants and whether the game stalls (loading). |
| No objects | HUD says `objects ON 0/20`: `hook: voice hooks MISSING` (signature broke with a game update), or `BusFx` policy sending everything to the bed (check `bus ... fx:` lines), or `MaxObjects` 0. |
| Wrong colors on Wei | no `player audio component` line → the player's name hash or layouts changed; see game-audio.md. |
| Markers don't sit on sources | adjust the marker FOV slider; the projection assumes the listener is the camera. |
| Signature `N matches` after a game update | regenerate from the legacy bytes with more/other context, verify uniqueness offline, keep the RIP decode offsets in sync. |
