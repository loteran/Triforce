"""Triforce Setup's Mods page works on the mods folder as the game reads it (setup/Mods.cs; no Setup is started).

Mods were folders to arrange by hand: a "-" in front of a name to switch one off, numbers in front to order them, an
archive to unpack into the right place. The Mods page does the same things to the same folders and keeps no list of its
own, so the game needs no change and a player who goes on by hand sees what the page shows.

Compiles Mods.cs (with Guard.cs, no window) and tests/setup_mods_test.cs and runs them on a game folder made in a
temporary folder, with authored mods and archives:

- the list as host/mods.c finds mods (four folders down, emulator layouts, names with - or _ off, names with a dot not
  looked at), what each carries, and the game's order;
- on and off by the folder's name, the order by a number in front that is kept apart from the mod's own name, every
  rename undone when one of them fails, and none made while the game runs;
- the files more than one mod carries and which mod wins them;
- the mark of the mods' code patches and cheats, which asks for a build when such a mod is added, removed, switched or
  edited and not when a RomFS mod is;
- removing into mods\\.removed, never a delete;
- archives: zips with the mod at their top, in a folder of their own, in an emulator's layout; refused with a reason
  when they hold no mod, several, one for another game, a path that leaves the folder, a device's name, a link, two
  files of one name, more than the bound (as listed, and as really unpacked) or more than the disk has room for;
  .7z and .rar through tar.exe, with a stand-in for it whose answers the test authors (a list with a link or a path out,
  an unpacking that leaves a junction or a hard link to a file outside) and with this PC's own tar.exe on a .7z it
  makes itself;
- and around every change: nothing outside the mods folder is made, changed or removed.

Reads the source for the window's side: the page asks the processes before a change, says "Build again to apply" from
the mark, and a test run stays inside its own folder.

    python -I -B tests/test_setup_mods.py
"""
import sys
from pathlib import Path

RUN_ALL_SKIP = {"linux": "Triforce Setup is a Windows program (.NET Framework 4.8)"}

sys.path.insert(0, str(Path(__file__).resolve().parent))
import setup_cs  # noqa: E402


def check_source():
    mods = setup_cs.source("setup/Mods.cs")
    # (every change begins by asking whether the game runs)
    for name in ("public static string SetOn(", "public static bool Move(", "public static string Remove(", "public static Mod Add("):
        body = mods[mods.index(name):]
        body = body[body.index("{"):]
        if not body.lstrip("{ \r\n").startswith("Refuse(mods);"):
            raise SystemExit("setup/Mods.cs %s no longer begins by asking whether the game runs" % name.strip("("))
    # (and nothing is renamed, made or deleted past the question "is this inside the mods folder?")
    code = "\n".join(line for line in mods.splitlines() if not line.lstrip().startswith("//"))
    allowed = {"Directory.Move(": 1, "Directory.Delete(": 2, "File.Delete(": 1}
    for call, most in allowed.items():
        if code.count(call) != most:
            raise SystemExit("setup/Mods.cs has %d calls of %s where %d are known (MoveFolder; Empty, which only clears what an archive's "
                             "own unpacking left): a new one must go through Within" % (code.count(call), call, most))
    shell = setup_cs.source("setup/Shell.Mods.cs")
    for needle, why in (
            ("Mods.GameRuns = GameRuns;", "the page no longer hands Mods.cs its look at the running game"),
            ("if (!Install.Allowed(folder))", "a test run of Setup may change the mods of a game folder outside its own folder"),
            ("Mods.NeedsBuild(Mods.CodeState(mods), ModsBuilt(game))", "the page no longer compares the mods' code with the last build's"),
            ("Mods.BuildAgain", "the page no longer says \"Build again to apply\""),
            ('Install.Set("ModsBuilt",', "the mark of the last build is no longer kept")):
        setup_cs.need(shell, needle, "setup/Shell.Mods.cs: " + why)
    main = setup_cs.source("setup/Shell.cs")
    ended = main[main.index("void Ended(string? failure)"):main.index("void CopyTheReport()")]
    setup_cs.need(ended, "MarkModsBuilt(finished);", "setup/Shell.cs Ended: a build that ended well no longer marks the mods' code as built in")
    build = setup_cs.source("setup/build.ps1")
    for file in ("Mods.cs", "Shell.Mods.cs"):
        setup_cs.need(build, '"$here\\%s"' % file, "setup/build.ps1 no longer compiles " + file)


def main():
    check_source()
    setup_cs.run_fixture("setup_mods_test.cs", sources=("setup/Mods.cs",))


if __name__ == "__main__":
    main()
