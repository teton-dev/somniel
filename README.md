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
   app; **do not uninstall first**, or you lose the game data and saves stored in the app.
3. Open Somniel and follow the first-run setup to point it at your own game files and keys.
4. The first launch asks whether to share anonymous data (see below), asks whether to download the Cobalt mod loader
   (optional, see [Mods](#mods)) and, on non-Adreno GPUs, shows a warning.

The first boot takes longer than later ones while the game's shaders are prepared in the background.

---

## Tested hardware

Somniel is built and tuned on the **Snapdragon 8 Gen 2 (Adreno 740)**, 8 GB and 12 GB models. Results so far:

| Chip (GPU) | Result |
| --- | --- |
| Snapdragon 8 Gen 2 (Adreno 740) | The main target: a mostly steady 60 fps. On dual-screen handhelds the second screen shows the dual-screen companion. |
| Dimensity 7300 (Mali-G615 MC2) | Playable at 30 fps in the handheld profile. The Somniel hub holds 29-30 fps, its shops included, with Hidden object culling (on by default on Mali). |
| Exynos 1330 (Mali-G68 MP2) | Boots and plays, but 3D scenes run well below 30 fps (about 18 fps at the 3D title menu). |
| Dimensity 6300 (Mali-G57 MC2) | Boots and plays, at low frame rates. |
| Xclipse (Samsung Exynos 2x00) | Untested. |

- **Mali GPUs: early support.** On a Mali GPU Somniel starts in the handheld profile at 30 fps, and the sidebar's
  Graphics page has settings that trade picture quality for speed. The first boot shows a notice about this.
- **Other Adreno GPUs and Snapdragon chips are untested.** They may work, and reports are welcome.
- Requires Vulkan 1.1; Android 13 or newer is recommended.

---

## More about Somniel

- **Native execution.** The game's code runs at native speed rather than through an emulator or recompiler, which is
  what makes mods that hook real machine code possible, and what makes 60 fps realistic on handheld hardware.
- **Optimized rendering and threading.** Shaders are translated on the device the first time they are used and cached
  (and all of the game's shaders are prepared in the background on first boot, so later runs hitch less). The
  renderer replays the game's graphics commands on Vulkan and the game's own threads run truly in parallel.
- **Proper sleep and wake.** Lock the screen or switch apps and the game parks itself: guest threads pause, audio
  pauses and the CPU is idle. Unlock and it comes back where you left it.
- **Built-in 60 fps** (no speed-up). The frame-rate patch is built in and selectable live. See the sidebar below.
- **Hardware movie decoding.** The opening movie and cutscene video go through Android's hardware VP9 decoder. On
  devices without one, Somniel converts each movie once in the background to H.264 and plays that on the hardware
  decoder instead.
- **On-screen controls** for phones, hidden automatically when a controller is connected. See
  [On-screen controls](#on-screen-controls).
- **Text entry.** When the game asks for text (your name in New Game, for example) Somniel shows an Android text
  dialog, so you can type it with the on-screen or a physical keyboard.
- **In-game sidebar**, opened with Back (or the system back gesture), covering:
  - **Accurate frame-rate control:** 30, 40 or 60 FPS, applied immediately without restarting.
  - **Render scale:** 50% to 125% of the game's 1728x972 in 5% steps, applied live.
  - **Performance monitor:** FPS, frame times with a graph, CPU/GPU temperatures and clocks.
  - **Graphics page:** Console mode (Auto / Handheld / Docked), Aspect ratio (Stretch to fill / 16:9), Shadows,
    Anti-aliasing, Texture sharpness, Texture size and Shader precision.
  - **On-screen controls page:** turn the touch controls on or off, choose which movement controls show, and more.
  - **Mods page:** download or import the Cobalt mod loader, import `.zip` or `.7z` mod archives, switch mod folders
    on and off, restart to apply.
- **Experimental Cobalt mod support.** Cobalt is not included with Somniel, but the Mods page can download the latest
  release for you in one tap, and its plugin (`.nro`) loader is implemented natively. See [Mods](#mods).
- **Custom GPU drivers:** import a driver package.
- **Dual-screen companion.** On handhelds with a second screen, the bottom screen shows a live touch companion (battle
  map, unit details and rosters) by igawa6, made for Eden Duo. It is bundled and starts by itself; the sidebar can turn
  it off.
- **Save import.** Sidebar, **Import saves...**: pick a `.zip` or the save files themselves (`Global`, `Auto`,
  `Manual0`...). Your current saves are first backed up to `Documents/Engage-Backup`, then replaced.

---

## On-screen controls

Somniel draws a transparent gamepad over the game so it can be played on a phone without a controller. The layout
follows the common console pattern, with the buttons you press most closest to your thumbs:

- **Left side:** the left stick (lower left) and the D-pad.
- **Right side:** A (the largest), B, X and Y in the lower right, and the right stick above them.
- **Top corners:** L and ZL on the left, R and ZR on the right.
- **Bottom centre:** Minus and Plus.

The sticks are "relative": they re-centre wherever your thumb lands, so you do not have to find them. The D-pad is
4-way, so a thumb drifting off an arm never presses two directions at once, and every button supports multi-touch and
sliding from one button to the next. The sidebar opens with the Back gesture.

**Fading.** After about four seconds without a touch the controls fade to almost invisible so they do not cover the
game, and they come back at your next touch. A touch while they are faint still counts.

**Customising them** (sidebar, **On-screen controls...**):

- **Always on** or **Always off**.
- **Turn off when a controller is connected.** On by default. Built-in controllers on handhelds such as the Retroid
  Pocket count as a controller, so the overlay stays out of the way there; untick it to show the controls anyway.
- **Movement controls:** D-pad and left stick, **D-pad only** (drawn larger, in the left stick's place) or **left stick
  only**. The right stick is always shown.
- **Fade when idle** on or off.

The on-screen controls have so far only been tried on a handheld, so the sizes and positions may need tuning on real
phones. Feedback is welcome.

---

## Mods

Cobalt support is **experimental**.

**Getting Cobalt.** [Cobalt](https://github.com/Raytwo/Cobalt) is the mod loader by Raytwo that Somniel's mod support
is built on. It is **not included with Somniel** (it has no licence that allows redistribution), so you fetch it
yourself, and Somniel makes that as easy as it can:

- The first time you start Somniel without it, a prompt offers to **Download** the latest release, to **Import file...**
  if you already have a Cobalt `release.zip`, or **Not now**.
- Any time later: sidebar, **Mods**, then **Download latest Cobalt** (it becomes **Update Cobalt** once installed) or
  **Import Cobalt zip...** for offline use. The download comes straight from the project's GitHub releases page.
- Cobalt is loaded when the game starts, so Somniel offers to restart the game after installing it.

**Using mods.**

- **Not all mods will work.** Mods that only replace game assets (LayeredFS-style romfs mods) are the most likely to
  work. Mods that ship code plugins (`.nro` or exefs patches) depend on hooks that may not be implemented yet, and some
  will do nothing or crash.
- **Do not install frame-rate or "60 FPS" mods.** 60 fps is built in and is controlled from the sidebar. A second
  patch on top of it will fight the built-in one and can cause crashes or wrong game speed.
- Changes to the mod list take effect after a restart, because Cobalt reads mods once at boot.
- If a mod misbehaves, turn it off in the Mods page and restart before reporting a bug against Somniel.

---

## What to expect

- **There will be bugs.** This is early software. It's very likely there are still rendering glitches on devices I have not tested yet, occasional stutter while shaders are first translated, and crashes in less common
  parts of the game.
- Performance is best on the Snapdragon 8 Gen 2 devices listed above. Long play sessions have had less testing than
  short ones.
- Save data lives in the app's own storage. Back it up before experimenting with mods or updating the app.
- Updating the app may require first-boot work again (such as preparing shaders) and may need a fresh game-data push.

---

## Anonymous reporting (opt-in)

On first boot Somniel asks whether you want to share anonymous performance and crash data. It is **off until you say
yes**, and you can change your mind in the sidebar at any time (declining also deletes anything queued).

- **Shared:** a random install ID (resettable), app version, device model, chipset and GPU names, Android version,
  RAM size, frame-rate totals, and, after a crash, the faulting library and offset.
- **Never shared:** your name, accounts, IP address (the server does not store it), file paths, save data or game files.

The data is used to find out which devices work and where it crashes.

---

## Reporting a bug

Open an issue on the [issue tracker](https://github.com/teton-dev/somniel/issues). Please include your device model, Android version, what you were doing, and `somniel.log` from the app's `files`
folder (`Android/data/dev.teton.somniel/files/`). Screenshots of rendering glitches help a lot.

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
