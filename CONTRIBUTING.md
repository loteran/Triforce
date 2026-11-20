# Contributing to Triforce

Triforce is a native PC port of the Switch 2 *The Legend of Zelda: Ocarina of Time* remake. The game's code is
translated to C and built on your own PC from your own copy of the game; the system and the graphics are rebuilt
natively, with the graphics replayed on Vulkan. No emulator is involved. Triforce builds on the open-source Hookshot
project (GPL-3.0), and its code keeps that license.

This page covers what to send, how to build and test it, and the rules that keep the project out of trouble. The
[development guide](docs/development.md) has the full pipeline and its commands; the [design notes](docs/design.md)
have the architecture.

## The rule with no exceptions

**No game files, keys, or anything made from them leave your PC.** The repository does not contain the game and
never will. That includes:

- the `.nup`, `romfs`, `exefs`, `.nso`, `.nca`, `.nsp`, `.xci` or `.nsz`/`.xcz` files, and any title keys;
- `work/` and everything in it: the extracted dump, `image.bin`, the translated C, test builds, runs and screenshots;
- `Triforce.exe`, `triforce_vk.dll`, `triforce_shaders.bin`, `triforce_programs.bin` (and `.idx`),
  `triforce_pipelines.bin`, `mkw_vk_pipelines.bin` and every other file a build or a run makes;
- saves (`user/`), `triforce_options.ini`, `triforce_env.txt` and `language.txt`.

Do not open an issue or a pull request asking where to get the game, how to decrypt it, or for someone else's files.
A screenshot, a video and `triforce_log.txt` are fine and often necessary; the log holds no game data.

`.gitignore` covers most of this, but check `git status` before you commit. If you are not sure whether a file is
game data, leave it out.

## Reporting a problem

Open an [issue](https://github.com/ruckus7382sfgsgdf/CBeta0.2/issues) and include:

- the Triforce version (the release name, or the first line of `triforce_log.txt`);
- your graphics card, its driver version, and your Windows version;
- where you were in the game and what you did;
- the logs:
  - **a game problem:** `triforce_log.txt` from your game folder (the folder with `Triforce.exe`). If you started the
    game again after the problem, send `triforce_log.1.txt` too: each start keeps the four runs before it as
    `triforce_log.1.txt` (the last) to `triforce_log.4.txt`.
  - **a freeze:** `triforce_hang.txt` from the game folder as well, when it is there. After 20 seconds without a frame
    the game writes down where each of its threads was (addresses in the program, no game data).
  - **something seen in play:** press **F8** when you see it, and send the folder it names (`captures\<date>-<time>`
    in the game folder): a picture of the frame, the end of the log, your settings and your save, with no game file
    among them. The System tab's **Export save for a bug report** makes one zip of the save and the logs instead.
  - **a Setup or build problem:** `setup.log` from `%LOCALAPPDATA%\Programs\Triforce`. For a build that stopped, press
    **Copy report** on the build page and paste it: the Setup and tools versions, what the build was started with,
    the last 200 lines of **Show details** and of `setup.log`, with your user folder written as `%USERPROFILE%` so
    your name is not in it.

Before you report:

- Try the [newest release](https://triforceproject.com) first.
- Search the open issues; if yours is there, add your details to it.
- For graphics or performance problems, test without ReShade, overlays, shader-cache tools and frame-generation
  injectors: they change what Triforce does.
- If graphics look wrong after an update, clearing the shader caches often helps: close the game and delete
  `triforce_programs.bin`, `triforce_programs.idx`, `triforce_pipelines.bin`, `triforce_pipeline_states.bin`,
  `triforce_computes.bin`, `mkw_vk_pipelines.bin`, `mkw_vk_pipelines_play.bin` and `triforce_prepared.bin` from the game folder (never
  `triforce_shaders.bin`). The next start prepares them again.

One topic per issue or pull request, in English, please.

## Ways to help

| | What |
|---|---|
| **Test on other hardware** | NVIDIA cards are the most tested. Reports from AMD and Intel cards and other drivers are useful even when everything works. |
| **Play further** | Reaching new areas, cutscenes, menus and items finds problems nobody has seen yet. |
| **Report what is broken** | As above, with the version, your hardware, where you were and the log. |
| **Send a fix** | Fork, fix, test, open a pull request. Small, focused changes are easiest to review. |
| **Improve the docs** | Corrections and clearer steps are welcome. |

## Development setup

You need your own supported copy of the game: program `0400486024DEC000`, update v131072, build `e0f3ccfa`. Other
versions do not work, and the build reads your files where they are without changing them. A build needs about 60 GB
free and takes around 40 minutes; a run uses about 10 GB of RAM. Build on an SSD if you can.

The easy way to get every tool a build needs is **Triforce Setup** from the
[releases](https://triforceproject.com): it installs the tools (Python with its packages,
llvm-mingw, the Vulkan headers, FFmpeg, SDL2) into `%LOCALAPPDATA%\Programs\Triforce\tools`.

To build from a clone instead:

1. Install Python 3 from [python.org](https://www.python.org/downloads/) (not the Microsoft Store build) and the
   pipeline's packages: `python -m pip install -r requirements.txt`.
2. Get [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (the `ucrt-x86_64` build) and set `HOOKSHOT_MINGW` to its
   folder.
3. From the installed tools folder, point `RX_CPM` at `tools\cpm` and `WGPU_NATIVE` at `tools\wgpu-native`, and copy
   `tools\third_party` into `third_party\` and `tools\work` into `work\`.
4. Run everything with `python -I` from the repository root.

(Some environment variables still carry the older `HOOKSHOT_` name; they work as documented.).

### Triforce Setup

The launcher lives in `setup/`: a .NET Framework 4.8 WPF app, built with `powershell -File setup\build.ps1` (needs the
C# compiler from Visual Studio Build Tools). `setup\pack_tools.ps1` packs the tools zip a release carries. Never run a
development build's install, update or uninstall without `TRIFORCE_TEST_ROOT` set to a scratch folder: it would act
on your real install. With it the install, the settings (a text file in place of the registry), the shortcuts, the
game folders and the build's work folder (`<root>\build`) all stay inside that folder. The network is not part of
that: such a run still asks the release site and downloads, unless `TRIFORCE_RELEASE_API` names another address.

What keeps a run of Setup from damaging an install is in `setup/Guard.cs`, written without the window so that
`tests/test_setup_*.py` compile it alone (with the same C# compiler) and run it, no Setup started:

- **One Setup at a time for an install.** The first one holds a lock named after the folder it works in (and, in a
  test run, after `TRIFORCE_TEST_ROOT`, so a test never meets the real install's Setup). A second start brings the
  first one's window to the front and ends; an uninstall is refused while Setup is open.
- **Room on the disk, asked first.** Before the tools are downloaded Setup asks the disk for room for the zip and for
  what it unpacks to (five times the zip's size, until the zip is there and says it exactly before its first file is
  unpacked), and stops with both numbers, what it takes and what is free, when there is none. With less than 60 GB on
  the game folder's disk the Build button says so in the same way and still lets the player go on. An update of a game
  folder is not asked about: what it takes depends on what changed.
- **Downloads that go on, and are checked.** A file is fetched into `<file>.part`. One that was cut off (the
  connection, Cancel, Setup closed) is gone on from at the next try with a Range request, but only as a part of the
  same file: `<file>.part.of` names the address and size it was begun for and the server's ETag (else its date) for
  it, which goes back as If-Range; anything else is fetched anew. A server that sends nothing for 60 seconds is given
  up. The file gets its name only once it has the size the release lists, which the server must not contradict. A
  tools zip that is there whole is not fetched again; one that does not unpack is.
- **Copy report**, beside Show details after a build that stopped (not one the player cancelled): what Setup knows of
  the build and the last 200 lines of its details and of `setup.log` go to the clipboard, with the account's folder
  (in either slash, doubled backslashes, any case, and its short name) replaced by `%USERPROFILE%`. A name in a path
  outside that folder stays.
- **Its own update, undone when the new Setup does not start.** The new exe takes the running one's name, the running
  one goes on as `TriforceSetup.old.exe` and starts the new one with `--relaunched`. No second program watches: the
  old Setup itself stays, out of sight, until the new one's window is up (a named event), 30 seconds at most. A new
  Setup that cannot be started, or that ends with an error's exit code before that, is replaced by the old exe again in
  one rename; the old window comes back and says so, and that release's Setup is not fetched again on this PC
  (`SetupSkip` in `HKCU\Software\Triforce`; the tools still are). One that runs on without the sign, or ends without
  an error, is left alone.

Three more parts are written the same way, each a file without the window and a test that runs it on folders of
its own:

- **The Mods page** (`setup/Mods.cs`, `tests/test_setup_mods.py`; the page is `Shell.Mods.cs`). It keeps no list:
  the mods folder is read as `host/mods.c` reads it, and every change is one a player could make by hand. On and off
  is the `-` in front of a folder's name; the order is a number in front (`01-`, two or three digits and a hyphen),
  kept apart from the mod's own name, and moving a mod numbers the folders of the mods folder anew, all of them or
  none; a removed mod moves into `mods\.removed`; an archive is unpacked into `mods\.adding-<id>` (a name with a
  dot in front is not looked at by the game), checked as it lies there (no path out of the folder, no link, no
  device's name, a bound on size and count), and only its mod folder moves into place. `.zip` is read with .NET,
  `.7z` and `.rar` with the `tar.exe` in System32 and nothing else: do not add an unpacker to the repository. Every
  write goes through one question, "is this inside the mods folder?", and no change is made while the game of that
  folder runs. Code patches (`exefs`) are built in: Setup keeps a mark of them at each build (`ModsBuilt` in
  `HKCU\Software\Triforce`) and says "Build again to apply" while the mods' mark is another one. Cheats are not in
  that mark: the game reads them when it starts.
- **Add to Steam** (`setup/Steam.cs`, `tests/test_setup_steam.py`; the pictures are drawn in `SteamArt.cs`, the
  link is `Shell.Steam.cs`). Steam's `shortcuts.vdf` is another program's file: it is read whole, other shortcuts are
  copied through as bytes and never rebuilt, a file that does not read to its end is refused, the new file takes the
  old one's name in one rename, and the old one is kept (`.triforce-backup`, `.before-triforce`). Nothing is
  written while Steam runs, and only from the question the player answers. **No test and no test run may touch a
  real Steam folder:** Steam's folder is always handed in, tests author their own in a temporary folder, and with
  `TRIFORCE_TEST_ROOT` Setup looks only at `<root>\Steam` and never asks the registry.
- **Sounds** (`setup/Sound.cs`, `tests/test_setup_sound.py`; where they play is `Shell.Sounds.cs`). Setup plays the
  game's own decide and cancel sounds, `Sound/Resource/Stream/SE_Sys_Do00.bwav` and `SE_Sys_Cancel00.bwav`, read from
  the player's game folder (or from the mod the game itself would read them from) and decoded in memory. **No sound
  is in the repository, in a release or in Setup, and none may be added:** before a game is built, and for a file that
  is missing or does not decode, Setup is silent. Nothing decoded is written to the disk. The tests author their own
  BWAV files and never read a game's. Sounds play only for what the player does (a click, a row chosen, a choice
  changed; not a page the programme turns), not while the game runs, and never in a run without a player
  (`--snapshot`, `TRIFORCE_UI_SCRIPT`, `TRIFORCE_TEST_ROOT`): such a run gets, with `TRIFORCE_SOUND_LOG=<file>`, a
  line in that file for each sound that would have played, and still makes none.

## Building

| Command | What it does |
|---|---|
| `python -I nx2/make_game.py --nup "C:\path\your-game.nup" --out "C:\Games\Triforce" --language en-US` | Build a complete game folder from a `.nup`. |
| `python -I nx2/make_game.py --romfs "C:\path\romfs" --exefs "C:\path\exefs" --out "C:\Games\Triforce"` | The same, from extracted folders. |
| `python -I nx2/make_game.py --check --nup "C:\path\your-game.nup"` | Check the game and its version without building. |

The translated C and every build are local files; they are never committed.

### RenoDX HDR (optional)

RenoDX's tone mapping and HDR output for OoT (`gpu/vk/vk_renodx.cpp`) are built in only with RenoDX's Zelda OoT shaders
(github.com/clshortfuse/renodx, MIT). They are worked from the game's own shaders, so neither they nor what is compiled
from them (`renodx_spv.h`) is ever committed, bundled or uploaded. `--renodx` (the builder's RenoDX HDR tick) runs
`scripts/renodx_shaders.py`, which fetches the sources at one pinned commit and the Slang compiler at one pinned
release, each file checked against its SHA-256, and compiles them on the PC into a `renodx` folder beside the work
folder (or `HOOKSHOT_RENODX=<folder>`). An `--update` of that game folder keeps it; `--no-renodx` drops it. In the game
it works with the Insert menu's HDR set to Native, where its rows appear.

Each replacement is named by the CRC-32 of the module the renderer's own emitter makes for that shader (the table in
`scripts/renodx_shaders.py`). A change to the emitter changes those, and RenoDX then replaces nothing, with only its
coverage line in the log to say so: dump the modules again (`RX_VK_DUMP_ALL_SPV=<folder>`) and match them by what they
read. Never share a game folder built with it.

What RenoDX's mod does not have (RenoDRT as a tone mapper, the gamma correction) is in `gpu/vk/shaders/renodx_glue`:
files of Triforce's own that are compiled together with the fetched shaders and call RenoDX's library there. Keep it
that way: nothing of RenoDX's source goes into them or anywhere else in the repository.

## Testing

The game-free checks need no game files:

```powershell
python -I -B tests/run_all.py
```

It runs every `tests/test_*.py` and `nx2/test_*.py`, the C++ unit tests under `gpu/vk/tests`, and a byte-compile of
every Python file, and ends with a table of results. Name a check to run only the ones whose path contains it. A new
`test_*.py` file is picked up by itself.

When you add a fix, add a game-free test where the behavior can be tested without the game. For a renderer or runtime
fix, say what you saw before and after, on which GPU and driver, and where in the game.

## Code conventions

- Match the file you are editing: its indentation, comment style and naming. Do not reformat unrelated code.
- Do not modify `third_party/`. Its parts are unmodified upstream copies, each with its notice in its own
  `PROVENANCE.md`.
- New switches use the `RX_` prefix in the renderer and `HOS_` in the runtime, are off unless asked for, and cost
  nothing while off.
- Rendering fixes must reproduce what the game itself does, not tuned factors that only look right on one screen.
- Python runs with `-I`; tests stay on the standard library. Keep line endings LF.

## Pull requests

1. Fork the repository and branch from `main`.
2. One topic per pull request: a fix does not carry a refactor.
3. Title: a short imperative sentence. Body: why it is needed, what changed, how it was tested (GPU, driver, game
   area, fps before and after), and the issue it fixes.
4. Screenshots for anything that shows on screen, before and after.
5. No build output, dumps or generated files. Check `git status`.

## Licensing

Triforce is free software under the [GNU General Public License, version 3 or later](LICENSE), like the Hookshot code
it builds on. By submitting a change you agree to license it under those terms. Do not paste code from a project with
an incompatible license, and never paste decompiled game code or anything generated from the game files.

The third-party parts (Eden, suyu, Sirit, the Vulkan Memory Allocator, SDL, Opus, Dear ImGui, zstd, the Libertinus
fonts, SMAA, zbic and the a64-kit tools) keep their own notices. Preserve them.

Thanks for testing, reporting and sending fixes. Every report from a different GPU and every area played further
moves the port forward.
