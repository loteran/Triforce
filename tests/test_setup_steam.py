"""Triforce Setup's Add to Steam writes Steam's shortcuts file exactly, or not at all (setup/Steam.cs; no Setup is
started, and the PC's real Steam is not looked at).

The game is added to Steam as a non-Steam shortcut by writing into another program's settings: an account's
config\\shortcuts.vdf, a binary file, and its config\\grid folder for the pictures. A file put together wrongly loses
the player's other shortcuts.

Compiles Steam.cs (with Guard.cs, no window) and tests/setup_steam_test.cs and runs them on Steam folders made in a
temporary folder, with shortcuts files the test authors byte by byte (not with Steam.cs's own writer):

- no file, an empty file and an empty list give the file Steam would write for one shortcut; with other shortcuts
  there (64-bit numbers, floats, values of unknown names, bytes that are no UTF-8, maps in maps, the lower-case names of
  older Steams) ours goes after them and every other byte is the file's own;
- a shortcut of ours that is there is set to the game folder in place, keeping what the player set in Steam for it;
  a shortcut the player named Triforce to another program is not ours;
- removing gives the file back to the byte; shortcuts after ours are counted anew only where the file counted them
  as Steam does;
- a file cut off after any number of its bytes, one that goes on after its end, one with a value of an unknown kind
  or without the list is refused, and on the disk nothing of the account's folder changes then;
- on the disk: written through a temporary file, the file as it was kept (before the last change, and before the
  first), the pictures under the shortcut's number in the grid folder, the player's own pictures kept, nothing
  outside the account's config folder touched; nothing written while Steam runs ("Close Steam first"), nor over a file
  that changed while Setup worked;
- the accounts of a Steam folder and which of them is the most recent, from Steam's own list or without it.

Reads the source for the window's side: the registry is only read, a test run never asks it and never looks at the
PC's Steam, and the shortcut is written on the player's click alone.

    python -I -B tests/test_setup_steam.py
"""
import re
import sys
from pathlib import Path

RUN_ALL_SKIP = {"linux": "Triforce Setup is a Windows program (.NET Framework 4.8)"}

sys.path.insert(0, str(Path(__file__).resolve().parent))
import setup_cs  # noqa: E402


def check_source():
    steam = setup_cs.source("setup/Steam.cs")
    code = "\n".join(line for line in steam.splitlines() if not line.lstrip().startswith("//"))
    # (the registry: read, never written; and nothing else of the PC is asked where Steam is)
    if code.count("Registry.") != 1 or 'Registry.CurrentUser.OpenSubKey(@"Software\\Valve\\Steam", false)' not in code:
        raise SystemExit("setup/Steam.cs: the registry is asked in another way than one read of Steam's own key")
    for call in ("CreateSubKey", "SetValue", "DeleteValue", "DeleteSubKey", "Process.Start"):
        if call in code:
            raise SystemExit("setup/Steam.cs: %s has no place there (Steam's registry is only read, Steam is never started or stopped)" % call)
    # (every file is written through WriteWhole, which asks whether the path is in the account's config folder)
    whole = code[code.index("static void WriteWhole("):code.index("static byte[] Bytes(")]
    if not whole[whole.index("{"):].lstrip("{ \r\n").startswith("Within(user, file);"):
        raise SystemExit("setup/Steam.cs WriteWhole no longer begins by asking whether the file is in the account's config folder")
    rest = code.replace(whole, "")
    for call in ("File.WriteAll", "new FileStream(", "File.Copy(", "File.Create(", "File.Replace("):
        if call in rest:
            raise SystemExit("setup/Steam.cs writes a file past WriteWhole (%s)" % call)
    for name in ("public static bool Add(", "public static bool Remove("):
        body = code[code.index(name):]
        if not re.match(r"\{\s*if \(Running\(user\.Root\)\) throw new SteamException\(CloseFirst\);", body[body.index("{"):]):
            raise SystemExit("setup/Steam.cs %s no longer begins by asking whether Steam runs" % name.strip("("))
    shell = setup_cs.source("setup/Shell.Steam.cs")
    for needle, why in (
            ('Install.TestRoot != null ? System.IO.Path.Combine(Install.TestRoot, "Steam")', "a test run of Setup may look at the PC's real Steam"),
            ("if (!Install.Allowed(user.Config))", "a test run of Setup may write outside its own folder"),
            ("Steam.CloseFirst", "the page no longer says to close Steam first"),
            ("SteamArt.Make()", "the shortcut's pictures are no longer Setup's own drawings")):
        setup_cs.need(shell, needle, "setup/Shell.Steam.cs: " + why)
    # (written only from the question the player answers: no other caller)
    for call in ("Steam.Add(", "Steam.Remove("):
        users = [f for f in ("Setup.cs", "Shell.cs", "Shell.Home.cs", "Shell.Saves.cs", "Shell.Test.cs", "Shell.Mods.cs", "Shell.Steam.cs", "SteamArt.cs")
                 if call in setup_cs.source("setup/" + f)]
        if users != ["Shell.Steam.cs"] or shell.count(call) != 1:
            raise SystemExit("%s is called from %s: it belongs to the one place where the player has said yes" % (call, users))
    art = setup_cs.source("setup/SteamArt.cs")
    setup_cs.need(art, "foreach (var (kind, _, width, height) in Steam.Pictures)", "setup/SteamArt.cs no longer draws the pictures in the sizes Steam.cs lists")
    for word in ("http", "WebClient", "File.Read", "FromFile"):
        if word in art:
            raise SystemExit("setup/SteamArt.cs: the pictures are drawn from Setup's own crest, not read or fetched (%s)" % word)
    build = setup_cs.source("setup/build.ps1")
    for file in ("Steam.cs", "SteamArt.cs", "Shell.Steam.cs"):
        setup_cs.need(build, '"$here\\%s"' % file, "setup/build.ps1 no longer compiles " + file)


def main():
    check_source()
    setup_cs.run_fixture("setup_steam_test.cs", sources=("setup/Steam.cs",))


if __name__ == "__main__":
    main()
