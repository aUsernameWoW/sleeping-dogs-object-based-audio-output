# SDAtmos — advanced users and developers

[![Build](https://github.com/aUsernameWoW/sleeping-dogs-object-based-audio-output/actions/workflows/build.yml/badge.svg)](https://github.com/aUsernameWoW/sleeping-dogs-object-based-audio-output/actions/workflows/build.yml)

[中文](#中文) | [English](#english)

新手安装说明见 [README.md](README.md)。 · Step-by-step install for players: [README.md](README.md).

## 中文

### 做了什么

游戏用 Wwise 2012 把声音混成 7.1 声道流。Windows 的 Dolby Atmos for home theater 不会做上混：声道流会被编码成
Dolby Audio（DD/DD+）输出，只有调用 `ISpatialAudioClient` 的程序才会输出 Atmos。本 mod hook 了游戏内部的
Wwise 引擎，改由 `ISpatialAudioClient` 输出：

- **7.1 声道床**：游戏的最终混音作为静态床对象输出。接收端切换到 Atmos，听感和原来一样。
- **动态对象**：每个 Wwise 帧，把最响的点状 3D 声音（枪声、脚步、车辆、人声）从声道床里取出来，把它们平移前的
  信号作为动态对象送出，方向就用 Wwise 为它算好的方向，包括高度。原本的 7.1 声像器会把高度压平。环境声、混响、
  spread 很大的声音这类扩散声仍留在床里；声音数量超过格式允许的对象数（HDMI 上为 20 个）时，多出来的并回床里，
  这也是 Dolby 游戏音频指南推荐的做法。
- **7.1.4 高度声道**：游戏本身没有高度内容，mod 把扩散类声音的一部分抬到床的四个顶部声道：天气（雨、雷、风）和
  鸟叫抬得最多，其余环境声（城市、人群、水）和混响返回少一些；地面声道相应减去同样的能量。抬上去的信号经过短延迟
  + 全通 + 高通的去相关处理，前后两对不同，这是 Dolby/DTS 上混器和 Atmos 混音指南的通行做法。
- **不绑定 Dolby**：对象上限、床布局和格式都在运行时向系统查询，所以 DTS:X for Home Theater、Windows Sonic 也走
  同一条路径（目前只在 Dolby Atmos for home theater 上测试过）。

状态：**实验性**。在作者的环境里游戏内工作正常，听感测试仍在进行。

### 原理

安装版 Steam exe 不带符号。Wwise 函数靠字节特征码定位；特征码取自旧版 v1.0 exe 及其 PDB（来自 SDmodding
项目）。Wwise 是静态链接的，两个版本里完全相同。找不到某组特征码时，对应的功能不启用，日志里写 `MISSING`。
hook 点：

- `CAkSinkXAudio2::Init/PassData/PassSilence`：在 XAudio2 拿到之前取走最终混音。XAudio2 继续静音运行，
  给 Wwise 当时钟。空间音频不可用时，游戏照旧走 XAudio2，不受影响。
- `CAkLEngine::RunVPL` + `CAkVPLMixBusNode::ConsumeBuffer`：每个声音混入总线的位置。对于成为对象的声音，
  mod 按 Wwise 本来会用的增益渐变（含总线音量）取出它的 PCM，再把 Wwise 自己的混音矩阵相应调低，让床里只剩
  不属于对象的部分。在床和对象之间切换时，用一个 21 ms 的缓冲做交叉淡化。
- 总线向上级总线传递输出的位置（高度声道），以及 `AK::SoundEngine::SetPosition`（把角色的声音位置从脚下抬到
  头部高度，`ActorLift`）。

完整设计说明和涉及的 Wwise 内部细节见 [CLAUDE.md](CLAUDE.md)（英文）；`docs\` 里有面向接手者的详细说明
（英文）：架构与数据流、Wwise 2012.2 内部结构与偏移、游戏侧的音频实体/角色组件/听者、Windows 空间音频输出、
对象路由策略、逆向流程、测试与日志解读。从 [docs/README.md](docs/README.md) 开始。

### 需求

- 《热血无赖：终极版》的两个发行版本：当前版本（在游戏里验证过）和旧版 v1.0（特征码就取自它），Windows 10/11
  x64。
- 为输出设备启用一种空间音效格式（Windows 声音设置 → 空间音效）：HDMI 回音壁/功放用 Dolby Atmos for home
  theater（Dolby Access 应用），或 DTS:X / Windows Sonic。
- 任意 ASI 加载器，例如 [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)（`SDAtmos.zip`
  里自带一份，作为 `dinput8.dll`）。
- 可选：支持插件的 [ReShade](https://reshade.me) **6.8.0**，用于游戏内菜单和 HUD。

### 下载

[Releases](https://github.com/aUsernameWoW/sleeping-dogs-object-based-audio-output/releases) 里每个版本都有：

| 文件 | 内容 |
| --- | --- |
| `SDAtmos.zip` | 解压到游戏目录：`dinput8.dll`（Ultimate ASI Loader）+ `plugins\SDAtmos.asi` + 许可声明 |
| `SDAtmos.asi` | 只有 mod 本体，放进已有加载器的 `plugins\` |
| `SDAtmos.pdb` | 调试符号，只在分析崩溃转储时需要 |
| `THIRD-PARTY-NOTICES.md` | 第三方代码的许可证 |

`main` 上每次提交都会自动编译、测试并发布为预发布版 `build-<N>`（没有在游戏里测过）。在游戏里验证过的构建会被
转为正式版；README 里的下载链接指向最新的正式版。Nexus Mods 上主文件 “SDAtmos” 是正式版，
“SDAtmos GitHub CI Build” 是每次的预发布版，都是同一个 `SDAtmos.zip`。

已经有 ASI 加载器（不论叫 `dinput8.dll`、`winmm.dll` 还是别的名字）时，只需要把 `SDAtmos.asi` 放进它加载插件
的目录（通常是 `plugins\`）。`SDAtmos.ini` 和 `SDAtmos.log` 写在 `.asi` 旁边。

### 游戏内

- **F9**：开关动态对象，用来和纯 7.1 声道床做 A/B 对比。
- **F7**：开关高度声道（A/B 对比）。
- **F8**：HUD（需要 ReShade）。显示所有带位置的声音的雷达图，并在画面上标出它们的方向。青色 = 对象，黄色 =
  符合条件但在排队，灰色 = 留在床里，绿色 = 沈威自己的声音（留在床里，`PlayerInBed`），橙色 = 所在总线带插入
  效果（留在床里，`BusFx`）。雷达以听者为中心，编号是对象槽位，点上的短杆表示声音在听者上方或下方。
- **F6**（调试）：强制下雨/放晴，用来听高度声道。
- ReShade 菜单 → **SDAtmos** 标签页：流状态、实时设置、声音列表、保存到 ini。

按键可以在 `SDAtmos.ini` 里改（`ToggleObjectsKey`、`ToggleHeightsKey`、`ToggleHudKey`、`ToggleRainKey`，
值为虚拟键码）。首次启动会生成带注释的 `SDAtmos.ini`（中英双语），日志写到 `SDAtmos.log`。

### 编译

Visual Studio 2022（v143），Windows SDK 10.0.26100。项目需要放在工作区的 `mods\SDAtmos`，工作区里还要有：

- `reference\reshade`：ReShade v6.8.0 源码，并初始化 `deps\imgui` 子模块。
- `reference\minhook`：[MinHook](https://github.com/TsudaKageyu/minhook) v1.3.4 源码（随项目一起编译）。

GitHub Actions 会对推送和 PR 按同样的布局编译（`-warnAsError`）并运行自动测试，依赖的确切版本见
`.github/workflows/build.yml`；然后打包 `SDAtmos.zip`，其中 Ultimate ASI Loader 的版本和 SHA-256 固定在
`.github/asi-loader.env`。推送到 `main` 且测试通过的构建会发布为预发布版 `build-<N>`，并作为新版本上传到
Nexus Mods；在 GitHub 上把预发布版转为正式版，会把它上传到 Nexus 的主文件（`nexus-release.yml`）。
`asi-loader.yml` 每月检查一次 Ultimate ASI Loader 的新版本，有新版时开 PR 更新 `asi-loader.env`；
Dependabot 每月更新 Actions 的版本。

### 致谢

- [SDmodding](https://github.com/SDmodding)：旧版 PDB 和 SDK。
- [MinHook](https://github.com/TsudaKageyu/minhook)。
- [ReShade](https://github.com/crosire/reshade) 的插件 API 和 Dear ImGui。
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)。

第三方代码及其许可证见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

与 Square Enix、United Front Games、Audiokinetic、Dolby、DTS、Microsoft 均无关联。

## English

### What it does

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
- **7.1.4 height channels**: the game has no height content of its own. The mod moves a share of the diffuse
  material up into the bed's four top channels: weather (rain, thunder, wind) and birds the most, the rest of
  the ambience (city, crowds, water) and reverb returns less; the floor loses the same energy. The lifted
  signal is decorrelated (short delay + all-passes + high-pass, front and back pairs different), which is
  what Dolby/DTS upmixers and Atmos mixing guides do.
- **Nothing Dolby-specific**: object limits, bed layout and format are queried at runtime, so DTS:X for Home
  Theater and Windows Sonic go through the same path (only Dolby Atmos for home theater has been tested).

Status: **experimental**. It works in-game on the author's setup. The listening tests are still under way.

### How it works

The installed Steam exe has no symbols. The Wwise functions are found by byte signatures, which were
generated from the legacy v1.0 build and its PDB (from the SDmodding project). Wwise is statically linked
and identical in both builds. When a group of signatures isn't found, that feature stays off and the log says
`MISSING`. The hooks:

- `CAkSinkXAudio2::Init/PassData/PassSilence`: take the final mix before XAudio2 sees it. XAudio2 keeps
  running silently as the clock that paces Wwise. If spatial audio isn't available, the game stays on XAudio2
  unchanged.
- `CAkLEngine::RunVPL` + `CAkVPLMixBusNode::ConsumeBuffer`: the point where each voice is mixed into its
  bus. For voices that become objects, the mod takes the voice's PCM with the same gain ramp Wwise would
  apply (including bus volumes). It then scales Wwise's own mix matrix down, so the bed gets only what isn't
  an object. Moves between bed and object crossfade over one 21 ms buffer.
- Where each bus hands its output to its parent (height channels), and `AK::SoundEngine::SetPosition` (lifts
  characters' sounds from their feet to head height, `ActorLift`).

[CLAUDE.md](CLAUDE.md) has the full design notes and the Wwise internals involved. `docs\` holds the
long-form material for whoever picks this up: architecture and data flow, Wwise 2012.2 internals and
offsets, the game's audio entities/actor components/listener, Windows spatial audio output, the object
routing policy, the reverse-engineering workflow, testing and log reading. Start at
[docs/README.md](docs/README.md).

### Requirements

- Both released builds of Sleeping Dogs: Definitive Edition: the current one (verified in game) and the
  legacy v1.0 (the signatures come from it), Windows 10/11 x64.
- A spatial sound format enabled for the output device (Windows Sound settings → Spatial sound): Dolby Atmos
  for home theater (Dolby Access app) for an HDMI receiver/soundbar, or DTS:X / Windows Sonic.
- Any ASI loader, e.g. [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
  (`SDAtmos.zip` includes it as `dinput8.dll`).
- Optional: [ReShade](https://reshade.me) **6.8.0** with add-on support, for the in-game menu and HUD.

### Downloads

Every version on [Releases](https://github.com/aUsernameWoW/sleeping-dogs-object-based-audio-output/releases)
has:

| File | Contents |
| --- | --- |
| `SDAtmos.zip` | Unpacks into the game folder: `dinput8.dll` (Ultimate ASI Loader) + `plugins\SDAtmos.asi` + notices |
| `SDAtmos.asi` | The mod alone, for the `plugins\` folder of an existing loader |
| `SDAtmos.pdb` | Debug symbols, only needed to read crash dumps |
| `THIRD-PARTY-NOTICES.md` | Licenses of the third-party code |

Every commit on `main` is built, tested and published as a prerelease `build-<N>` (not tested in game).
Builds verified in game are promoted to full releases; the README's download link points to the newest one.
On Nexus Mods the main file "SDAtmos" is the full release and "SDAtmos GitHub CI Build" follows the
prereleases; both are the same `SDAtmos.zip`.

If you already have an ASI loader (whether it's called `dinput8.dll`, `winmm.dll` or something else), just put
`SDAtmos.asi` where it loads plugins from (usually `plugins\`). `SDAtmos.ini` and `SDAtmos.log` are written
next to the `.asi`.

### In game

- **F9**: dynamic objects on/off. This is an A/B switch against the plain 7.1 bed.
- **F7**: height channels on/off (A/B).
- **F8**: HUD (needs ReShade). It shows a radar of every positioned sound and markers at their on-screen
  directions. Cyan = object, yellow = qualifies but waiting, gray = stays in the bed, green = the player's
  own sounds (kept in the bed, `PlayerInBed`), orange = its bus runs insert effects (kept in the bed, `BusFx`).
  The radar is centered on the listener, numbers are object slots, and a stick on a dot means above (up) or
  below (down) the listener.
- **F6** (debug): forces rain/clear weather, to listen to the height channels.
- ReShade menu → **SDAtmos** tab: stream status, live settings, voice list, save to ini.

The keys can be changed in `SDAtmos.ini` (`ToggleObjectsKey`, `ToggleHeightsKey`, `ToggleHudKey`,
`ToggleRainKey`, virtual-key codes). On first start the mod writes a commented `SDAtmos.ini` (Chinese/English)
and logs to `SDAtmos.log`.

### Building

Visual Studio 2022 (v143), Windows SDK 10.0.26100. The project expects to sit at `mods\SDAtmos` in a
workspace that also has:

- `reference\reshade`: ReShade v6.8.0 source, with the `deps\imgui` submodule initialized.
- `reference\minhook`: [MinHook](https://github.com/TsudaKageyu/minhook) v1.3.4 source (compiled with the
  project).

GitHub Actions builds pushes and pull requests in that same layout (with `-warnAsError`) and runs the
automated tests; `.github/workflows/build.yml` lists the exact dependency versions. It then packages
`SDAtmos.zip`, with the Ultimate ASI Loader version and SHA-256 pinned in `.github/asi-loader.env`. Builds of
`main` that pass are published as prereleases `build-<N>` and uploaded to Nexus Mods as a new version;
promoting a prerelease to a full release on GitHub uploads it to the Nexus main file (`nexus-release.yml`).
`asi-loader.yml` checks monthly for a new Ultimate ASI Loader release and opens a PR that updates
`asi-loader.env`; Dependabot updates the Actions monthly.

### Credits

- [SDmodding](https://github.com/SDmodding): the legacy build's PDB and SDK.
- [MinHook](https://github.com/TsudaKageyu/minhook).
- [ReShade](https://github.com/crosire/reshade) add-on API and Dear ImGui.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).

The third-party code and its licenses are listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Not affiliated with Square Enix, United Front Games, Audiokinetic, Dolby, DTS or Microsoft.
