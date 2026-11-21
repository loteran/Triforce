<p align="center"><a href="https://ko-fi.com/W7W31VXIVC"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="ko-fi"></a></p>

> [!NOTE]
> **Unofficial fork with Linux additions** (Linux AppImage, AMD FSR 4 on Linux, vendor frame generation). Upstream: [git.triforceproject.com/triforce/source](https://git.triforceproject.com/triforce/source). This is where the [NX2navi Linux AppImage](https://github.com/loteran/NX2navi) work continues.

<p align="center">
  <img src="assets/triforce.png" alt="Triforce" width="230">
</p>

<p align="center">
  <img src="assets/readme/title.svg" alt="TRIFORCE" width="480">
</p>

<p align="center">
  <b>Your game, native on your PC.</b><br>
  <sub>No emulator: Triforce builds a real Windows program on your PC, from your own copy of the game, and draws its graphics on Vulkan.</sub>
</p>

<p align="center">
  <a href="https://triforceproject.com"><img src="https://img.shields.io/badge/Download-Triforce%20Setup-D4A72C?style=for-the-badge&labelColor=111111" alt="Download Triforce Setup"></a>
  <img src="https://img.shields.io/badge/Windows-10%20%7C%2011-D4A72C?style=for-the-badge&labelColor=111111" alt="Windows 10 | 11">
  <img src="https://img.shields.io/badge/Renderer-Vulkan%201.3-1E62E8?style=for-the-badge&labelColor=111111" alt="Vulkan 1.3">
  <img src="https://img.shields.io/badge/Native-No%20emulator-2FAA3A?style=for-the-badge&labelColor=111111" alt="No emulator">
  <img src="https://img.shields.io/badge/License-GPL--3.0%2B-C8202E?style=for-the-badge&labelColor=111111" alt="GPL-3.0+">
</p>

<p align="center">
  <a href="#the-three-pieces">About</a> &nbsp;◆&nbsp;
  <a href="#installing-triforce">Install</a> &nbsp;◆&nbsp;
  <a href="#every-time-you-play">Playing</a> &nbsp;◆&nbsp;
  <a href="#before-you-play">Settings</a> &nbsp;◆&nbsp;
  <a href="#controls">Controls</a> &nbsp;◆&nbsp;
  <a href="#convert-saves">Saves</a> &nbsp;◆&nbsp;
  <a href="#mods">Mods</a> &nbsp;◆&nbsp;
  <a href="#under-the-surface">How it works</a> &nbsp;◆&nbsp;
  <a href="#the-sages">Credits</a>
</p>

<p align="center"><img src="assets/readme/divider.svg" alt="" width="100%"></p>

## <img src="assets/readme/gold.svg" alt="" width="26"> The three pieces

**Triforce** is a native PC version of your game. Every piece has one job: to make it look and play on your PC exactly
the way it does on the console.

| | Piece | What it does |
|:-:|---|---|
| <img src="assets/readme/ruby.svg" alt="" width="20"> | **Power** · the recompiler | Turns the game's own ARM64 code into a Windows program ahead of time. No emulation at runtime, no CPU guessing. |
| <img src="assets/readme/sapphire.svg" alt="" width="20"> | **Wisdom** · the system | Plays the part of the console's operating system: files, saves, audio, controllers, threads and timing. |
| <img src="assets/readme/emerald.svg" alt="" width="20"> | **Courage** · the renderer | Rebuilds the console's graphics on Vulkan, with the game's own lighting, fog, water and foliage. |

What makes the game look right in Triforce:

- <img src="assets/readme/emerald.svg" alt="" width="16"> **Living forest:** trees with full canopies, ferns, grass,
  drifting leaves and fireflies, all drawn by the game's own GPU culling.
- <img src="assets/readme/sapphire.svg" alt="" width="16"> **True light:** ambient light probes keep the hero, signs and
  plants lit in the shade; the pond's water and the game's own fog are as the console shows them, with no tuned
  workarounds on top.
- <img src="assets/readme/ruby.svg" alt="" width="16"> **Steady play:** no freeze after loading a save, no exposure
  flicker, and GPU data that survives memory pools being rebuilt.
- <img src="assets/readme/gold.svg" alt="" width="16"> **Triforce Setup:** one small download that installs, builds,
  updates and launches the game, with your settings, controls and saves handled before the game even starts.

> [!IMPORTANT]
> **Bring your own game.** Triforce contains no game data, no keys and no ready-made executable, and it won't tell you
> how to get them. Everything is built **on your PC, from your own copy**, and everything it makes (the program, the
> generated code, shader caches and saves) stays yours. Don't share those files.

<p align="center"><img src="assets/readme/divider.svg" alt="" width="100%"></p>

## <img src="assets/readme/emerald.svg" alt="" width="26"> Installing Triforce

*Every hero starts in the forest.* You download one small program, **Triforce Setup**, and it does the rest: the game
itself is assembled on your PC, from your copy, in one guided build.

### Before you start

| You need | Details |
|---|---|
| <img src="assets/readme/gold.svg" alt="" width="18"> Windows | 10 or 11, 64-bit. Nothing else to install: Setup runs on what Windows already has |
| <img src="assets/readme/sapphire.svg" alt="" width="18"> Graphics card | Vulkan 1.3 with mesh shaders, on an up-to-date driver (tested on NVIDIA) |
| <img src="assets/readme/ruby.svg" alt="" width="18"> Memory | 16 GB of RAM or more (the game uses about 10 GB) |
| <img src="assets/readme/emerald.svg" alt="" width="18"> Disk space | About 2 GB for Setup and its tools, and about 60 GB free while building |
| <img src="assets/readme/gold.svg" alt="" width="18"> Your game | Program `0400486024DEC000`, update v131072, build `e0f3ccfa`, as a `.nup` or as `romfs` + `exefs` folders. Other versions are refused. |

### Four steps to play

**1 · Download.** Get **`TriforceSetup.exe`** from the [newest release](https://triforceproject.com)
and run it.

**2 · Install.** Choose where Triforce goes (your own user folder by default, so no administrator prompt) and whether
you want it on the desktop and in the Start menu. Setup then fetches the Triforce tools (about 250 MB, once).

**3 · Forge your game.** Point Setup at your game, either the `.nup` or the `romfs` and `exefs` folders; it checks the
version first. Pick a language and a folder for the game, then press **Build**. The first build takes around
**40 minutes**, and your PC stays usable the whole time.

**4 · Play.** Press **Play**. Settings and controls are in the game's own **Options** screen.

From then on, the **Triforce** shortcut on your desktop and in the Start menu opens Setup first: that's where updates and saves live.

### Uninstalling

Remove **Triforce** from Windows' *Settings → Apps*. That takes Setup, its tools, its `save-backups` folder and the build's
work folder (`%LOCALAPPDATA%\Triforce\build`) away; your game folder and its saves stay until you delete them yourself.

<p align="center"><img src="assets/readme/divider.svg" alt="" width="100%"></p>

## <img src="assets/readme/gold.svg" alt="" width="26"> Every time you play

Opening Triforce shows the **Welcome back** screen first: your game, your saves and the newest Triforce side by side.

- **A new version is out:** choose **Update & merge** to bring it into your game, or **Not now** to keep playing what you
  have; you'll be asked again next time. Updating downloads the new release and builds your game folder again from the
  game files it keeps. Only what changed is compiled again, so it usually takes a few minutes. **Your saves and
  settings stay.** Setup keeps itself up to date the same way.
- **You're up to date:** press **Continue**.
- **No internet, or the Triforce site is down:** Setup says so, and you play what you have.

Beside **Your game** on that screen are **Mods** (see [Mods](#mods)) and **Add to Steam**. Add to Steam puts the game
into your Steam library as a non-Steam game, with Triforce's own crest as its pictures; from there Steam starts the
game itself, without Setup. **Close Steam first:** Steam writes its own list of shortcuts when it closes, so Setup
changes nothing while Steam runs. Your other shortcuts stay as they are, the file as it was is kept beside the new one
(`shortcuts.vdf.triforce-backup`), and the same link takes the game out of Steam again.

**Sounds.** Once your game is built, Setup plays two of the game's own menu sounds: the decide sound when you choose a
button, a link or a row, and the cancel sound when you go back. They are read from your own game folder each time
Setup starts (a mod that replaces one of them is heard here as in the game) and kept in memory only: **no sound comes
with Setup**, so before a game is built it is silent. Moving the mouse makes no sound, and Setup stays quiet while
the game is running. **Sounds**, at the top of the window beside the minimize button, goes through **Off**, **Low**
(the start), **Medium** and **High** at each click.

Then come your settings, and **Start game**.

<p align="center"><img src="assets/readme/divider.svg" alt="" width="100%"></p>

## <img src="assets/readme/sapphire.svg" alt="" width="26"> Settings

*Choose your path.* Settings live in the game's own **Options** screen (title screen or pause menu): the tabs Display,
Graphics, Audio, Controls and System sit beside the game's own list, and **L** / **R** switch between them. Each tab's
settings come in groups under the game's own separator: Display has Screen, Ultrawide, HDR, RenoDX and Overlay; Graphics
has Upscaling, Anti-aliasing, Frame generation, Frame rate & latency and Textures. A setting that does nothing with the
others as they are is not shown (Paper white with HDR off, the DLSS model without DLSS). A few apply at the next start;
the game asks. They're saved in `triforce_options.ini` beside the game.

| Setting | Options |
|---|---|
| Fullscreen | Off, On |
| HDR | Off, Expanded, Native (the game's own HDR), Native + RenoDX, on an HDR display; with HDR on, Paper white 80 to 400 nits and Peak brightness 400 to 10,000 nits. Native + RenoDX applies at the next start (Off, Expanded and Native change at once) |
| RenoDX | With HDR Native + RenoDX: RenoDX's tone mapping in place of the game's own, and its rows. UI brightness (the HUD's and the menus' white); Tonemapper Vanilla, PsychoV35 or RenoDRT; Gamma correction Off, 2.2 or BT.1886 (2.4); Color space, Exposure, Middle gray, Highlights, Shadows, Contrast, Saturation and Bloom. With PsychoV35 also Match tone slope, Tone slope and Hue clip; with RenoDRT Highlight saturation, Blowout, Flare, Hue correction and Color grading |
| Frame generation | Off, Auto, 2x up to 8x; AMD FSR (one frame made by AMD's frame generation library between each two of the game's, the HUD kept where the game drew it) where your card, driver and build have AMD's frame generation, shown with its version: AMD FSR (3.1.6) on a Radeon RX 6000 or 7000, FSR 4's on an RX 9000. Paused with HDR Native or RenoDX (the renderer's own makes the one frame there, as at 2x); HDR Expanded works; NVIDIA DLSS 2x, 3x, 4x on an NVIDIA RTX card with a build that has NVIDIA's Streamline (an RTX 40 or 50 runs it; applies at the next start) |
| Frame generation engine | Auto, NVIDIA DLSS-G, AMD (library), Triforce: who makes the made frames. Auto picks the best that runs on your PC; a choice that cannot run here is skipped |
| VSync | On, Off: Off shows each frame at once, with a little less delay and possible tearing; the frame rate stays the cap's |
| Frame pacing | Off, Even, Steady (any rate), Steady (refresh steps). Even (the default): each frame is shown at an even interval, for less stutter at up to one frame of delay. Steady applies at the next start: the game is paced to a frame rate your PC holds where you are, never above the cap, and each frame is shown a period after the last: the frames come evenly, for some frame rate given up (on the lab's recorded Market frame times about 74 to 81 fps where it ran at 93 to 95, with the frame-to-frame jitter halved). The rate is measured by letting about 30 frames run unpaced every 12 s or so (and right away where frames run late), and it goes down within a second of a heavier scene and back up within one such probe of a lighter one; loads and single hitches do not move it. Any rate suits a screen with variable refresh (G-SYNC, FreeSync) or VSync off; refresh steps keeps to whole divisions of the refresh rate (at 240 Hz: 240, 120, 80, 60, 48, 40, 30) for VSync without variable refresh. The overlay's Full view shows the rate it holds. Where RivaTuner or Reflex limits the game, the cap holds as before. `HOS_FPS_STEADY=0`, `1` or `2` in `triforce_env.txt` over the row |
| Low latency | Off, On, On + Boost: NVIDIA Reflex on an NVIDIA card (Boost keeps the card's clocks up), AMD Anti-Lag 2 on an AMD one (Off, On); not shown where neither runs. With DLSS frame generation Reflex is always on, and it holds the frame rate cap where each frame starts in place of the game's own wait, so a frame waits once (where RivaTuner limits the game, RivaTuner does) |
| Frame rate cap | 30, 40, 45, 60, 90, 120, 144, 165, 240, Monitor's rate (your screen's refresh rate, up to 240). Past 60 is experimental; applies at the next start. Where RivaTuner Statistics Server is installed and running (Windows), the game puts this cap into RivaTuner's profile for `Triforce.exe` and RivaTuner limits the game instead of the game's own wait, once it is seen holding the game to it; with frame generation 2x to 8x or AMD FSR 4 the limit is the cap times the frames shown for each. Closing the game puts the profile back as it was, and after a crash the next start does (`triforce_rtss_profile.txt` beside the game keeps what it held). The game never starts RivaTuner. Changing that profile needs the game run as administrator (RivaTuner keeps its profiles under Program Files); without it a limit you set in RivaTuner yourself, at or under the cap, holds the game alone. With frame generation on Auto or DLSS frame generation, while the window is minimised and during movies (when the limit is the game's) the game limits itself. The overlay's Full view says who limits and why. `HOS_RTSS=0` in `triforce_env.txt` leaves RivaTuner out entirely |
| Upscaler | Game, NVIDIA DLSS, AMD FSR 3.1, AMD FSR 4 (named by the version AMD's library runs, such as AMD FSR 4.1): those your card and build can run |
| Render resolution | Auto (the game's dynamic resolution), 100%, 75%, 67%, 50% with the game's own upscaler; with DLSS and FSR Auto, Native (100%: DLAA, or FSR's native anti-aliasing), Quality, Balanced, Performance, Ultra Performance. Applies at the next start |
| Anti-aliasing | Game SMAA, Off, FXAA, SMAA Ultra: how the 3D's edges are smoothed with the game's own upscaler. DLSS and FSR smooth them themselves, from frames the game's SMAA has not been over, and the row is then not shown |
| Sharpness | 0–100% (0, off, by default; around 50% suits DLSS and FSR): with DLSS or AMD FSR as the Upscaler the 3D before the HUD, with the game's own upscaler the picture at the window |
| FSR sharpening | Off, 5–100%: AMD's own sharpening inside FSR 4 and FSR 3.1 through AMD's library, apart from Sharpness |
| Texture filtering | Game, 2x, 4x, 8x, 16x anisotropic |
| Texture detail | Automatic, Full, Reduced, Low: how large the game's textures are kept in video memory. Automatic goes by your card; applies at the next start |
| Fast pipeline link | Off, On (the Debug tab; on by default): new pipelines are first linked from prepared shader libraries, so fewer draws wait for the graphics driver. The first start with it compiles those libraries once; applies at the next start |
| Replay helper threads | Automatic, Off, 2 to 8 threads (the Debug tab): more of your processor's cores prepare each frame for the graphics card, where short trials as you play find that faster, or at once where the frame plainly waits for that work (and no trial where it cannot help). Automatic uses 2 or 3 by your processor, none with four cores or fewer; with DLSS or FSR on too, for upscaling or frame generation. Choose Off if frames flicker; applies at the next start |
| Lazy write-back | Off, On (the Debug tab; on by default, Windows only): what the GPU writes into memory the game shares with it is copied back only when something reads it there, not after every submission. Stays off with an AMD card's driver unless `RX_VK_LAZY_WB=1` is set; applies at the next start |
| Start next frame early | Off, On (the Debug tab; on by default; experimental): the game starts its next frame while the graphics card still draws the end of this one (the upscaling and the interface), where that end holds nothing the game reads back; other frames run as before. Applies at the next start (`RX_VK_TAIL_EARLY=1` or `=0` in `triforce_env.txt` over it) |
| Opaque textures as BC1 | Off, On (the Debug tab): the game's large textures that have no transparency kept in half the video memory, with the same picture. On by default; applies at the next start |
| Game language | The game's 16 languages, as `language.txt` beside the game lists them; applies at the next start |
| Game speed | Real time (keeps normal pace), Per frame (slows down below 60 fps) |
| Output resolution | 1536x864 (handheld: the game's handheld profile, as if the console were undocked), 1920x1080, 2048x1152 (the game's own), 2560x1440, 3840x2160, 5120x1440, 5120x2160, 5120x2880; applies at the next start. The finished picture is scaled to the window by AMD FSR 1 when it is enlarged (sharpened only by the Sharpness row) and by the plain copy otherwise, and always by the plain copy with HDR Native, which keeps its 10 bits (`RX_VK_UPSCALE=<0..3>` in `triforce_env.txt` fixes the filter) |
| Aspect ratio | 16:9 (the game's own), 16:10, 21:9, 32:9: the fixed shapes are for a screen or a window of that shape (a screen of another shape shows black bars); applies at the next start. Wider than 16:9, HUD position and Menu sides show too |
| FPS counter | Off, On, Full: On is the frame rate in the top right corner; Full shows NX2navi's performance overlay there in its place (under the options while they are open), in the options' look and see-through: the build, the GPU's and the CPU's load and temperature, video memory and memory, the frame rate with its 1% and 0.1% lows, the frame times and their graph, the GPU's and the replay's time and waits per frame, the picture's settings and the GPU's and CPU's names (a "-" where the PC does not say: temperature and power need an NVIDIA or AMD card). It measures only while shown |
| Menu sounds | Off, 5% to 100% (80% unless you change it): the game's own interface sounds as you move through these settings, the tabs and the rows, read from your game files (a mod's when a mod replaces them). Under Master volume on the Audio tab; changes at once |
| Controller in background | Off, On: the pad keeps playing while another window has focus |
| Left stick dead zone, Right stick dead zone | Default (24%), or 0% to 50%: how far that stick moves before the game sees it |
| Rumble strength | Off, 25% to 300% of the game's rumble |
| Vibration curve | 0.20 to 1.50: below 1.00 the game's many weak rumbles are lifted so you can feel them; 1.00 is the game's |
| Trigger vibration | Off, On: an Xbox One or Series controller's triggers rumble too |
| Walk key | Off, Caps Lock, Shift, Ctrl, Tab: hold it with the movement keys and Link walks instead of running |
| Walk scale | 0.20 to 0.90: how far the movement keys push the stick while the Walk key is held |
| Button prompts | Auto, Switch, Xbox, PlayStation 4, PlayStation 5, Steam Deck, Keyboard & mouse: the button icons the game shows (Controls page). Auto takes the controller connected when the game starts, or with none the device you used last in the game before (the keyboard and mouse the first time); Keyboard & mouse shows the key or mouse button each control is bound to. Applies at the next start |
| Text speed | Normal, 2x, 4x, Instant |
| Export save | Your save as a zip in your Downloads folder (System tab; see *Export and import a save* below) |
| Export save for a bug report | The same zip with the logs and settings a problem report needs |
| Import a save | A save export of Triforce or NX2navi becomes your save at the next start |
| Bug capture (F8) | One folder with what a problem report needs (System tab; see *Bug capture* below) |
| Scan amiibo | A dump of one of your own figures from a folder `amiibo` beside the game (`.bin`, as the figure's tag has it: 532, 540 or 572 bytes), put on the reader when the game looks for a figure. Only the figure's serial number and which figure it is are read from the file: no key is needed. On the Debug tab (with Debug tools On) |
| Game flags | The game's story and world flags by name, read from your game files: see which are set in your save, and set or clear one (Debug tab) |
| Cutscenes | The game's events by name, read from your game files: start one (Debug tab) |
| Mod cheats | Off, On for each cheat of a mod's cheat file, under its own name (System tab, only with such a mod; see *Mods* below) |

In game, **F10** or **Alt+Enter** switches between a window and fullscreen. **F1** opens the same options from anywhere.
**F8** takes a bug capture.

<p align="center"><img src="assets/readme/divider.svg" alt="" width="100%"></p>

## <img src="assets/readme/ruby.svg" alt="" width="26"> Controls

Play with a controller, or with keyboard and mouse. The **Controls** tab of the game's Options sets what presses
each of the console's buttons: one controller input and up to two keys.

Moving (<kbd>W</kbd> <kbd>A</kbd> <kbd>S</kbd> <kbd>D</kbd>) and the camera (arrow keys, or <kbd>I</kbd> <kbd>J</kbd>
<kbd>K</kbd> <kbd>L</kbd>) are fixed.

<p align="center"><img src="assets/readme/divider.svg" alt="" width="100%"></p>

## <img src="assets/readme/sapphire.svg" alt="" width="26"> Convert saves

Coming from NX2navi or an emulator? Your journey comes with you. **Convert saves** on the Welcome back screen brings
your NX2navi saves into Triforce, and moves saves of this game between Triforce and an emulator in either direction:

- **Into Triforce:** Setup looks for your saves by itself (an NX2navi game on this PC; Eden, yuzu, suyu, Sudachi,
  Citron, torzu and Ryujinx, and portable copies started from a desktop or Start menu shortcut), or you choose the
  folder.
- **Out of Triforce:** your Triforce saves go back to an emulator the same way.

Every slot comes across as it is. Whatever gets replaced is backed up first, in the `save-backups` folder beside Setup.
Close the game and the emulator before converting.

**On Linux**, Triforce keeps its saves in `~/.local/share/hookshot/save/5a3c9e1f0b7d24680000000000000001` (under
`$XDG_DATA_HOME`). The builder's **Import saves…** link (beside Play, and on the launcher) lists what it finds: Eden,
yuzu, suyu, Sudachi, Citron and torzu (also their flatpak data), Ryujinx (also flatpak), and NX2navi game folders
(under `~/Games`, your install folder and its parent; a finished commit in `profile/save-tx` counts when newer).
**Other folder…** points at any other place, such as an NX2navi game folder. Importing is one way, into Triforce: the
saves it replaces are copied to `~/.local/share/hookshot/save-backup/<date and time>` first, and it refuses to run
while the game or an emulator is open.

<p align="center"><img src="assets/readme/divider.svg" alt="" width="100%"></p>

## <img src="assets/readme/emerald.svg" alt="" width="26"> Export and import a save

The **System** tab of the Options has three rows for your save. Each says under its help what it did and where the
file went.

| Row | What it does |
|---|---|
| **Export save** | Writes your save as `Triforce-save-<date>-<time>.zip` into your Downloads folder (beside the game when that folder can't be written): to keep, or to give to someone |
| **Export save for a bug report** | The same zip, its name ending in `-bug-report`, with `triforce_log.txt`, the log of the run before, `triforce_options.ini` and `triforce_hang.txt` added |
| **Import a save** | Takes such a zip, Triforce's or NX2navi's, from the `save-import` folder beside the game |

- The zip holds the save as the game last saved it, never one half written, and **never a game file**.
- **To import:** put the zip into the game folder's `save-import` folder (the row makes the folder the first time) and
  choose the row. It becomes your save **at the next start**; choose the row again before that to cancel. The save it
  replaces is kept in `user\save-backup\<id>\import-<date>-<time>`, and the zip moves to `save-import\imported`.
- A zip that is not a save export of Triforce or NX2navi, or is damaged, is refused and nothing changes.
- An emulator's save is a folder, not such a zip: use **Convert saves** above.

<p align="center"><img src="assets/readme/divider.svg" alt="" width="100%"></p>

## <img src="assets/readme/ruby.svg" alt="" width="26"> Bug capture

When you see something wrong, press **F8** in the game's window (or choose **Bug capture (F8)** on the System tab). The
game goes on; a moment later a line at the foot of the screen names the folder that was written,
`captures\<date>-<time>` in the game folder:

| File | What |
|---|---|
| `frame.png` | The frame shown after the press, without the options or the FPS counter over it (an HDR picture brought to sRGB). Uncompressed: zip the folder to send it |
| `capture.txt` | The Triforce version, the graphics card and driver as the log names them, the frame rate before the press, and the map and Link's position while he is in play |
| `log-tail.txt` | The last 400 lines of `triforce_log.txt` |
| `triforce_options.ini` | Your settings |
| `Triforce-save-<date>-<time>.zip` | Your save, as **Export save** writes it |

Send the folder with your report and say what you saw. Nothing leaves your PC by itself, and **no game file is in a
capture**; delete old ones whenever you like. A press while a capture is still being written is skipped. If you bound
F8 to one of the controls yourself, the key stays the game's and the row takes the capture.

<p align="center"><img src="assets/readme/divider.svg" alt="" width="100%"></p>

## <img src="assets/readme/gold.svg" alt="" width="26"> If something goes wrong

| What happens | What to try |
|---|---|
| Setup says the build isn't supported | Check you chose the right game, with its update installed |
| Setup says the Triforce site could not be reached | Check your connection; you can still play what you have |
| A download stops halfway | Press **Try again**: it goes on from where it stopped |
| Setup says the new Setup could not start | It has put the Setup you had back in its place: press **Try again** to go on with it, and tell us in the Discord |
| The build stops | Press **Copy report** on that page and paste it into the Discord: it holds the build's last lines and Setup's log, without your user name |
| Setup says there is no room on a disk | Make room on the disk it names (it says how much it takes and how much is free), then press **Try again** |
| The game opens and closes straight away | Update your graphics driver |
| A short hitch the first time something appears | Expected for now: shaders are being prepared, and the same spot is smooth next time |
| The wrong language | Choose it in Options, System, Game language (or edit the code in `language.txt` beside the game; the file lists them all) |

Still stuck? Tell us in the [Triforce Discord](https://discord.gg/triforce): where you were in the game, your graphics
card, a screenshot or clip, and `triforce_log.txt` from the game folder. A bug capture (**F8**, above) has most of
that in one folder. **Never attach game files.**

<p align="center"><img src="assets/readme/divider.svg" alt="" width="100%"></p>

## <img src="assets/readme/emerald.svg" alt="" width="26"> Mods

Make a `mods` folder beside `Triforce.exe` and drop mods in with the usual layout, one folder a mod:

| In the mod's folder | What it is | When it takes effect |
|---|---|---|
| `romfs/...` | Files that replace or add game files | At the next start |
| `exefs/*.ips`, `exefs/*.pchtxt` | Patches of the game's code | At the next **build** of the game |
| `cheats/<build id>.txt` | A cheat file, as emulators take it | At the next start; each cheat is switched in the Options |

Deeper layouts such as `mods/contents/<title id>/<name>/romfs` work too: any folder up to four levels down that holds
a `romfs`, `exefs` or `cheats` folder is a mod.

- Start a folder's name with `-` or `_` to switch that mod off.
- Mods are taken in the order of their folder names, and where two change the same thing the name that sorts last
  wins: number them `01-…`, `02-…` to set the order.
- `triforce_log.txt` lists every mod that loaded, the files it supplied, its code patches and its cheats.

**Code patches.** The game's code is compiled ahead of time, so a patch of it is built in rather than loaded: put the
mod in, then build the game again into the same folder with Triforce Setup (from a clone:
`nx2/make_game.py --update <game folder>`). An update of the game builds them in again by itself. The build applies
the patches to its own copy of the game's code, never to your game files, says which it applied, and translates the
game again only when the patches changed.

- An `.ips` file is named after the build id of the code it patches (IPS or IPS32); a `.pchtxt` file names it in its
  `@nsobid-` line. A patch for another build of the game is refused, and the build says so.
- A patch file is applied whole or not at all. One that writes outside the game's code and data, a code cave among
  them (new code put into the empty space after the game's own), is refused.
- Taking a mod out or switching it off does not take its patch out of the game: build again. The game's log says at
  each start which patches are built in and which wait for a build, and `triforce_code_mods.txt` in the game folder
  lists what the last build applied.
- If a build stops in *Translating* or the game misbehaves with a patch, switch the mod off and build again.

**Cheats.** A cheat file is named after the first 16 digits of the game's build id (`E0F3CCFAF50065AE.txt` for the
supported game); one for another build is not read. Each of its cheats gets a row under **Mod cheats** on the System
tab of the Options, with the file's own name. A cheat is off until you switch it on, and one you switched on stays on
at the next start (`triforce_cheats.txt` in the game folder keeps the list; delete it to start with all off). A
master code (`{name}`) always runs.

- Cheats run once every frame of the game and can change the game's memory only: its heap and its data.
- A cheat that rewrites the game's instructions cannot work here and says so on its row: make it a code patch.
- A cheat with an opcode Triforce does not know is not run at all; its row and the log name the opcode.
- The first 96 cheats have a row.

**The Mods page.** Setup does the same for you, on the same folders: **Mods**, beside *Your game* on the Welcome back
screen, lists what is in the `mods` folder, what each mod carries, and which mod wins the files two of them share.

- **Add a mod** takes a `.zip`, `.7z` or `.rar` file, or drop one onto the page. It is unpacked inside the `mods`
  folder, checked, and the folder that holds `romfs` is put in place under the mod's name. (`.7z` and `.rar` are opened
  with Windows' own `tar.exe`; where a PC has none, Setup says so and you unpack the archive yourself.) An archive
  without a `romfs`, `exefs` or `cheats` folder, with several mods to choose from, or with anything that would land
  outside the `mods` folder is refused, with the reason.
- **Switch off** and **Switch on** put the `-` in front of the folder's name or take it away.
- The **arrows** move a mod earlier or later by numbering the folders `01-…`, `02-…`; the mod's own name stays.
- **Remove** moves the mod's folder into `mods\.removed`, which the game does not look at. Nothing is deleted: empty
  that folder yourself when you are sure.
- Nothing is changed while the game is running: close it first.
- RomFS mods and cheats take effect at the game's next start. Where a mod carries code patches, which are built into
  the game, the page says **Build again to apply** after such a mod was added, removed or switched.

<p align="center"><img src="assets/readme/divider.svg" alt="" width="100%"></p>

## <img src="assets/readme/sapphire.svg" alt="" width="26"> Under the surface

*Zora wisdom: what happens beneath the water.*

1. **Translate:** `nx2/lift.py` turns the game's ARM64 machine code into C.
2. **Compile:** `nx2/build.py` builds that C together with `host/`, which stands in for the console's operating system.
3. **Replay:** `host/nvn_native.c` captures every NVN2 graphics call the game makes, and `gpu/vk/` replays it on Vulkan.
4. **Shaders:** the console's Ampere shaders are lowered to Maxwell (`lower_to_maxwell.h`), then translated to SPIR-V.

The [development guide](docs/development.md) and [design notes](docs/design.md) go deeper.

<details>
<summary><b>Building from source</b></summary>

<br>

From a clone of this repository, the game is built with:

```powershell
python -I nx2/make_game.py --nup "C:\path\your-game.nup" --out "C:\Games\Triforce" --language en-US
```

Use `--romfs` and `--exefs` in place of `--nup` for unpacked folders, `--update <game folder>` to build a folder again
from the game files it keeps, or `--check` to only verify the game's version. You'll need Python 3 with `lz4`, `numpy`,
`capstone`, `unicorn` (2.1) and `zstandard`, plus the compiler and libraries described in the
[development guide](docs/development.md).

Triforce Setup lives in `setup/`: `setup\build.ps1 -Version vX.Y.Z` makes `TriforceSetup.exe`, and
`setup\pack_tools.ps1 -Tag vX.Y.Z` makes `Triforce-Tools.zip`. A release carries both files under the same tag.

</details>

<p align="center"><img src="assets/readme/divider.svg" alt="" width="100%"></p>

## <img src="assets/readme/emerald.svg" alt="" width="26"> Join the quest

The simplest help of all: **run Triforce on a graphics card nobody has tried yet, or play further than anyone has, and
report back.**

Want to do more? Fork it, fix what you find, and send the change back. Keep game files, keys and anything generated
from your game out of your fork. [CONTRIBUTING.md](CONTRIBUTING.md) explains the setup, the test commands, the code
style and what a good pull request looks like.

<p align="center"><img src="assets/readme/divider.svg" alt="" width="100%"></p>

## <img src="assets/readme/gold.svg" alt="" width="26"> The Sages

*Seven Sages sealed the Triforce. These ones helped forge it.*

> **Triforce is built off Hookshot**, NotRexed's native recompilation project.
>
> **NX2navi has merged with Triforce.** The two projects go on as one, under this name.

- **Evil:** Triforce's creator and lead. Brought the forest back to life with
  the GPU culling fixes for trees, ferns, grass, falling leaves, fireflies and the tree canopies; fixed the ambient
  light probes, the pond's water, the game's own fog, the freeze after loading a save and the exposure flicker; made the
  rendering diagnostics tools and Triforce Setup.
- **NX2navi (chrg6 and its contributors):** the native port that merged with Triforce in November 2026, with its own
  recompiler, renderer and launcher for the same game. Its players and their saves come along (**Convert saves**
  finds an NX2navi game by itself), and its work is coming into Triforce release by release.
- **NotRexed:** Hookshot, the recompiler this build started from, as well as BlueShell (Mario Kart World),
  Skybound (Tears of the Kingdom) and the a64-kit tools.
- **Eden** and **Eglantine:** the shader recompiler and the console shader work.
- **suyu:** the graphics decoders and the AArch64 translator.
- **seedds (nxnative):** how task and mesh shaders really behave.
- **zortos293 (Lichii):** Ampere shader lowering, and the compare-exchange retry fix.
- **mstorsjo (llvm-mingw):** the Windows compiler that comes with the build, so no Visual Studio is needed.
- **kinnay (zbic)**, **Opus** and **SDL:** decompression, sound and controller input.
- **sostenesfreitas (ocarina-recomp):** an early README layout, and notes that confirmed the mesh stage's control
  section.
- **MythosOfs (Orchid):** the original fog and exposure workaround.
- **Carlos Lopez (clshortfuse) and [RenoDX](https://github.com/clshortfuse/renodx):** the HDR tone mapping, eye
  adaptation and colour grading of the optional RenoDX HDR build, from RenoDX's mod for this game.
- **Mesa (NAK):** the map of Ampere's instruction encoding.
- **PigeonMage:** the 3D model of the mask the Mask Salesman wears on the Mods screen
  ([Majora's Mask](https://sketchfab.com/3d-models/majoras-mask-e9cfcdf9875846f6b6137f0156cd4eb2), CC BY 4.0; placed on
  his head and its textures resized, credits in `triforce_assets\CREDITS.txt` beside the game).
- **Everyone who sent fixes:** SkippySteve (a great many of them), klimovich008, d3zessete, chrispository, vzeol,
  monsterlag, leonidasmark, Segmentree, Zorkats, ShyVortex, hiKareeem, gab9281, i900gaming and more.

<p align="center"><img src="assets/readme/divider.svg" alt="" width="100%"></p>

## <img src="assets/readme/ruby.svg" alt="" width="26"> License

Triforce is open source under the **GNU General Public License, version 3 or any later version**. You're free to use
it, study it, change it and share it, as long as anything you share stays under the same license, with its source
available. The full terms are in [LICENSE](LICENSE).

Parts of Triforce come from other GPL projects (NotRexed's recompiler, Eden, suyu and Orchid) and remain under the
GPL. Bundled components with their own GPL-compatible licenses (Sirit, Vulkan Memory Allocator, SDL, Opus, stb, zbic
and the a64-kit tools) keep their notices in their own folders. Triforce Setup's tools package also carries Python and
llvm-mingw, under their own licenses.

The optional RenoDX HDR support is worked from RenoDX, which is under the MIT license: its notice is in
[third_party/renodx/LICENSE.md](third_party/renodx/LICENSE.md). RenoDX's own shaders are never part of Triforce: a
build that asks for them downloads and compiles them on the builder's PC.

<sub>Triforce is a fan-made project with no connection to Nintendo, and Nintendo hasn't endorsed it. The Legend of
Zelda and Ocarina of Time are Nintendo's trademarks.</sub>
