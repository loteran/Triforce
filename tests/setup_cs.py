"""Compiles setup/Guard.cs (Triforce Setup's parts that need no window; with it, where a check asks, another such file
as setup/Mods.cs, setup/Steam.cs or setup/Sound.cs) with a console test program and runs it: shared by the tests/test_setup_*.py checks,
not a check itself.

The compiler is the one setup/build.ps1 uses (the Roslyn csc.exe of Visual Studio or its Build Tools: Setup is written in
modern C#, which the csc.exe inside Windows does not read), the references are .NET Framework 4.8's, which every
Windows 10 and 11 has. The program runs in a temporary folder of its own: no Setup is started, and nothing of an installed
Triforce is read or written.
"""
import glob
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FRAMEWORK = r"C:\Windows\Microsoft.NET\Framework64\v4.0.30319"
REFERENCES = ("System.dll", "System.Core.dll", "System.Net.Http.dll", "System.IO.Compression.dll",
              "System.IO.Compression.FileSystem.dll")


def find_csc():
    """setup/build.ps1's search, and its choice among several"""
    found = glob.glob(r"C:\Program Files*\Microsoft Visual Studio\*\*\MSBuild\Current\Bin\Roslyn\csc.exe")
    return sorted(found, reverse=True)[0] if found else None


def source(rel):
    return (ROOT / rel).read_text(encoding="utf-8-sig")


def need(text, needle, why):
    """A check that reads the source: what cannot be run without starting Setup"""
    if needle not in text:
        raise SystemExit(why)


def cut(rel, start, end, using=()):
    """A class cut out of a source file that needs the window (from the line with start to the one with end), as a
    source of its own: the production code, to compile beside stand-ins for what it calls"""
    text = source(rel)
    return "".join("using %s;\n" % u for u in using) + "namespace TriforceSetup;\n" + text[text.index(start):text.index(end)]


def run_fixture(fixture, extra=None, timeout=300, sources=()):
    """Compiles setup/Guard.cs with tests/<fixture> (and extra, the text of one more source: see cut; and sources,
    more files of setup/ that need no window, as setup/Mods.cs) and runs the program; its exit code other than 0 fails
    the check. False when this PC has no compiler for it (then only the source was read); on CI that fails instead."""
    csc = find_csc()
    if not csc:
        why = "no C# compiler here (Visual Studio Build Tools: MSBuild\\Current\\Bin\\Roslyn\\csc.exe), %s was not run" % fixture
        if os.environ.get("CI") or os.environ.get("GITHUB_ACTIONS"):
            raise SystemExit(why)
        print("NOT RUN: " + why)
        return False
    with tempfile.TemporaryDirectory(prefix="triforce-setup-test-") as tmp:
        exe = os.path.join(tmp, "fixture.exe")
        cmd = [csc, "/nologo", "/target:exe", "/langversion:latest", "/nullable:enable", "/optimize+", "/utf8output",
               "/codepage:65001", "/nowarn:CS8618,CS8632,CS1701,CS1702", "/out:" + exe]
        cmd += ["/reference:" + os.path.join(FRAMEWORK, r) for r in REFERENCES]
        cmd += [str(ROOT / "setup" / "Guard.cs")] + [str(ROOT / s) for s in sources] + [str(ROOT / "tests" / fixture)]
        if extra:
            Path(tmp, "cut.cs").write_text(extra, encoding="utf-8")
            cmd.append(os.path.join(tmp, "cut.cs"))
        subprocess.run(cmd, check=True, cwd=tmp, timeout=timeout)
        subprocess.run([exe], check=True, cwd=tmp, timeout=timeout)
    return True
