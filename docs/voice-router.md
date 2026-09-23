# The voice router

`core/objects.cc` decides, once per voice per buffer, whether the voice's dry mix goes into the bed (as Wwise
would have panned it) or becomes a dynamic object. It runs on the Wwise audio thread inside the
`CAkVPLMixBusNode::ConsumeBuffer` hook, before the original mixer runs.

## Guiding principle

Dolby's game guidance and Wwise's own object pipeline agree: point sources that move relative to the listener
become objects, diffuse sound (ambience, reverb, wide spread) stays in the bed, whatever exceeds the object
budget folds into the bed, and listener-relative content (music, UI, the player's own sounds) is never an
object. Microsoft's docs describe the same "hero sound" model. The router encodes exactly this.

## Per-voice decision (`OnDryMix`)

Inputs per voice: the ray list (`r`, `theta`, `phi`, count), the panner type (2D voices have no position
and are never objects), the channel mask of the voice's buffer, the `AkAudioMix` (Previous/Next gains into
the 8 speakers), the bus's downstream gain, and the voice's PCM.

Derived values:

- `gainNext` = √Σ gains² over the 7 non-LFE speakers (pan is power-preserving, so this is attenuation ×
  volume); `gainPrev` the same for the Previous gains.
- `pointness` = max speaker gain / `gainNext`. A point source panned between two speakers gives ≥ 0.7; Wwise
  spread smears gain evenly (0.41-0.45 over 5-6 speakers). Threshold **0.6**.
- `level` = `gainNext` × downstream × RMS of the buffer (linear, roughly dBFS-referenced). Threshold
  **0.0003 (≈ -70 dBFS)** for becoming a new object.

Reasons a voice is not a candidate, in priority order (these are what the HUD and the menu's voice table
show):

| Reason | Meaning |
|---|---|
| `Player` | one of the local player's game objects (see below), `PlayerInBed` on |
| `BusFx` | the bus chain runs an effect objects can't reproduce, `BusFx` policy ≥ 1 |
| `MultiPosition` | more than one ray (one sound at several places) |
| `NotMono` | stereo or more channels |
| `Quiet` | `gainNext` = 0, or `level` below threshold |
| `Spread` | `pointness` < 0.6 |
| `Candidate` | qualifies; waiting for a slot or a rank |
| `Object` | is one (or crossfading in/out) |

Two flags per tracked voice:

- `mCandidate` — may become an object (reason == `Candidate`).
- `mHoldable` — may stay one: no `Player`/`BusFx` rule, mono, single position, and `pointness` ≥ 0.5
  (hysteresis of 0.1 below the entry threshold). Quiet is not a reason to lose a slot.

## Slots, roles and crossfades

Roles: `Bed`, `FadeIn` (this buffer crossfades bed → object), `Object`, `FadeOut` (object → bed; the slot
stays reserved until the fade ends).

- A brand-new candidate takes a free slot **immediately**, in the same buffer it first appears: the attack is
  an object from sample 0, and no crossfade is needed because nothing has been heard yet. Footsteps and
  gunshots are the typical case (each is a new Wwise voice).
- Crossfades last one buffer (21 ms) and reuse Wwise's own Previous→Next gain ramp: the bed share is
  scaled `1-w0 → 1-w1` and the object gets `w0 → w1`.
- Object samples = voice PCM × (Previous→Next gain ramp) × object share × downstream gain, then the bus
  chain's Parametric EQ (below). Position = Wwise direction on a sphere of `Distance` m.
- A voice that didn't render this buffer has ended (or gone virtual); its slot is freed without a crossfade
  since Wwise already faded it.

## Ranking (`FinishFrame`, once per buffer)

The pool = candidates ∪ holdable objects, ranked by `level` (objects count double, `kKeepBonus`). The top
`gTarget` are "wanted". Objects that are not wanted are demoted: at once if not holdable (`disqualified`:
became spread, or a bed rule now applies), otherwise only after holding the role ≥ 12 buffers (~250 ms,
`outranked`). Bed candidates that are wanted are promoted (`FadeIn`) after the same minimum hold. Slots are
handed out lowest-free-first.

Before this change objects were demoted as soon as they dipped under the level threshold or spread past
0.6, which produced ~47 demotions per 5 s in fights (each a 21 ms position jump); with decay riding and the
spread hysteresis it is 0-4.

`gTarget` (slots new objects may take) follows `Objects.Enabled` × min(`MaxObjects`, slots the stream
reserved). `gBudget` (slots that may still be in use) shrinks only after the affected objects have faded
back, so switching objects off or lowering the limit is click-free. If the stream reopens with fewer slots,
the excess objects drop to the bed without a fade (nothing to fade through).

## Player attribution

`PlayerInBed` (default on) keeps the player's own sounds in the bed. With the listener on the camera they sit
~3 m ahead, below and slightly left; as objects the renderer collapses them into a sharp point in front of
the viewer, whereas the bed gets the game's own close-range spread (front + side pairs), i.e. the original
mix. Identification is by game object, never by position alone, so NPCs in melee range are unaffected
except for one fallback:

1. **Component entity**: the game object's `AudioEntity::m_name` equals the "PlayerOne_Havok" CRC. The
   component address (entity − 0x40) and its `m_SFXEntity` pointer are remembered and logged
   (`objects: player audio component ...`), re-learned whenever they change (loads, re-initialization).
2. **SFX entity**: game object == the remembered `m_SFXEntity`.
3. **One-shots**: the entity's name is one of `OneShot_100`…`OneShot_999` (CRCs precomputed at first use).
   Then `m_pOwnerHandle` is read: inside the player's component → his (footsteps, and whatever else the
   component fires). Before the component is known, the footstep handles reveal it: handle − 0x1C0 or −
   0x1C8 is a component whose entity name is the player's.
4. **Position fallback for one-shots** not owned by the component (action-tree tasks own the walk↔sprint
   transition steps, damage rigs, VFX): within 1.5 m of the player's entity position
   (`m_WorldMatrix` row 3) → his. NPC one-shots in melee range are caught too, which only sends them to the
   bed as the original mix had them.

All game-memory reads are SEH-guarded (`ReadU32/ReadU64/ReadPosition`), see game-audio.md for why.

## Bus insert effects

Objects skip the bus chain, so any insert effect on the voice's bus or its parents is missing from them.
`ChainHasFx` walks `AkVPL::m_pParent` from the dry bus to the top and reads each bus's `m_aFX`; the device's
final mix is checked as well. Per slot: an effect instance present, not bypassed, no bypass-all. Then:

- **Meter** (0x81): ignored, it only measures.
- **Parametric EQ** (0x69): **reproduced on the object.** The `CAkParametricEQFX` instance keeps the
  normalized biquad coefficients per band and its params say which bands are on and the output level;
  `ApplyEq` runs the same difference equation over the object's samples with per-slot, per-stage, per-band
  memories so the filters are continuous across buffers (memories cleared when a slot changes hands; a band
  whose dirty flag is set is skipped for one buffer, since its coefficients are stale until the bus's own
  `Execute` recomputes them). Up to 4 EQ instances per chain.
- **Anything else** (compressor, ML1 limiter, delay, reverbs, FutzBox…) can't be reproduced. Under
  `BusFx = 1` the voice stays in the bed (`Reason::BusFx`); the master bus counts only under `BusFx = 2`
  because a master limiter would rule out every object; `BusFx = 0` ignores them.

Each bus's active effects are logged once when first seen and again when they change
(`objects: bus <ptr> (id <busID>, parent <ptr>) fx: ParametricEQ (0x690003) [applied to objects]`).

The bed share of a crossfading voice still goes through the real bus effects, so bed and object halves match.

## Telemetry and stats

Every voice reported to `telemetry::Add` carries θ, φ, r, level, sound ID, slot and reason; the frame is
published at buffer end (try-lock; the HUD reads the latest published frame). `objects::LogStats` writes a
line every voice-log period: instant promotions, promotions, demotions by cause, voices ended as objects,
max voices tracked (table of 128), max candidates.
