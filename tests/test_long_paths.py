"""Long Windows paths (handoff §2): the manifests say longPathAware, and the game folder's path helpers find a folder of 250
characters and more.

The manifests: the game's (nx2/win_manifest.py, linked into the exe), Hookshot Builder's (builder/HookshotBuilder/app.manifest),
its setup's (builder/setup/setup.manifest) and Triforce Setup's (setup/build.ps1) each have longPathAware true, the game's
still with its UTF-8 code page. Triforce Setup is .NET Framework, whose own path checks stay at 260 characters in an exe csc
builds (no TargetFramework attribute: taken as written for 4.0) whatever its manifest says: setup/Setup.cs sets the two
AppContext switches 4.6.2 defaults to, first thing in Main. On Windows a small program with the same two lines and Setup's
manifest is compiled and writes a file at 300 characters and more (where Windows' own LongPathsEnabled is on: without it no
manifest helps, and that part says it was not run), and the same program with TRIFORCE_LEGACY_PATHS=1 is refused, as
Setup was.

The helpers: host/hos_paths.h (hos_game_path_a, hos_game_path_w) and gpu/vk/vk_paths.h (game_module_path) are compiled as they
are, with GetModuleFileName answering a path the test chooses, and run against real folders: bin\\Triforce.exe in a folder of
some 270 characters with image.bin in it is that folder's Triforce.exe (it was taken for its bin folder: the probe for
image.bin was a MAX_PATH buffer), without image.bin it stays the bin path, and a short folder is as it always was.

    python -I -B tests/test_long_paths.py [--cc clang] [--cxx clang++]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc", "--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
sys.path.insert(0, str(ROOT / "tests"))
import win_manifest  # noqa: E402

WS2016 = "{http://schemas.microsoft.com/SMI/2016/WindowsSettings}"
WS2019 = "{http://schemas.microsoft.com/SMI/2019/WindowsSettings}"
fails = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        fails.append(what)


def manifest_settings(text):
    """{tag: text} of a manifest's windowsSettings, the namespace kept in the tag"""
    root = ET.fromstring(text.encode("utf-8") if isinstance(text, str) else text)
    out = {}
    for el in root.iter():
        if el.tag.endswith("}windowsSettings"):
            for s in el:
                out[s.tag] = (s.text or "").strip()
    return out


def check_manifests():
    game = manifest_settings(win_manifest.MANIFEST)
    check(game.get(WS2016 + "longPathAware") == "true", "the game's manifest (nx2/win_manifest.py) has longPathAware true")
    check(game.get(WS2019 + "activeCodePage") == "UTF-8", "the game's manifest keeps its UTF-8 code page")
    check(win_manifest.MANIFEST in win_manifest.res_bytes(), "the game's .res carries that manifest")
    for rel in ("builder/HookshotBuilder/app.manifest", "builder/setup/setup.manifest"):
        s = manifest_settings((ROOT / rel).read_text(encoding="utf-8-sig"))
        check(s.get(WS2016 + "longPathAware") == "true", "%s has longPathAware true" % rel)
    ps = (ROOT / "setup" / "build.ps1").read_text(encoding="utf-8-sig")
    at = ps.rindex("<?xml", 0, ps.index('name="TriforceSetup"'))
    manifest = ps[at:ps.index("</assembly>", at) + len("</assembly>")]
    s = manifest_settings(manifest)
    check(s.get(WS2016 + "longPathAware") == "true", "setup/build.ps1's manifest for Triforce Setup has longPathAware true")
    return manifest


def check_setup_source():
    text = (ROOT / "setup" / "Setup.cs").read_text(encoding="utf-8-sig")
    main = text[text.index("static int Main("):]
    switches = ('AppContext.SetSwitch("Switch.System.IO.UseLegacyPathHandling", false);',
                'AppContext.SetSwitch("Switch.System.IO.BlockLongPaths", false);')
    first_use = min(main.index(n) for n in ("Install.", "Path.", "File.", "Directory.") if n in main)
    for sw in switches:
        check(sw in main and main.index(sw) < first_use,
              "setup/Setup.cs Main sets %s before any path is looked at" % sw.split('"')[1])
    return switches


def run_netfx(manifest, switches):
    """The switches' effect, in a program built as Setup is (csc, no TargetFramework attribute, Setup's manifest)"""
    if os.name != "nt":
        return
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\FileSystem") as k:
            enabled = winreg.QueryValueEx(k, "LongPathsEnabled")[0] == 1
    except OSError:
        enabled = False
    import setup_cs
    csc = setup_cs.find_csc() or os.path.join(setup_cs.FRAMEWORK, "csc.exe")
    if not enabled or not os.path.isfile(csc):
        print("NOT RUN: .NET Framework long paths (%s)" % ("no csc" if enabled else "Windows' LongPathsEnabled is off on this PC"))
        return
    program = r"""
using System; using System.IO;
static class P {
    static int Main(string[] a) {
        if (Environment.GetEnvironmentVariable("TRIFORCE_LEGACY_PATHS") != "1") {
            %s
            %s
        }
        var deep = a[0];
        while (deep.Length < 300) deep = Path.Combine(deep, "abcdefghijklmnopqrstuvwxyz0123456789");
        try {
            Directory.CreateDirectory(deep);
            var f = Path.Combine(deep, "file.txt");
            File.WriteAllText(f, "whole");
            return File.ReadAllText(f) == "whole" && f.Length > 300 ? 0 : 2;
        } catch (PathTooLongException) { return 3; }
    }
}
""" % switches
    with tempfile.TemporaryDirectory(prefix="triforce-longpath-") as tmp:
        Path(tmp, "p.cs").write_text(program, encoding="utf-8")
        Path(tmp, "app.manifest").write_text(manifest, encoding="utf-8")
        exe = os.path.join(tmp, "p.exe")
        subprocess.run([csc, "/nologo", "/target:exe", "/out:" + exe, "/win32manifest:" + os.path.join(tmp, "app.manifest"),
                        os.path.join(tmp, "p.cs")], check=True, cwd=tmp, timeout=300, stdout=subprocess.DEVNULL)
        env = dict(os.environ)
        env.pop("TRIFORCE_LEGACY_PATHS", None)
        r = subprocess.run([exe, os.path.join(tmp, "new")], cwd=tmp, env=env, timeout=120)
        check(r.returncode == 0, "Setup's switches: a file at 300 characters and more is written and read (exit %d)" % r.returncode)
        env["TRIFORCE_LEGACY_PATHS"] = "1"
        r = subprocess.run([exe, os.path.join(tmp, "old")], cwd=tmp, env=env, timeout=120)
        check(r.returncode == 3, "without them (TRIFORCE_LEGACY_PATHS=1) .NET Framework refuses it, as Setup did (exit %d)" % r.returncode)


C_FIXTURE = r"""
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
static char fake_a[4096];
static wchar_t fake_w[4096];
static DWORD stub_a(HMODULE m, char *b, DWORD n) {
    (void)m;
    size_t l = strlen(fake_a);
    if (l >= n) { memcpy(b, fake_a, n); return n; }
    memcpy(b, fake_a, l + 1);
    return (DWORD)l;
}
static DWORD stub_w(HMODULE m, wchar_t *b, DWORD n) {
    (void)m;
    size_t l = wcslen(fake_w);
    if (l >= n) { wmemcpy(b, fake_w, n); return n; }
    wmemcpy(b, fake_w, l + 1);
    return (DWORD)l;
}
#define GetModuleFileNameA stub_a
#define GetModuleFileNameW stub_w
#include "hos_paths.h"
int main(int argc, char **argv) {
    /* argv[1]: the exe's path as GetModuleFileName gives it (UTF-8), argv[2]: what the helpers should answer */
    (void)argc;
    snprintf(fake_a, sizeof fake_a, "%s", argv[1]);
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, fake_w, 4096);
    char a[2048];
    wchar_t w[2048], want_w[2048];
    MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, want_w, 2048);
    const DWORD na = hos_game_path_a(a, sizeof a);
    const DWORD nw = hos_game_path_w(w, 2048);
    int bad = 0;
    if (na != strlen(argv[2]) || strcmp(a, argv[2])) fprintf(stderr, "hos_game_path_a: %s\n", a), bad = 1;
    if (nw != wcslen(want_w) || wcscmp(w, want_w)) fprintf(stderr, "hos_game_path_w: %ls\n", w), bad = 1;
    return bad;
}
"""

CXX_FIXTURE = r"""
#include <cstdio>
#include <cstring>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static char fake_a[4096];
static DWORD stub_a(HMODULE, char* b, DWORD n) {
    size_t l = strlen(fake_a);
    if (l >= n) { memcpy(b, fake_a, n); return n; }
    memcpy(b, fake_a, l + 1);
    return (DWORD)l;
}
#define GetModuleFileNameA stub_a
#include "vk_paths.h"
int main(int, char** argv) {
    snprintf(fake_a, sizeof fake_a, "%s", argv[1]);
    char a[2048];
    const DWORD n = mkwvk::game_module_path(a, sizeof a);
    if (n != strlen(argv[2]) || strcmp(a, argv[2])) { fprintf(stderr, "game_module_path: %s\n", a); return 1; }
    return 0;
}
"""


def long_dir(base, length):
    """a folder (made) whose path is about length characters, through \\\\?\\ so that making it needs no setting of the PC's"""
    path = base
    i = 0
    while len(path) < length:
        path = os.path.join(path, "folder_%02d_abcdefghijklmnopqrstuvwxyz" % i)
        i += 1
    os.makedirs("\\\\?\\" + path, exist_ok=True)
    return path


def run_helpers(cc, cxx):
    if os.name != "nt":
        print("NOT RUN: the helpers' Windows half (Windows only)")
        return
    with tempfile.TemporaryDirectory(prefix="triforce-longpath-") as tmp:
        build = os.path.join(tmp, "build")
        os.makedirs(build)
        Path(build, "c.c").write_text(C_FIXTURE, encoding="utf-8")
        Path(build, "x.cpp").write_text(CXX_FIXTURE, encoding="utf-8")
        cexe, xexe = os.path.join(build, "c.exe"), os.path.join(build, "x.exe")
        subprocess.run([cc, "-O1", "-I", str(ROOT / "host"), os.path.join(build, "c.c"), "-o", cexe], check=True, timeout=300)
        subprocess.run([cxx, "-std=c++20", "-O1", "-I", str(ROOT / "gpu" / "vk"), os.path.join(build, "x.cpp"), "-o", xexe],
                       check=True, timeout=300)

        def case(what, exe_path, want):
            for helper, exe in (("hos_game_path_a/_w", cexe), ("game_module_path", xexe)):
                r = subprocess.run([exe, exe_path, want], timeout=60, capture_output=True, text=True, encoding="utf-8",
                                   errors="replace")
                check(r.returncode == 0, "%s: %s %s" % (helper, what, r.stderr.strip()))

        deep = long_dir(os.path.join(tmp, "g"), 270)
        os.makedirs("\\\\?\\" + os.path.join(deep, "bin"), exist_ok=True)
        exe_path = os.path.join(deep, "bin", "Triforce.exe")
        case("a %d-character folder without image.bin: the bin path as it is" % len(deep), exe_path, exe_path)
        with open("\\\\?\\" + os.path.join(deep, "image.bin"), "wb") as f:
            f.write(b"x")
        case("a %d-character folder with image.bin: the folder's Triforce.exe" % len(deep), exe_path,
             os.path.join(deep, "Triforce.exe"))
        short = os.path.join(tmp, "s")
        os.makedirs(os.path.join(short, "bin"))
        Path(short, "image.bin").write_bytes(b"x")
        case("a short folder with image.bin, as before", os.path.join(short, "bin", "Triforce.exe"),
             os.path.join(short, "Triforce.exe"))
        case("an exe not in bin, as before", os.path.join(short, "Triforce.exe"), os.path.join(short, "Triforce.exe"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc")
    ap.add_argument("--cxx")
    a = ap.parse_args()
    manifest = check_manifests()
    switches = check_setup_source()
    run_netfx(manifest, switches)
    cc = a.cc if a.cc and shutil.which(a.cc) and os.name != "nt" else shutil.which("clang") or a.cc
    cxx = a.cxx if a.cxx and shutil.which(a.cxx) and os.name != "nt" else shutil.which("clang++") or a.cxx
    run_helpers(cc, cxx)
    if fails:
        raise SystemExit("%d failed" % len(fails))
    print("all passed")


if __name__ == "__main__":
    main()
