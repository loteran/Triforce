"""(Triforce) Free up space never deletes the folders a cut-off install lent from the installed version
(builder/py/hookshot_builder/core.py free, freeable and measure; builder/HookshotBuilder/Storage.cs and Reuse.cs; no network,
no release, no Setup started).

An update moves the folders of parts that did not change (Python, the compiler...) from the installed version into
versions\\<tag>.part and writes each down first in its reused.txt; an install cut off leaves them there. freeable() listed
every versions folder but the installed one, that <tag>.part included, and free() (also after each build) deleted it: the
installed version had lost its Python for good. Now freeable() and Measure leave such a folder out (and change nothing),
and free() gives its folders back first (undo_every_reuse) and then lets it go; one whose folders could not all go back stays.
While another install holds the setup's folder (tests/test_builder_setup_lock.py), free() undoes nothing and leaves
downloads\\ and versions\\ to it.

Checked in a temporary HOOKSHOT_SETUP_BASE with the Python builder's own functions, and with Reuse.cs compiled alone with a
console program (the compiler tests/setup_cs.py finds, .NET Framework's references) on authored folders. Storage.cs needs
.NET 8 and its folder is the real %LOCALAPPDATA%\\HookshotBuilder: its source is read for the calls.

    python -I -B tests/test_builder_free_lent.py
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest import mock

RUN_ALL_SKIP = {"linux": "the Python builder (builder/py) and the WPF builder are Windows programs"}

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "builder" / "py"))
from hookshot_builder import core, setup_parts as sp  # noqa: E402

fails = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        fails.append(what)


SHA_A = "a" * 64
HOLDER = r"""
import sys
sys.path.insert(0, %r)
from hookshot_builder import setup_parts as sp
lock = sp.take_lock(0, sys.argv[1])
print("1" if lock else "0", flush=True)
sys.stdin.read()
""" % str(ROOT / "builder" / "py")


def setup_base(base):
    """v1 installed (its python 1000 bytes), v2.part cut off holding v1's python and its note, v0 left from before (5000
    bytes), a download (3000 bytes)"""
    v = base / "versions"
    py = v / "v1" / "HookshotBuilder" / "tools" / "python"
    py.mkdir(parents=True)
    (py / "python.exe").write_bytes(b"p" * 1000)
    (v / "v1" / "HookshotBuilder" / "tools" / "nx2").mkdir(parents=True)
    (v / "v1" / "HookshotBuilder" / "tools" / "nx2" / "make_game.py").write_text("#", encoding="utf-8")
    (v / "v1" / "installed.txt").write_text("v1", encoding="utf-8")
    (v / "v1" / "parts.txt").write_text(SHA_A + "\n", encoding="utf-8")
    (v / "current.txt").write_text("v1", encoding="utf-8")
    (v / "v0").mkdir()
    (v / "v0" / "old.bin").write_bytes(b"o" * 5000)
    (base / "downloads").mkdir()
    (base / "downloads" / (SHA_A + ".zip")).write_bytes(b"z" * 3000)
    part = v / "v2.part"
    part.mkdir()
    p = sp.Part(SHA_A, "10", ".", "-", "-", "https://github.com/x/y/releases/download/v2/HookshotBuilder-python-a.zip")
    assert sp.reuse_part("v2", p, str(part)) and not py.exists() and (part / sp.REUSED).is_file()
    return py / "python.exe", part


def python_free():
    with tempfile.TemporaryDirectory(prefix="triforce-free-lent-") as tmp:
        for case in ("undone", "stuck", "installing"):
            base = Path(tmp) / case / "HookshotBuilder"
            with mock.patch.dict(os.environ, {"HOOKSHOT_SETUP_BASE": str(base)}):
                python, part = setup_base(base)
                v = base / "versions"
                lent_python = part / "HookshotBuilder" / "tools" / "python" / "python.exe"
                if case == "undone":
                    listed = [os.path.basename(d) for d in core.freeable()]
                    check("v2.part" not in listed and "v0" in listed and "downloads" in listed,
                          "freeable: a <tag>.part that holds the installed version's folders is not listed %s" % listed)
                    free = core.measure()[1]
                    check(free == 8000, "measure: what Free lets go leaves it out (%d bytes, 8000 wanted)" % free)
                    check(lent_python.is_file() and (part / sp.REUSED).is_file() and not python.exists(),
                          "measure: nothing is moved or deleted")
                    left = core.free(True)
                    check(python.is_file() and python.stat().st_size == 1000,
                          "free(after_build): the installed version has its python back")
                    check(sorted(os.listdir(v)) == ["current.txt", "v1"],
                          "free(after_build): v2.part (given back first) and v0 are gone %s" % sorted(os.listdir(v)))
                    check(not (base / "downloads").exists() and left == 0 and core.measure()[1] == 0,
                          "free(after_build): nothing freeable is left (%d, %d)" % (left, core.measure()[1]))
                elif case == "stuck":
                    with mock.patch.object(sp, "undo_reuse", lambda work: False):
                        core.free(True)
                    check(lent_python.is_file() and (part / sp.REUSED).is_file(),
                          "free: a <tag>.part whose folders could not all go back is kept, with them")
                    check(not (v / "v0").exists(), "free: and the rest goes")
                else:
                    holder = subprocess.Popen([sys.executable, "-I", "-B", "-c", HOLDER, str(base)], stdin=subprocess.PIPE,
                                              stdout=subprocess.PIPE, text=True, encoding="utf-8")
                    try:
                        check(holder.stdout.readline().strip() == "1", "installing: another process holds the setup's folder")
                        (base / "sa").mkdir()
                        (base / "sa" / "x.o").write_bytes(b"x")
                        core.free(True)
                        check(lent_python.is_file() and (part / sp.REUSED).is_file() and not python.exists(),
                              "free while an install runs: its <tag>.part is not undone under it")
                        check((v / "v0").is_dir() and (base / "downloads").is_dir() and not (base / "sa").exists(),
                              "free while an install runs: versions\\ and downloads\\ are left to it, the build files go")
                    finally:
                        holder.stdin.close()
                        holder.wait(timeout=60)


CS_FIXTURE = r"""
using System;
using System.IO;
namespace HookshotBuilder;
static class Fixture {
    static int fails;
    static void Check(bool ok, string what) { Console.WriteLine((ok ? "ok   " : "FAIL ") + what); if (!ok) fails++; }
    static int Main(string[] a) {
        var versions = Path.Combine(a[0], "versions");
        var py = Path.Combine(versions, "v1", "HookshotBuilder", "tools", "python");
        var part = Path.Combine(versions, "v2.part");
        var lent = Path.Combine(part, "HookshotBuilder", "tools", "python");
        var note = Path.Combine(part, "reused.txt");
        Directory.CreateDirectory(lent);
        File.WriteAllText(Path.Combine(lent, "python.exe"), "py");
        File.WriteAllText(note, "v1\nHookshotBuilder\\tools\\python\n");
        var plain = Path.Combine(versions, "v4.part");
        Directory.CreateDirectory(plain);
        File.WriteAllText(Path.Combine(plain, "kept.txt"), "x");
        Directory.CreateDirectory(Path.Combine(versions, "v1"));
        File.WriteAllText(Path.Combine(versions, "v1", "reused.txt"), "not a part");
        Check(Reuse.Lent(part) && Reuse.Lent(part + "\\"), "Reuse.Lent: a <tag>.part with a note");
        Check(!Reuse.Lent(plain) && !Reuse.Lent(Path.Combine(versions, "v1")), "Reuse.Lent: not one without, nor a finished version");
        // (held open: its folder cannot move, and the folder that still holds it is named)
        using (var held = new FileStream(Path.Combine(lent, "python.exe"), FileMode.Open, FileAccess.Read, FileShare.None))
            Check(Reuse.UndoEveryReuse(versions) == part && Reuse.Lent(part) && !Directory.Exists(py),
                  "Reuse.UndoEveryReuse: one that could not go back is named, and stays lent");
        Check(Reuse.UndoEveryReuse(versions) == null, "Reuse.UndoEveryReuse: then all went back");
        Check(File.Exists(Path.Combine(py, "python.exe")) && !Directory.Exists(lent) && !File.Exists(note) && !Reuse.Lent(part),
              "Reuse.UndoEveryReuse: the installed version has its python back, the note is gone, the part is not lent");
        Check(File.Exists(Path.Combine(plain, "kept.txt")), "Reuse.UndoEveryReuse: a .part without a note is left alone");
        // a note that names a path out of its folder, or a version's tag that leaves versions\: ignored
        Directory.CreateDirectory(Path.Combine(part, "x"));
        File.WriteAllText(note, "v1\n..\\..\\evil\nx\n");
        Directory.CreateDirectory(Path.Combine(versions, "v1", "x"));
        Check(Reuse.UndoEveryReuse(versions) == null && !Directory.Exists(Path.Combine(a[0], "evil")) &&
              Directory.Exists(Path.Combine(part, "x")), "Reuse.Undo: a path out of its folder is ignored, one already back left alone");
        File.WriteAllText(note, "..\\evil\nx\n");
        Check(Reuse.UndoEveryReuse(versions) == null && !File.Exists(note) && Directory.Exists(Path.Combine(part, "x")),
              "Reuse.Undo: a note without a version's name is dropped, nothing moved");
        return fails;
    }
}
"""


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def storage_source():
    src = (ROOT / "builder" / "HookshotBuilder" / "Storage.cs").read_text(encoding="utf-8-sig")
    freeable = between(src, "static IEnumerable<string> Freeable(", "/// Bytes in the builder's folder")
    check("!Reuse.Lent(v)" in freeable, "Storage.cs Freeable: a lent <tag>.part is not freeable")
    check("Reuse." not in freeable.replace("Reuse.Lent(v)", ""), "Storage.cs Freeable: changes nothing (Measure counts it)")
    measure = between(src, "public static (long total, long freeable) Measure()", "/// Lets go of what Measure counts")
    check("Reuse." not in measure and "Freeable()" in measure, "Storage.cs Measure: no lock, no undo, nothing moved")
    free = between(src, "public static long Free(bool afterBuild = false)", "public static string Gb(")
    take, undo = free.find("using var setup = Reuse.TakeLock(Base);"), free.find("Reuse.UndoEveryReuse(")
    delete = free.find("foreach (var d in Freeable(afterBuild, installing)) Delete(d);")
    check(0 <= take < undo < delete and "if (!installing) Reuse.UndoEveryReuse(" in free,
          "Storage.cs Free: takes the setup's folder, gives back what was lent, then deletes what is freeable")
    check("Freeable(afterBuild: true, installing)" in free, "Storage.cs Free: what is left after a build is counted the same way")


def csharp():
    import setup_cs
    csc = setup_cs.find_csc()
    if not csc:
        why = "no C# compiler here (tests/setup_cs.py): Reuse.cs was not run"
        if os.environ.get("CI"):
            raise SystemExit(why)
        print("NOT RUN: " + why)
        return
    with tempfile.TemporaryDirectory(prefix="triforce-reuse-") as tmp:
        Path(tmp, "f.cs").write_text(CS_FIXTURE, encoding="utf-8")
        exe = os.path.join(tmp, "reuse.exe")
        subprocess.run([csc, "/nologo", "/target:exe", "/langversion:latest", "/nullable:enable", "/out:" + exe,
                        "/reference:" + os.path.join(setup_cs.FRAMEWORK, "System.dll"),
                        "/reference:" + os.path.join(setup_cs.FRAMEWORK, "System.Core.dll"),
                        str(ROOT / "builder" / "HookshotBuilder" / "Reuse.cs"), os.path.join(tmp, "f.cs")],
                       check=True, timeout=300, stdout=subprocess.DEVNULL)
        work = os.path.join(tmp, "base")
        os.makedirs(work)
        r = subprocess.run([exe, work], capture_output=True, text=True, timeout=120)
        print(r.stdout, end="")
        check(r.returncode == 0, "Reuse.cs: every check passed (exit %d)" % r.returncode)


def main():
    storage_source()
    python_free()
    csharp()
    if fails:
        raise SystemExit("%d failed" % len(fails))
    print("all passed")


if __name__ == "__main__":
    main()
