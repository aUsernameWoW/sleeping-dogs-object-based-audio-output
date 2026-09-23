# Sleeping Dogs: Definitive Edition — object-based audio output (SDAtmos)

[中文](#中文) | [English](#english)

## 中文

让《热血无赖：终极版》通过 Windows 空间音频输出真正的对象音频：回音壁/功放显示 **Dolby Atmos**，而不是
Dolby Audio。

游戏用 Wwise 2012 把声音混成 7.1 声道流。Windows 的 Dolby Atmos for home theater 不会做上混：声道流会被编码成
Dolby Audio（DD/DD+）输出，只有调用 `ISpatialAudioClient` 的程序才会输出 Atmos。本 mod hook 了游戏内部的
Wwise 引擎，改由 `ISpatialAudioClient` 输出：

- **7.1 声道床**：游戏的最终混音作为静态床对象输出。接收端切换到 Atmos，听感和原来一样。
- **动态对象**：每个 Wwise 帧，把最响的点状 3D 声音（枪声、脚步、车辆、人声）从声道床里取出来，把它们平移前的
  信号作为动态对象送出，方向就用 Wwise 为它算好的方向，包括高度。原本的 7.1 声像器会把高度压平。环境声、混响、
  spread 很大的声音这类扩散声仍留在床里；声音数量超过格式允许的对象数（HDMI 上为 20 个）时，多出来的并回床里，
  这也是 Dolby 游戏音频指南推荐的做法。
- **不绑定 Dolby**：对象上限、床布局和格式都在运行时向系统查询，所以 DTS:X for Home Theater、Windows Sonic 也走
  同一条路径（目前只在 Dolby Atmos for home theater 上测试过）。

状态：**实验性**。在作者的环境里游戏内工作正常，听感测试仍在进行。

### 原理

安装版 Steam exe 不带符号。Wwise 函数靠字节特征码定位；特征码取自旧版 v1.0 exe 及其 PDB（来自 SDmodding
项目）。Wwise 是静态链接的，两个版本里完全相同。hook 点：

- `CAkSinkXAudio2::Init/PassData/PassSilence`：在 XAudio2 拿到之前取走最终混音。XAudio2 继续静音运行，
  给 Wwise 当时钟。空间音频不可用时，游戏照旧走 XAudio2，不受影响。
- `CAkLEngine::RunVPL` + `CAkVPLMixBusNode::ConsumeBuffer`：每个声音混入总线的位置。对于成为对象的声音，
  mod 按 Wwise 本来会用的增益渐变（含总线音量）取出它的 PCM，再把 Wwise 自己的混音矩阵相应调低，让床里只剩
  不属于对象的部分。在床和对象之间切换时，用一个 21 ms 的缓冲做交叉淡化。

完整设计说明和涉及的 Wwise 内部细节见 `CLAUDE.md`（英文）。

### 需求

- 《热血无赖：终极版》（Steam 当前版本），Windows 10/11 x64。
- 为输出设备启用一种空间音效格式（Windows 声音设置 → 空间音效）：HDMI 回音壁/功放用 Dolby Atmos for home
  theater（Dolby Access 应用），或 DTS:X / Windows Sonic。
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)（例如作为 `dinput8.dll`）。
- 可选：支持插件的 [ReShade](https://reshade.me) 6.8.0，用于游戏内菜单和 HUD。

### 安装

把 `SDAtmos.asi` 放进游戏的 `plugins\` 文件夹。首次启动会在旁边生成带注释的 `SDAtmos.ini`（中英双语），日志
写到 `SDAtmos.log`。

游戏内：

- **F9**：开关动态对象，用来和纯 7.1 声道床做 A/B 对比。
- **F8**：HUD（需要 ReShade）。显示所有带位置的声音的雷达图，并在画面上标出它们的方向。青色 = 对象，黄色 =
  符合条件但在排队，灰色 = 留在床里，绿色 = 沈威自己的声音（留在床里，`PlayerInBed`）。
- ReShade 菜单 → **SDAtmos** 标签页：流状态、实时设置、声音列表、保存到 ini。

### 编译

Visual Studio 2022（v143），Windows SDK 10.0.26100。项目需要放在工作区的 `mods\SDAtmos`，工作区里还要有：

- `reference\reshade`：ReShade v6.8.0 源码，并初始化 `deps\imgui` 子模块。
- `reference\SPatch\external`：SDmodding 的 MinHook 构建（`MinHook.h` / `MinHook.lib`）。

### 致谢

- [SDmodding](https://github.com/SDmodding)：旧版 PDB、SDK 和 MinHook 构建。
- [ReShade](https://github.com/crosire/reshade) 的插件 API 和 Dear ImGui。

与 Square Enix、United Front Games、Audiokinetic、Dolby、DTS、Microsoft 均无关联。

## English

Makes Sleeping Dogs: Definitive Edition output real object audio through Windows spatial audio: the
soundbar/receiver shows **Dolby Atmos** instead of Dolby Audio.

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

### How it works

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

### Requirements

- Sleeping Dogs: Definitive Edition (Steam, current build), Windows 10/11 x64.
- A spatial sound format enabled for the output device (Windows Sound settings → Spatial sound): Dolby Atmos
  for home theater (Dolby Access app) for an HDMI receiver/soundbar, or DTS:X / Windows Sonic.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (e.g. as `dinput8.dll`).
- Optional: [ReShade](https://reshade.me) 6.8.0 with add-on support, for the in-game menu and HUD.

### Install

Copy `SDAtmos.asi` into the game's `plugins\` folder. On first start it writes a commented `SDAtmos.ini`
next to itself (bilingual, Chinese/English) and logs to `SDAtmos.log`.

In game:

- **F9**: dynamic objects on/off. This is an A/B switch against the plain 7.1 bed.
- **F8**: HUD (needs ReShade). It shows a radar of every positioned sound and markers at their on-screen
  directions. Cyan = object, yellow = qualifies but waiting, gray = stays in the bed, green = the player's
  own sounds (kept in the bed, `PlayerInBed`).
- ReShade menu → **SDAtmos** tab: stream status, live settings, voice list, save to ini.

### Building

Visual Studio 2022 (v143), Windows SDK 10.0.26100. The project expects to sit at `mods\SDAtmos` in a
workspace that also has:

- `reference\reshade`: ReShade v6.8.0 source, with the `deps\imgui` submodule initialized.
- `reference\SPatch\external`: `MinHook.h` / `MinHook.lib` from SDmodding's MinHook build.

### Credits

- [SDmodding](https://github.com/SDmodding): the legacy build's PDB and SDK, and the MinHook build.
- [ReShade](https://github.com/crosire/reshade) add-on API and Dear ImGui.

Not affiliated with Square Enix, United Front Games, Audiokinetic, Dolby, DTS or Microsoft.
