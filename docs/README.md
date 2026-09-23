# SDAtmos documentation

Everything learned while building SDAtmos, written for whoever picks it up next: how the mod works, what
Sleeping Dogs: Definitive Edition and its Wwise build do internally, and how those facts were established.
`../CLAUDE.md` is the short, agent-oriented summary of the same material; these documents are the long form.

Suggested reading order:

1. [architecture.md](architecture.md) — what the mod does, data flow, threads, files, design decisions.
2. [wwise-internals.md](wwise-internals.md) — Wwise 2012.2 inside `sdhdship.exe`: render loop, sink,
   voice pipeline, buses, effects, struct layouts and offsets.
3. [game-audio.md](game-audio.md) — the game (UFG engine) side: audio entities, actor components, one-shots,
   the listener, the local player, bus layout seen in-game.
4. [spatial-output.md](spatial-output.md) — Windows spatial audio (`ISpatialAudioClient`): formats, limits,
   bed/object model, the ring and clocking, fallback paths.
5. [voice-router.md](voice-router.md) — which voices become objects and why, player attribution, bus effects
   and EQ reproduction, crossfades, ranking.
6. [height-bed.md](height-bed.md) — the 7.1.4 height channels: what the industry puts overhead, which buses
   feed them here, the energy-preserving carve and the decorrelator.
7. [reverse-engineering.md](reverse-engineering.md) — the workflow: legacy PDB + IDA, pattern scanning in the
   installed build, verifying offsets, the SDmodding SDK, pitfalls.
8. [testing-and-logs.md](testing-and-logs.md) — build/test/deploy, in-game test procedure, how to read
   `SDAtmos.log` and the HUD, baseline numbers, troubleshooting.

Conventions used throughout:

- Addresses written as `0x140xxxxxx` are virtual addresses in the **legacy** Steam v1.0 build (image base
  `0x140000000`), the one the PDB and IDA database in `reference\SDmodding\game-itself` describe. `+0xNNNNNN`
  is an RVA. The installed exe is a different build; see reverse-engineering.md for how the two relate.
- Struct offsets are hexadecimal bytes from the start of the object, taken from the PDB types and verified
  against decompiled code.
- "Buffer" means one Wwise render buffer of 1024 frames (21.3 ms at 48 kHz); "pass" means one
  `ISpatialAudioClient` update of 480 frames (10 ms).
