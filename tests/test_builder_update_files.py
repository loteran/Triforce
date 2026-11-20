"""The Windows builder's own update (builder/HookshotBuilder/Updater.cs; no WPF, no network, no release).

Its download believed the server's length over the size the release lists, went straight into the file it was to become,
and asked the disk nothing; its unpacking let .NET write whatever names the zip held, left its part folder when it failed,
and deleted the version there before the new one was in its place, so a move that failed left neither.

Compiles Updater.cs's own UnsafeName (cut out of it, with its device list) and runs it on names, with the same answers as
Setup's rule (setup/Mods.cs Unsafe) and the Python builder's (builder/py/hookshot_builder/setup_parts.py unsafe_member).
Reads the source for the rest: DownloadOnce takes the release's size first and refuses another, asks for room before the
first byte, writes <file>.part and renames it once whole, removing it on a failure; Unpack checks every name before it
extracts, removes its part on a failure, and moves the version there aside and back.

    python -I -B tests/test_builder_update_files.py
"""
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_SKIP = {"linux": "the Windows builder is a Windows program"}

sys.path.insert(0, str(Path(__file__).resolve().parent))
import setup_cs  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder" / "py"))
from hookshot_builder import setup_parts  # noqa: E402

NAMES = ["NUL", "nul.txt", "tools/CON", "a/Aux.dll", "COM1", "lpt9.log", "com\u00b9.txt", "a/CONIN$", "prn .txt", "a/b.", "a/b ",
         "a./b", "c:/x", "C:x", "a/b:stream", "/etc/x", "\\x", "../x", "a/../../x", "a\\..\\x", "a/b<c", "a/b?", "a/\x01", "",
         "tools/python/python.exe", "a/b/", "./a", "COM10", "LPT0", "console.txt", "nulx", "a.b.c", "CONFIG",
         "HookshotBuilder/HookshotBuilder.exe"]

PROGRAM = """
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
static class Cut
{
/* ACTUAL */
    static int Main()
    {
        foreach (var line in File.ReadAllLines(Environment.GetCommandLineArgs()[1]))
        {
            var name = string.Concat(line.Split(',').Where(s => s.Length > 0).Select(s => (char)int.Parse(s)));
            Console.WriteLine(UnsafeName(name) == null ? "ok" : "refused");
        }
        return 0;
    }
}
"""


def check_source(src):
    once = src[src.index("static async Task DownloadOnce("):src.index("static HttpRequestException HttpFailure(")]
    for needle, why in (
            ("var total = size > 0 ? size : told ?? 0;", "the server's length goes before the release's size again"),
            ("if (size > 0 && told is { } other && other != size)", "a server that names another size than the release is believed"),
            ("NoRoom(path, total)", "no room is asked for before the download"),
            ('var part = path + ".part";', "the download goes straight into its file"),
            ("File.Move(part, path, true);", "the part is not renamed into place once whole"),
            ("File.Delete(part);", "a failed download leaves its part")):
        if needle not in once:
            raise SystemExit("builder/HookshotBuilder/Updater.cs DownloadOnce: " + why)
    if once.index("NoRoom(path, total)") > once.index("File.Create(part)"):
        raise SystemExit("builder/HookshotBuilder/Updater.cs DownloadOnce asks for room after it began writing")
    unpack = src[src.index("static async Task<string> Unpack("):]
    if not unpack.index("UnsafeName(e.FullName)") < unpack.index("ZipFile.ExtractToDirectory(zip, part)"):
        raise SystemExit("builder/HookshotBuilder/Updater.cs Unpack extracts before every name was checked")
    if "Directory.Delete(target, true)" in unpack or "Directory.Move(old, target)" not in unpack:
        raise SystemExit("builder/HookshotBuilder/Updater.cs Unpack deletes the version there before the new one is in place")


def main():
    src = (ROOT / "builder/HookshotBuilder/Updater.cs").read_text(encoding="utf-8-sig")
    check_source(src)
    expect = ["refused" if setup_parts.unsafe_member(n) else "ok" for n in NAMES]
    csc = setup_cs.find_csc()
    if not csc:
        print("NOT RUN: no C# compiler here; the source was read")
        return
    start = src.index("    static readonly HashSet<string> Devices")
    end = src.index("    static HttpRequestException HttpFailure(")
    with tempfile.TemporaryDirectory(prefix="builder-names-") as tmp:
        cs, exe, names = Path(tmp) / "cut.cs", Path(tmp) / "cut.exe", Path(tmp) / "names.txt"
        cs.write_text(PROGRAM.replace("/* ACTUAL */", src[start:end]), encoding="utf-8")
        names.write_text("\n".join(",".join(str(ord(c)) for c in n) for n in NAMES) + "\n", encoding="utf-8")
        subprocess.run([csc, "/nologo", "/langversion:latest", "/nullable:enable", "/out:" + str(exe),
                        "/reference:" + str(Path(setup_cs.FRAMEWORK) / "System.Core.dll"), str(cs)], check=True, cwd=tmp)
        got = subprocess.run([str(exe), str(names)], check=True, capture_output=True, text=True, cwd=tmp).stdout.split()
    wrong = [(n, g, e) for n, g, e in zip(NAMES, got, expect) if g != e]
    if len(got) != len(NAMES) or wrong:
        raise SystemExit("Updater.cs UnsafeName answers otherwise than the builder's rule: %r" % wrong)
    print("PASS: the builder's update checks names as Setup does, takes the release's size and writes through a part")


if __name__ == "__main__":
    main()
