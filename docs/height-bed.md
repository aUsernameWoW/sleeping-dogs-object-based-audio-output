# The height bed (7.1.4)

Since 2026-09-23 the static bed is 7.1.4 when the spatial format's native bed has the four top channels
(Dolby Atmos for home theater does: `TFL TFR TBL TBR`). The game only ever mixed 7.1, so the height content
is derived from the floor mix by `core/heights.*` and `core/height_dsp.hh`.

## Why, and what the industry does

- Dolby's game-developer guidance ("Artistic considerations", Dolby Atmos) describes the bed as the place
  for "diffuse elements such as ambience, reverb, or anything else that doesn't need to dynamically move
  around the room", and Dolby's Pro Logic IIz literature is explicit that ambient sound and "amorphous
  effects such as rain or wind" are what height speakers are for. Microsoft's spatial sound docs suggest
  that renderers with 7.1 support "simply add support for the four additional height channels" of the
  ISpatialAudioClient bed.
- Mixing guides for Atmos (Production Expert, Pro Sound Effects, Dolby/UMG music best practices) agree on
  the negatives: never put *identical* content in front and back heights (it collapses and cancels in
  fold-downs), and keep dry direct sources on the floor or as objects.
- Psychoacoustics (Hyunkook Lee, Huddersfield): a height-layer copy of a floor signal pulls the image up
  to a phantom between the layers unless it is 7.5-9.5 dB lower; elevation is perceived from spectral
  cues, not level or timing, and nothing below ~250 Hz localizes overhead at all.
- Commercial upmixers (Dolby Surround Upmixer, DTS Neural:X, Auro-Matic) extract the ambient/diffuse part
  of the floor channels for the heights. The DTS height-upmix patent (US20170325043A1) gives the recipe the
  mod follows: a 5-20 ms Haas delay on the height path (transients stay localized on the floor), a nested
  all-pass decorrelator (delays of a few ms, |g| < 1), and a low-frequency shelf down on the heights.
- Dolby's Renderer guide notes that home-theater renderers do no size/decorrelation processing themselves,
  so any decorrelation has to be baked into the signal, which is what the mod does.

Links and quotes are in the research notes of the 2026-09-23 session; the numbers used are below.

## Mechanism

1. **Where.** After all voices have been mixed into their buses, `CAkLEngine::GetBuffer` walks the active
   `AkVPL`s (children before parents) and calls `CAkLEngine::TransferBuffer(vpl)`: the bus runs its insert
   effects (`GetResultingBuffer` → `ProcessAllFX`), then hands its `m_BufferOut` to the parent's
   `CAkVPLMixBusNode::ConsumeBuffer(AkAudioBufferBus&, bool pan, AkAudioMix*)` or, for a top-level bus, to
   `CAkVPLFinalMixNode::ConsumeBuffer`. Both are hooked; the source bus is recovered from the buffer pointer
   (`m_BufferOut` is at `+0x420` of the `AkVPL`). This is the only point where a reverb bus's *return*
   (post-effect) exists as a separate signal.
2. **Which buses.** Decided by the bus itself (its `m_busID`), never by ancestry, so each signal is carved
   exactly once per tier:
   - *Sky*: bus IDs in `Heights.SkyBuses`, default `weather` (317282339: thunder, wind, rain) and `birds`
     (352130103). Default share -3 dB.
   - *Ambience*: `Heights.AmbienceBuses`, default `ambient` (77978275), the parent of the whole ambience
     subtree (weather, birds, city, traffic, crowds, water, kitchens...). Default share -6 dB. Weather is
     carved at its own transfer first; what remains flows into `ambient` and is carved again there, so
     weather ends up 0.5 + 0.5 × 0.25 ≈ 63 % overhead, plain city ambience 25 %.
   - *Reverb*: any bus whose active insert effects include RoomVerb, MatrixReverb or ConvolutionReverb
     (`wwise::IsReverbFx`). In this game those are the 28 aux buses of `Init.bnk`. Default share -6 dB.
   The IDs come from the game's `Init.bnk` (extracted from `SFX.pck`; see game-audio.md for the tree and
   how the names were recovered).
3. **How much.** For a share `s` (linear, from the dB setting) the height accumulators receive
   `floor × s × (bus volume ramp) × (parent's downstream gain)`, i.e. exactly the level the signal would
   have had at the device output, and the floor channels are scaled in place by `sqrt(1 - s² × Σw²)`
   (energy-preserving; `w` are the mapping weights below, so a channel that feeds two heights at -3 dB
   each loses the same power as one that feeds one height at 0 dB). The enable switch (F7) is a per-buffer
   ramp `m` multiplying `s`, so toggling crossfades over one 21 ms buffer.
4. **Mapping** (`height_dsp::BuildFloorMap`, from the bus buffer's Wwise channel mask, planar order FL FR C
   BL BR SL SR with LFE last): `TFL ← FL`, `TFR ← FR`, `TBL ← 0.707 (SL + BL)`, `TBR ← 0.707 (SR + BR)`
   (a 5.1 bus uses just its backs at 1.0); a bus without surrounds spreads FL/FR over front and back
   heights at 0.707 each. Center and LFE never go up.
5. **Decorrelation** (`height_dsp::DecorrelatorBank`, once per frame on the summed accumulators; linear,
   so summing first is the same as decorrelating each source): pre-delay 8 ms front / 12 ms back
   (`Heights.Delay`, back = front + 4), three Schroeder all-passes (front 1.7/3.1/4.7 ms, back
   2.3/3.7/5.3 ms, g = 0.6), then a 2nd-order Butterworth high-pass at 200 Hz (`Heights.HighPass`). Left and
   right of a pair share settings so a centered floor source stays centered overhead; front and back
   differ so they never carry identical content.
6. **Output.** `heights::FinishFrame` returns the four planar channels; `spatial::Push` writes them after
   the floor channels in the ring (stride = floor + 4 + 32 objects), and the render thread copies them
   into the four extra static objects the stream activated (`AudioObjectType_TopFront*/TopBack*`). If the
   format's native static mask lacks any of the four, the stream logs it, activates none, and
   `heights::Active()` is false: nothing is carved and the floor is left untouched.

## Settings

`[Heights]` in `SDAtmos.ini`: `Enabled` (F7), `Sky`, `Ambience`, `Reverb` (dB shares, live in the menu),
`Delay`, `HighPass` (static), `SkyBuses`, `AmbienceBuses` (comma-separated IDs, up to 8 each; an empty value
means none). The ReShade tab has the three sliders; the HUD status line shows `heights ON/OFF/n/a`.

## What to listen for

- Rain and thunder should sit overhead and around, not in front; city ambience should feel taller without
  moving; interiors with reverb should gain ceiling.
- Dialogue, gunshots, footsteps, cars, music must not move: they are objects or on other buses.
- The floor loses at most 1.25 dB (ambience at -6 dB) / 3 dB (weather at -3 dB) of its ambience level,
  compensated by the heights; overall loudness should stay put on F7 A/B.
- Combing: the heights are a delayed, filtered copy of the floor. If ambience sounds hollow, raise the
  pre-delay (12-15 ms) or lower the shares.

## Log lines

- `heights: 48000 Hz, pre-delay 8.0/12.0 ms front/back, high-pass 200 Hz, shares ...` at sink init.
- `hook: height bed hooks ready, heights on at start` — both bus-transfer signatures found.
- `spatial: stream started ... bed [FL FR C LFE SL SR BL BR TFL TFR TBL TBR] (12 ch)` — the stream has the
  heights; otherwise `spatial: this format's bed has no height channels [...]`.
- `heights: bus 317282339 (weather) -> 2276207995 (?), mask 0x63F: sky tier` — once per bus ID at its
  first transfer, for every bus (tier `none` included), which makes the log a dump of the runtime bus tree.
  First in-game session (2026-09-23): the reverb tier fired all the time, the ambience tier never: the
  `ambient` bus never transferred. Whatever ambience routes through at runtime shows up in these lines and
  in the voice snapshots' `| bus a>b>c` chains; fix `AmbienceBuses` from there.
- The F6 debug hotkey forces rain (`core/weather.*`), since the game's random weather may not oblige.
- `heights: on; bus transfers carved: S sky, A ambience, R reverb; peak dBFS TFL TFR TBL TBR: ...` every
  10 s.

Offline test: `tests/heights_test.cc` (channel maps for 7.1/5.1/stereo, energy check of the carve, the
decorrelator's delay/flatness/high-pass, front ≠ back, left = right).
