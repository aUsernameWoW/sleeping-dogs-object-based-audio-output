# Reverse-engineering workflow

## Two builds

- **Installed**: the current Steam build of `sdhdship.exe` (SHA-256 `2A33EC78…0527DA`), x64, DX11.
- **Legacy**: Steam v1.0 (SHA-256 `C6DB199B…8CC6035`), the build the SDmodding community documented. Its
  exe, full PDB and an IDA database live in `reference\SDmodding\game-itself` (read-only). All SDK RVAs and
  every address in these docs refer to it.

The two are near-identical: same file size, `.text` 80 bytes longer in the installed build. Functions keep
their bytes except for rel32/RIP-relative displacements, and shift by a few dozen bytes: e.g.
`CAkMixer::Mix3D` -0x20, `CAkSinkXAudio2::PassData` -0x10, `RunVPL`/`ConsumeBuffer` +0x50,
`CAkOutputMgr::m_Devices` reference in `AnalyzeMixingGraph` +0x134. Data addresses checked so far are
identical (`g_pipelineCoreFrequency` RVA 0x20F71F4), but the mod never relies on that: everything is found
by pattern or decoded from a RIP-relative operand.

## Finding things

1. **Name → address in the legacy build**: the IDA MCP tools on `sdhdship.exe.i64` (`lookup_funcs`,
   `func_query` with a regex on the mangled name, `xrefs_to`, `decompile`, `disasm`, `type_inspect` for
   struct layouts, `get_bytes`). The database has full PDB names, so `?RegisterGameObj@SoundEngine@AK@@...`
   style queries work; `find` with `type: string` locates strings.
2. **Legacy bytes → signature**: take 16-40 bytes from the function start (or from a distinctive spot inside
   it), replace rel32/RIP displacements and stack-frame constants that might change with `?`, and check
   uniqueness offline against the installed exe with a quick Python regex over the file (see the commit
   history for the snippet), then in-game: `scan::FindUnique` logs `scan: <name> at +0xRVA` or `N matches,
   not using it`.
3. **Globals**: never hard-code. Find an instruction that references the global RIP-relatively inside a
   signatured function and decode it with `scan::RipTarget(disp, trailing)` (`trailing` = bytes after the
   displacement in the same instruction, e.g. an immediate). Examples: the sample rate from
   `CAkSinkXAudio2::Init + 0x4D`, `CAkOutputMgr::m_Devices` from `AnalyzeMixingGraph`.
4. **Layouts**: `type_inspect` gives member offsets; confirm them in decompiled code that uses the field
   (the decompiler shows `this->field` names) and, where cheap, by what the log shows in-game (e.g. the
   `AkAudioMix` speaker order was confirmed from θ vs which speaker index lit up).
5. **Game-side classes**: the SDmodding SDK headers are hand-written from the same PDB and include RVAs and
   handy constants (e.g. `IsLocalPlayer` with the "PlayerOne_Havok" hash). Verify against the PDB before
   relying on a size or offset; where they disagree, the PDB wins.
6. **Hashes**: `qSymbol` is CRC-32 poly 0x04C11DB7 MSB-first, init -1, no final xor (`sCrcTable32` entry 1 =
   0x04C11DB7 confirms it). `game::SymbolUID` implements it; verify a known pair (e.g. "PlayerOne_Havok" →
   0x90ECB5FF) when in doubt. Wwise IDs (`TiDo::CalcWwiseUid`) are FNV-based and unrelated.

## Hooking

- MinHook (upstream v1.3.4, built from source): `Hook()` runs `MH_CreateHook` + `MH_EnableHook`;
  `MH_RemoveHook` disables and frees. All hooks
  are created in `DllMain` before the game runs, so there is no thread-safety concern at install time. Pairs
  that only make sense together (`RunVPL` + `ConsumeBuffer`) are rolled back if one fails.
- Prefer intercepting where the data already is: the plan to hook `PostEvent`/`SetPosition` was dropped
  because those give IDs and positions but no samples; the PCM and the listener-relative direction only
  exist together inside `RunVPL`/`ConsumeBuffer`.
- The hook that rewrites `AkAudioMix` gains and the one that reads the sink's `m_MasterOut` both run on the
  Wwise audio thread; nothing there may block (telemetry uses try-lock, the ring is SPSC lock-free, logging
  is buffered by the CRT).

## Pitfalls collected

- Don't edit `.vcxproj`/`.sln` through shell heredocs or Python string literals: `\r` inside paths becomes a
  carriage return. Use the editor tools. Likewise `sed -i` strips CRs from CRLF files.
- Source files are CRLF; a new file written with LF triggers git's autocrlf warning. Convert before
  committing.
- `WritePrivateProfileString` treats BOM-less ini files as ANSI and would mangle the UTF-8 comments, hence
  the byte-wise `config::Save`.
- `__try/__except` can't share a function with objects that have destructors: the guarded readers are tiny
  standalone functions.
- The ASI is loaded from inside `dinput8.dll`; `GetModuleHandle(nullptr)` is the game, `GetModuleHandleEx`
  on our own address pins us (the hooks and threads point into the module).
- The game window is ANSI (`CreateWindowExA`); irrelevant to audio but easy to trip over when adding UI.
- ReShade add-ons must match ReShade's Dear ImGui build exactly (6.8.0 → ImGui 1.92.5 docking,
  `IMGUI_VERSION_NUM 19250`); a `static_assert` guards it.
- Commit signing uses the Windows OpenSSH agent; if it refuses, the agent wants the user's approval.
  Retry rather than disabling signing.
