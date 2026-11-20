"""The builder's setup keeps the installed version whole when an update stops (builder/setup/setup.c; no network, no release).

An update moves the folders of parts that did not change from the installed version into the new one (reuse_part) instead
of downloading them. A failure, a closed window or a setup cut off left them in <tag>.part, which the next run deleted with
them: the version still installed had lost its Python or its compiler. Now each move is written down first
(<tag>.part\\reused.txt), and undo_reuse moves them back: in fail(), and at the next run's start before <tag>.part goes.
(Triforce) At that start every versions\\*.part with a note is undone (undo_every_reuse), not only the one of the release set
up now: a cut-off setup of another release left the installed version without its folders, and it ran without them.

Cuts setup.c's own functions (the Part list's reading, mkdirs to undo_reuse) out of it and compiles them with a small
program that runs them on authored folders in a temporary place: a part moved over and its note, then undone (back where it
was, the note gone, a folder the installed version has again left alone), a name in the note that leaves its folder ignored.
Reads the source for the calls: fail() undoes while a version is unpacked, the install undoes before it removes <tag>.part,
and the note goes with a finished install.

    python -I -B tests/test_builder_setup_rollback.py [--cc gcc]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"linux": "the builder's setup is a Windows program"}

ROOT = Path(__file__).resolve().parents[1]

PROGRAM = r"""
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
/* PART_STRUCT */
/* LIST_HELPERS */
/* ACTUAL */
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static int there(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static void put(const wchar_t *p, const char *text) {
    HANDLE f = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD n;
    WriteFile(f, text, (DWORD)strlen(text), &n, NULL);
    CloseHandle(f);
}
int wmain(int argc, wchar_t **argv) {
    (void)argc;
    wchar_t versions[MAX_PATH], v1py[MAX_PATH], part[MAX_PATH], moved[MAX_PATH], note[MAX_PATH], p[MAX_PATH];
    swprintf(versions, MAX_PATH, L"%ls\\versions", argv[1]);
    swprintf(v1py, MAX_PATH, L"%ls\\v1\\HookshotBuilder\\tools\\python", versions);
    swprintf(part, MAX_PATH, L"%ls\\v2.part", versions);
    swprintf(moved, MAX_PATH, L"%ls\\HookshotBuilder\\tools\\python", part);
    swprintf(note, MAX_PATH, L"%ls\\reused.txt", part);
    mkdirs(v1py);
    swprintf(p, MAX_PATH, L"%ls\\python.exe", v1py), put(p, "py");
    swprintf(p, MAX_PATH, L"%ls\\current.txt", versions), put(p, "v1\n");
    swprintf(p, MAX_PATH, L"%ls\\v1\\parts.txt", versions), put(p, "aaaa\n");
    mkdirs(part);
    Part pt;
    memset(&pt, 0, sizeof pt);
    strcpy(pt.sha, "aaaa"), strcpy(pt.dest, "."), strcpy(pt.from, "-"), strcpy(pt.to, "-");
    strcpy(pt.url, "https://github.com/x/y/releases/download/v2/HookshotBuilder-python-aaaa.zip");
    CHECK(undo_reuse(versions, part) == 1);   /* (no note: nothing to undo) */
    CHECK(reuse_part(versions, "v2", &pt, part) == 1);
    CHECK(!there(v1py) && there(moved) && there(note));
    CHECK(undo_reuse(versions, part) == 1);
    CHECK(there(v1py) && !there(moved) && !there(note));
    swprintf(p, MAX_PATH, L"%ls\\python.exe", v1py);
    CHECK(there(p));
    /* moved again, and the installed version has the folder again meanwhile: left alone, nothing written over */
    CHECK(reuse_part(versions, "v2", &pt, part) == 1);
    mkdirs(v1py);
    CHECK(undo_reuse(versions, part) == 1 && there(moved) && there(v1py));
    /* a note that names a path out of its folder: ignored */
    put(note, "v1\n..\\..\\evil\nHookshotBuilder\\tools\\python\n");
    CHECK(undo_reuse(versions, part) == 1);
    swprintf(p, MAX_PATH, L"%ls\\..\\evil", versions);
    CHECK(!there(p));
    /* (Triforce) a setup of another release (v2) cut off, and now v3 is set up: v2.part's note is undone too (undo_every_reuse);
       a .part without a note and a file named .part are left alone */
    DeleteFileW(note);
    swprintf(p, MAX_PATH, L"%ls\\python.exe", moved), DeleteFileW(p), RemoveDirectoryW(moved);
    swprintf(p, MAX_PATH, L"%ls\\python.exe", v1py), put(p, "py");
    CHECK(reuse_part(versions, "v2", &pt, part) == 1 && !there(v1py) && there(note));
    wchar_t plain[MAX_PATH], left[MAX_PATH] = L"";
    swprintf(plain, MAX_PATH, L"%ls\\v3.part", versions), mkdirs(plain);
    swprintf(p, MAX_PATH, L"%ls\\kept.txt", plain), put(p, "x");
    swprintf(p, MAX_PATH, L"%ls\\v5.part", versions), put(p, "a file");
    /* (held open: its folder cannot move, and the folder that still holds it is named) */
    swprintf(p, MAX_PATH, L"%ls\\python.exe", moved);
    HANDLE held = CreateFileW(p, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(held != INVALID_HANDLE_VALUE);
    CHECK(undo_every_reuse(versions, left, MAX_PATH) == 0 && !wcscmp(left, part) && there(moved) && there(note));
    CloseHandle(held);
    left[0] = 0;
    CHECK(undo_every_reuse(versions, left, MAX_PATH) == 1 && !left[0]);
    CHECK(there(v1py) && !there(moved) && !there(note));
    swprintf(p, MAX_PATH, L"%ls\\python.exe", v1py);
    CHECK(there(p));
    swprintf(p, MAX_PATH, L"%ls\\kept.txt", plain);
    CHECK(there(p));
    if (!fails) printf("PASS: what reuse_part moves is written down first, and undo_reuse moves it back\n");
    return fails != 0;
}
"""


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def check_source(src):
    fail = between(src, "static void fail(const wchar_t *fmt", "/* ---- HTTPS GET")
    if "undo_reuse(g_versions_dir, g_part_dir)" not in fail or fail.index("undo_reuse(") > fail.index("ExitProcess(1)"):
        raise SystemExit("builder/setup/setup.c fail() no longer moves back what was moved from the installed version")
    reuse = between(src, "static int reuse_part(", "/* (Triforce) The folders reuse_part moved")
    if reuse.index("reuse_note(install, was, rel)") > reuse.index("MoveFileW(from, to)"):
        raise SystemExit("builder/setup/setup.c reuse_part moves a folder before it is written down")
    every = between(src, "static int undo_every_reuse(", "/* The checksums of the parts this version holds")
    if "undo_reuse(versions, dir)" not in every:
        raise SystemExit("builder/setup/setup.c undo_every_reuse no longer undoes each <tag>.part")
    start = src[src.index('swprintf(exe, MAX_PATH, L"%ls\\\\%ls", install, wrun);'):]
    sweep = start.find("if (!undo_every_reuse(versions, left, MAX_PATH))")
    if not 0 <= sweep < start.index("if (repair || GetFileAttributesW(marker)"):
        raise SystemExit("builder/setup/setup.c looks at the installed version before what every cut-off setup moved from it "
                         "went back")
    main = src[src.index('swprintf(install, MAX_PATH, L"%ls\\\\%ls.part", versions, wtag);'):]
    if not main.index("rmtree(install);") < main.index("wcscpy(g_versions_dir, versions), wcscpy(g_part_dir, install);"):
        raise SystemExit("builder/setup/setup.c removes <tag>.part before what it holds of the installed version went back")
    done = main[main.index("if (!MoveFileW(install, final_dir))"):]
    if done.index('g_part_dir[0] = 0;') > done.index("write_small(marker, tag);") or "DeleteFileW(note);" not in done[:600]:
        raise SystemExit("builder/setup/setup.c: a finished install keeps the note, or fail() could still undo it")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    src = (ROOT / "builder/setup/setup.c").read_text(encoding="utf-8")
    check_source(src)
    if sys.platform != "win32":
        print("NOT RUN: Windows only")
        return
    part = between(src, "typedef struct {\n    char sha[65];", "/* the next non-empty line")
    helpers = between(src, "static char *next_line(char **next)", "static void status(")
    actual = between(src, "static void mkdirs(const wchar_t *path)", "/* The checksums of the parts this version holds")
    code = PROGRAM.replace("/* PART_STRUCT */", part).replace("/* LIST_HELPERS */", helpers).replace("/* ACTUAL */", actual)
    with tempfile.TemporaryDirectory(prefix="setup-rollback-") as tmp:
        c, exe = Path(tmp) / "rollback.c", Path(tmp) / "rollback.exe"
        c.write_text(code, encoding="utf-8")
        subprocess.run([a.cc, "-std=gnu11", "-O1", "-Wall", "-Wno-unused-function", "-municode", "-DUNICODE", "-D_UNICODE", str(c),
                        "-o", str(exe)], check=True)
        work = Path(tmp) / "base"
        work.mkdir()
        subprocess.run([str(exe), str(work)], check=True, timeout=60)


if __name__ == "__main__":
    main()
