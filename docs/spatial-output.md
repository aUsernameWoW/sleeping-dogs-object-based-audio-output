# Windows spatial audio output

## ISpatialAudioClient facts

- The API is format-agnostic: the user picks Dolby Atmos for home theater / DTS:X / Windows Sonic in the
  Windows sound settings; the app activates `ISpatialAudioClient` on the endpoint and queries what it gets.
  Without a spatial format enabled, activation fails (or `GetMaxDynamicObjectCount` returns 0).
- Two kinds of audio objects: **static** ones bound to a channel of the bed (up to 8.1.4.4: 8 around, LFE,
  4 above, 4 below, plus stereo left/right) and **dynamic** ones with an arbitrary position that may change
  per update.
- Coordinates: meters, **+x right, +y up, +z behind** the listener (Windows convention). The mod converts
  Wwise's spherical direction with `x = d·sinθ·cosφ`, `y = d·sinφ`, `z = -d·cosθ·cosφ`.
- Limits on the user's setup (`spatial_orbit_manual --probe` and the stream open log): endpoint
  `BD-HTS -2 (HD Audio Driver for Display Audio)`, Dolby Atmos for home theater, **32 objects total, 20 dynamic**
  when a 7.1.4 bed takes 12, native statics = 7.1.4 + StereoL/R, object format float mono 48 kHz, **480
  frames per update (10 ms)**. Microsoft's documentation agrees (Atmos over HDMI: 12 static + 20 dynamic;
  DTS:X HDMI: 17 + 32 on current Windows; headphone formats: up to 128).
- Home-theater renderers don't render distance in a meaningful way and can't render "below the listener"
  (no floor speakers); objects below the horizon collapse onto the horizontal plane. Height works.
- Windows applies the system mixing policy: spatial streams mix with non-spatial apps.

## The mod's stream (`core/spatial_out.cc`)

Opening (`Open`): default render endpoint → `ISpatialAudioClient` → query max dynamic objects and native
static mask → check that mono float at Wwise's rate is a supported object format (no resampling is done) →
`ActivateSpatialAudioStream` with the bed's static mask, `MaxDynamicObjectCount` = min(format max, 32) when
objects are wanted, category `AudioCategory_GameEffects`, an event handle → activate one static object per
bed channel → `Start`.

Bed channel mapping (`BuildChannelMap`): SPEAKER_* bits of Wwise's interleaved layout → `AudioObjectType`.
A 5.1 mix puts its surrounds on the BACK bits, which the Atmos bed calls Side; with true side channels
present, back stays back. Non-native static types are allowed (Windows folds them) but logged. The four
height channels (`TopFrontLeft/Right`, `TopBackLeft/Right`) are added after the floor channels only when
the format's native static mask has all four (a folded-down height would just be a delayed copy of the
floor); they are fed by `core/heights.cc` (see height-bed.md) and `HeightsActive()` tells the audio
thread whether to bother.

Render thread (`Run`), one pass per event (10 ms):

1. `BeginUpdatingAudioObjects` → frame count (480) and currently available dynamic objects.
2. Bed: copy `frameCount` frames from the ring into each static object's buffer once the ring has been
   primed with 2048 frames; if the ring runs dry, output silence, count an underrun and wait for a full
   prime again rather than stutter on every trickle.
3. Dynamic objects: the ring stores, per frame, the floor channels, 4 height samples and 32 object samples, and a
   separate small ring of **block metadata** (one entry per Wwise buffer: start frame, length, active mask,
   32 positions). A pass covers at most two blocks (480 < 1024). For each slot in the active mask: activate a
   Windows object if the slot has none (`ActivateSpatialAudioObject(AudioObjectType_Dynamic)`), copy the
   samples, `SetPosition` from the block holding the pass's first frame. Slots that fall silent keep their
   Windows object for 30 passes (300 ms) before `SetEndOfStream` + release, so a slot reused a few frames
   later isn't re-activated every time.
4. If activation fails (Windows has no free object), the slot is marked failed, panned into the bed with
   constant-power gains between the two adjacent horizontal bed speakers (`PanGains`), counted as a "folded
   slot-pass", and retried once the slot goes idle.
5. Latency trim: track the minimum slack (fill after copy) over 200 passes (~2 s); if it never dropped below
   3072 frames, drop the excess above 2048 in one go. This removes the burst Wwise renders at startup to fill
   XAudio2's queue.
6. `EndUpdatingAudioObjects`; stats every 10 s; status for the menu every 10 passes.

Failure handling: any failing HRESULT ends `Run`, the stream is torn down, `gActive` goes false (the sink hook
stops silencing XAudio2, so the game is audible again), and the thread retries every 0.5 s after device
invalidation or 3 s otherwise. Only the first failure after a working period is logged, so a machine
without spatial sound doesn't flood the log. The event stopping for 2 s (20 timeouts) is treated as
device loss.

Ring: 16384 frames (~340 ms); the producer (`Push`, Wwise audio thread) refuses a buffer that wouldn't fit
and counts overflow frames. Fill normally sawtooths between ~1400 and ~2400 frames (Wwise pushes 1024 at a
time, the renderer takes 480).

The whole thread runs with `AvSetMmThreadCharacteristics("Games")`.

## Standalone test

`tests/spatial_orbit_manual.cc` is the milestone-1 program: `--probe` prints the limits and formats; without
arguments it plays a 7.1.4 bed with a 60 Hz hum on center plus one dynamic object (noise bursts every
250 ms) circling at ear height, then 45° overhead, for 30 s. The soundbar switching from "Dolby Audio" to
"Dolby Atmos" and the height being audible was the go/no-go for the whole project.
