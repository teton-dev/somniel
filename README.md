# Somniel
<img src="app/src/main/ic_launcher-playstore.png" width="160" alt="Somniel icon">

**Somniel runs Fire Emblem Engage (v2.0.0) natively on Android.** It is not an emulator and not a static recompilation:
the game's own AArch64 code runs directly on your phone's CPU, and Somniel supplies what sits underneath it (a
Horizon OS layer, an NVN-to-Vulkan renderer, audio, input and save data).

> **Early, experimental software, heavily assisted by Claude.** Expect bugs. Please read [What to expect](#what-to-expect) before installing.

Somniel contains **no game files**. You need your own legally obtained copy of Fire Emblem Engage with the 2.0.0
update, and your own console keys. The first-run setup dialog walks you through picking them.

---

## Download and install

1. Download the latest APK from the [Releases page](https://github.com/teton-dev/somniel/releases).
2. Install it (allow installs from your browser or file manager if Android asks). To update, install over the existing
   app, or use **Check for updates** in the sidebar. **Do not uninstall first**, or you lose the game data and saves
   stored in the app.
3. Open Somniel and follow the first-run setup to point it at your own game files and keys.
4. The first launch asks whether to share anonymous data (see below), offers the optional Cobalt mod loader (see
   [Mods](#mods)) and, on non-Adreno GPUs, shows a notice.

The first boot is slower than later ones while the game's shaders are prepared in the background.

---

## Tested hardware

Built and tuned on the **Snapdragon 8 Gen 2 (Adreno 740)**, 8 GB and 12 GB models.

| Chip (GPU) | Result |
| --- | --- |
| Snapdragon 8 Gen 2 (Adreno 740) | The main target: a mostly steady 60 fps. |
| Dimensity 7300 (Mali-G615 MC2) | Playable at 30 fps in the handheld profile; the Somniel hub holds 29-30 fps with Hidden object culling (on by default on Mali). |
| Exynos 1330 (Mali-G68 MP2) | Boots and plays, but 3D scenes run well below 30 fps. |
| Dimensity 6300 (Mali-G57 MC2) | Boots and plays, at low frame rates. |
| Xclipse (Samsung Exynos 2x00), other Adreno | Untested; reports welcome. |

**Mali GPUs: early support.** Somniel starts them in the handheld profile at 30 fps, and the Graphics page has settings
that trade picture quality for speed. Requires Vulkan 1.1; Android 13 or newer is recommended.

---

## Features

- **Native execution**: the game's code runs at native speed, which is what makes 60 fps realistic on handhelds and
  lets mods that hook real machine code work.
- **Frame rate and speed**: 30, 40 or 60 fps, applied live. **Dynamic game speed** keeps walking and game time at
  normal speed when the device dips below the chosen rate, down to a 20 fps floor.
- **Render scale** 50-125 % in 5 % steps, applied live.
- **Graphics page**: Console mode (Auto / Handheld / Docked), Aspect ratio, Shadows, Anti-aliasing, Hidden object
  culling, Texture sharpness, Texture size and Shader precision.
- **Performance monitor**: FPS, frame-time graph, CPU/GPU temperatures and clocks.
- **On-screen controls** for phones, hidden automatically when a controller is connected. Layout, always on/off, and
  movement-control style are set in the sidebar. They fade out when idle.
- **Dual-screen companion**: on handhelds with a second screen, the bottom screen shows a live battle map, unit details
  and rosters (by igawa6, made for Eden Duo). Bundled, starts by itself, can be turned off.
- **Save import** (`.zip` or the save files; current saves are backed up first), **text entry** through an Android
  dialog, **custom GPU drivers**, **in-app updates**, and **proper sleep and wake** (lock the screen and the game
  parks itself with the CPU idle).
- **Hardware movie decoding**. Devices without a hardware VP9 decoder convert each movie once, in the background, to
  H.264.
- **Experimental Cobalt mod support**, see [Mods](#mods).

---

## Optimization work

Frame-time smoothness is the priority: the goal is fewer long frames, not just a higher average, and without simply
removing effects. What has been done:

**Shaders**
- The game's ~9,300 Switch shader stages are translated to SPIR-V on the device with a native translator, cached on
  disk, and all of them are prepared in the background on first boot, so later runs rarely compile mid-game.
- Pipelines needed by a frame are found by looking ahead through it and built together on several threads instead of
  one after another (first combat start on one Mali tablet: 602 ms down to 403 ms). Shaders used in earlier sessions
  are rebuilt at the title screen, so a scene you have seen starts without compiling.
- Shaders are translated for what each GPU supports (no clip distances on Mali, the GPU's own subgroup size).

**Stutter fixes**
- Walking the Somniel: shaders already prepared were being prepared again, with pauses of up to half a second; now no
  pause is longer than about 50 ms.
- Combat start: pauses of up to 200 ms on Adreno 740 are now about 60 ms.
- Mali frame pacing: frames used to arrive two at a time with a long gap; presenting no longer waits on the GPU, and a
  draw whose shader was late is no longer dropped (this caused the camera to judder back).
- Freeze fix when the game paused its threads for garbage collection, and a corrected game clock on devices whose
  timer is not 19.2 MHz.

**Throughput**
- Texture loading is about 1.5 times faster on every GPU.
- The unit-info 3D portrait follows the render scale; in the handheld profile the final passes only cover the visible
  picture; anti-aliasing can be turned off in every scene (about 2 fps in busy Mali scenes).
- **Hidden object culling** skips objects completely hidden behind walls: the Somniel shops went from about 25 to a
  steady 29-30 fps on a Dimensity 7300. It is off by default on Adreno, where it was slower.
- Guest threads run truly in parallel, per-call graphics timing is off by default (it cost about a fifth of the render
  thread), logging is kept light on slow devices, and Mali frame timings go to Android's performance hints.

---

## On-screen controls

A transparent gamepad is drawn over the game so it can be played on a phone. Sticks are "relative" (they re-centre
where your thumb lands), every button supports multi-touch and sliding between buttons, and the sidebar opens with the
Back gesture. Sidebar, **On-screen controls...**: always on or off, turn off when a controller is connected (on by
default; built-in handheld controllers count), D-pad and left stick / D-pad only / left stick only, and fade when idle.
Feedback on sizes and positions is welcome.

---

## Mods

Cobalt support is **experimental**.

**Getting Cobalt.** [Cobalt](https://github.com/Raytwo/Cobalt) is the mod loader by Raytwo that Somniel's mod support
is built on. It is **not included** (it has no licence that allows redistribution), so you fetch it yourself: accept the
first-start prompt, or use sidebar, **Mods**, **Download latest Cobalt** (or **Import Cobalt zip...** for offline use).
Cobalt loads when the game starts, so Somniel offers to restart the game after installing it.

**Using mods.**
- **Not all mods will work.** Asset-only (LayeredFS-style romfs) mods are the most likely to work. Mods with code
  plugins (`.nro`, exefs patches) depend on hooks that may not be implemented yet.
- **Do not install frame-rate or "60 FPS" mods.** Frame rate and speed are built in and will fight a second patch.
- Mod list changes take effect after a restart. If a mod misbehaves, turn it off and restart before reporting a bug.

---

## What to expect

- **There will be bugs**: rendering glitches on untested devices, a stutter the first time a scene's shaders are
  prepared, and crashes in less common parts of the game.
- Performance is best on the Snapdragon 8 Gen 2 devices above. Long sessions have had less testing than short ones.
- Save data lives in the app's own storage. Back it up before experimenting with mods or updating.

---

## Anonymous reporting (opt-in)

On first boot Somniel asks whether to share anonymous performance and crash data. It is **off until you say yes**, and
you can change your mind in the sidebar at any time (declining also deletes anything queued).

- **Shared:** a random install ID (resettable), app version, device model, chipset and GPU names, Android version, RAM,
  frame-rate and frame-time totals with a coarse reason for slow frames, and, after a crash, the faulting library and
  offset.
- **Never shared:** your name, accounts, IP address (the server does not store it), file paths, save data or game files.

---

## Reporting a bug

Open an issue on the [issue tracker](https://github.com/teton-dev/somniel/issues). Include your device model, Android
version, what you were doing, and `somniel.log` from `Android/data/dev.teton.somniel/files/`. Screenshots of rendering
glitches help a lot.

---

## Credits and licences

Somniel is free software, released under the [GNU General Public License v3.0](LICENSE). Bundled third-party code
keeps its own licence; the details are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Thanks to the authors of
the projects Somniel builds on:

- **Dual-screen companion:** the Fire Emblem Engage companion and the Eden Duo module SDK by
  [igawa6](https://github.com/igawa6) ([eden-duo-companions](https://github.com/igawa6/eden-duo-companions),
  [Eden Duo](https://github.com/igawa6/eden-duo), GPL-3.0). Bundled unmodified, with the companion's source in
  `third_party/eden_duo_companions`. Eden Duo is a fork of the [Eden](https://git.eden-emu.dev/eden-emu/eden) emulator.
- **Shader translator:** derived from the shader compiler of [Ryujinx](https://github.com/Ryujinx/Ryujinx) by the
  Ryujinx Team and contributors (MIT).
- **NVN notes and ASTC decoding:** [engage-pc-recomp](https://gitlab.com/engage-pc-recomp/engage-pc-recomp) by Valiantsin
  Dzerakh (MIT), whose ASTC decoder ports LAS Recomp's by Peiikai (MIT).
- **60 fps:** the frame-rate control follows the community "60 FPS (no Speedup)" patch by systemdev, zeusiota and
  dogethis ([GameBanana](https://gamebanana.com/mods/432758)).
- **[SDL3](https://www.libsdl.org/)** by Sam Lantinga and the SDL contributors (zlib).
- **[volk](https://github.com/zeux/volk)** by Arseny Kapoulkine (MIT).
- **[libadrenotools](https://github.com/bylaws/libadrenotools)** by Billy Laws (BSD-2-Clause).
- **[bcdec](https://github.com/iOrange/bcdec)** by Sergii Kudlai (MIT).
- **Companion build dependencies:** [LZ4](https://github.com/lz4/lz4) by Yann Collet (BSD-2-Clause), `bc_decoder` by
  the Skyline Team and the SwiftShader authors (MPL-2.0), and [stb_truetype](https://github.com/nothings/stb) by Sean
  Barrett (MIT or public domain).
- **Mods:** [Cobalt](https://github.com/Raytwo/Cobalt) by Raytwo, which you download separately.
- **App icon:** the placeholder icon uses the Fire Emblem series symbol artwork by Toomai Glittershine (SmashWiki) on
  Alear's blue and red.

Fire Emblem is a trademark of Nintendo; Somniel is an unofficial fan project and is not affiliated with or endorsed by
Nintendo or Intelligent Systems. Somniel and the companion are not affiliated with or endorsed by the Eden project.
