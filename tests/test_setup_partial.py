"""Triforce Setup's copies are never seen half done, and a save is copied under the game's own lock (setup/Guard.cs Partial and
SaveLock, setup/Saves.cs Copy, setup/Setup.cs Install.Run and Tools; no Setup and no game is started).

Setup copied itself into the install folder and a save over the one there file by file, in place: a full disk, a file held
open or Setup closed in the middle left a Setup that does not start under the name the shortcuts start, or half a save. It
found a running game by its processes, then copied: a game started in between mounted half a save. The tools' zip had only
its paths out of the folder skipped, and the other names written as Windows took them.

Compiles Guard.cs, Mods.cs, Saves.cs, Setup.cs's Tools class (cut out of it) and tests/setup_partial_test.cs, and runs them
in a temporary folder: a copy cut off before it goes into place leaves what was there and no partial, a partial left before
is no obstacle; the save's lock file of Triforce's, NX2navi's and an emulator's save folders; a copy refused while the lock
is held (and not with TRIFORCE_SAVE_LOCK=0), the lock held across the copy and let go after, a copy that fails in the middle
leaving the save that was there, the backup of the one replaced, a save moved aside by a copy cut off coming back; a tools zip
with a device's name, a name ending in a dot or a space, a colon or a path through .. refused before its first file, and the
tools moved aside by an unpacking cut off coming back. Reads the source for the callers: Install.Run copies through Partial,
Saves.Copy takes the lock first, the game's side takes the same file (host/save_tx.h, tests/test_save_lock.py).

    python -I -B tests/test_setup_partial.py
"""
import sys
from pathlib import Path

RUN_ALL_SKIP = {"linux": "Triforce Setup is a Windows program (.NET Framework 4.8)"}

sys.path.insert(0, str(Path(__file__).resolve().parent))
import setup_cs  # noqa: E402


def check_source():
    setup = setup_cs.source("setup/Setup.cs")
    run = setup[setup.index("public static string Run(string dir, bool desktop, bool startMenu)"):setup.index("static string StartFolder")]
    if "Partial.CopyFile(Self, exe);" not in run or "File.Copy(" in run:
        raise SystemExit("setup/Setup.cs Install.Run copies Setup into the install folder in place, not through Partial.CopyFile")
    unpack = setup[setup.index("public static void Unpack(string zip, string tag"):setup.index("/// nx2/make_game.py as a child process")]
    if unpack.index("Mods.Unsafe(name, true)") > unpack.index("e.ExtractToFile("):
        raise SystemExit("setup/Setup.cs Tools.Unpack writes files before every name was checked")
    saves = setup_cs.source("setup/Saves.cs")
    copy = saves[saves.index("public static string? Copy(string from"):saves.index("static void CopyTree(")]
    order = [copy.index(n) for n in ("SaveLock.Take(dst)", "SaveLock.Take(src)", "Partial.Recover(dst);", "Partial.Stage(src, dst)",
                                     "CopyTree(dst, backup)", "Partial.SwapIn(partial, dst)")]
    if order != sorted(order) or "Directory.Delete(dst, true)" in copy:
        raise SystemExit("setup/Saves.cs Copy: the locks are not taken first, or the copy does not go through a partial into place")
    guard = setup_cs.source("setup/Guard.cs")
    tx = (setup_cs.ROOT / "host/save_tx.h").read_text(encoding="utf-8")
    if '"save.lock"' not in guard or '"%s\\\\save.lock"' not in tx:
        raise SystemExit("Setup's save lock (setup/Guard.cs SaveLock) and the game's (host/save_tx.h save_tx_lock) are not the same file")


def main():
    check_source()
    tools = setup_cs.cut("setup/Setup.cs", "/// The Triforce tools beside the installed Setup", "/// nx2/make_game.py as a child process",
                         using=("System", "System.IO", "System.IO.Compression", "System.Linq"))
    setup_cs.run_fixture("setup_partial_test.cs", extra=tools, sources=("setup/Mods.cs", "setup/Saves.cs"))


if __name__ == "__main__":
    main()
