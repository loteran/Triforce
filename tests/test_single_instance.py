"""One game at a time for a game folder, and one Hookshot Builder at a time for its build tools (handoff §5).

The game: host/hos_instance.h is compiled as it is into a small program, and real processes are run against real folders:
the lock's name is one for each spelling of a folder (case, '/' or '\\', a separator at the end, "\\\\?\\" in front) and another
for another folder ((Triforce) "\\\\?\\UNC\\server\\share" and \\\\server\\share, "C:" and "C:\\", whatever the working
directory, and the folder of a game at a drive's root is "C:\\"); a second taker of a held folder is refused and told the
holder's process id and executable, and the message names the folder and the holder; another folder is not refused (the
lab's copy and the owner's game); a taker that waits gets the folder of a holder that ends meanwhile (a restart, a close
just before); and the folder of a holder that was killed is taken. The source is read for what only the running game would show: host/hos_main.c takes the lock after a
restart's wait for the run before it and before the log is opened and rotated, HOS_SINGLE_INSTANCE=0 leaves it out, and the
builder's --prepare-shaders run is refused without a message box, as when there is no Vulkan (exit 3).

The builder: builder/HookshotBuilder/Instance.cs is compiled alone with a console program (the compiler tests/setup_cs.py
finds, .NET Framework's references: the file uses nothing of .NET 8's) and run the same way; App.xaml.cs asks it before any
window or build, its --cli mode included.

    python -I -B tests/test_single_instance.py [--cc clang]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"linux": "the locks are Windows' named mutexes"}

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
fails = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        fails.append(what)


C_FIXTURE = r"""
#include "hos_instance.h"
int main(int argc, char **argv) {
    (void)argc;
    wchar_t folder[1100];
    MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, folder, 1100);
    if (!strcmp(argv[1], "folder")) {
        hos_instance_folder_of(folder);
        printf("%ls\n", folder);
        return 0;
    }
    if (!strcmp(argv[1], "name")) {
        wchar_t name[96];
        hos_instance_name(folder, name, 96);
        printf("%ls\n", name);
        return 0;
    }
    HosInstanceWho who;
    const int got = hos_instance_take(folder, (DWORD)atoi(argv[3]), &who);
    if (!strcmp(argv[1], "hold")) {
        printf("%d\n", got);
        fflush(stdout);
        char buf[16];
        while (fread(buf, 1, sizeof buf, stdin) > 0) {}   /* (held until the test closes stdin) */
        return 0;
    }
    wchar_t text[2600];
    hos_instance_message(folder, &who, text, 2600);
    char exe[3000], msg[8000];
    WideCharToMultiByte(CP_UTF8, 0, who.exe, -1, exe, sizeof exe, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, text, -1, msg, sizeof msg, NULL, NULL);
    printf("%d|%lu|%s|%s\n", got, (unsigned long)who.pid, exe, msg);
    return 0;
}
"""

CS_FIXTURE = r"""
using System;
using System.IO;
namespace HookshotBuilder;
static class Fixture {
    static int Main(string[] a) {
        if (a[0] == "name") { Console.WriteLine(Instance.Name(a[1])); return 0; }
        var got = Instance.Take(a[1], int.Parse(a[2]));
        if (a[0] == "hold") { Console.WriteLine(got ? "1" : "0"); Console.Out.Flush(); Console.In.ReadToEnd(); return 0; }
        Console.WriteLine((got ? "1" : "0") + "|0||" + Instance.Refusal(a[1]).Replace("\n", " "));
        return 0;
    }
}
"""


def exercise(label, cmd, folder, other, holder_named):
    """the lock between real processes: cmd + [mode, folder, wait_ms]"""

    def run(mode, path, wait=0):
        r = subprocess.run(cmd + [mode, path] + ([] if mode == "name" else [str(wait)]), capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=120)
        return r.stdout.strip()

    def hold(path):
        p = subprocess.Popen(cmd + ["hold", path, "0"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                             encoding="utf-8")
        return p, p.stdout.readline().strip()

    names = {run("name", s) for s in (folder, folder.lower() + "\\", folder.upper().replace("\\", "/"))}
    if label == "game":
        names.add(run("name", "\\\\?\\" + folder))
    check(len(names) == 1, "%s: one name for each spelling of a folder %s" % (label, sorted(names)))
    check(run("name", other) not in names, "%s: another name for another folder" % label)

    p, said = hold(folder)
    try:
        check(said == "1", "%s: the first one takes the folder" % label)
        got = run("try", folder.lower() + "\\").split("|")
        check(got[0] == "0", "%s: a second one for the same folder (spelled otherwise) is refused" % label)
        if holder_named:
            check(got[1] == str(p.pid), "%s: and told the holder's process id (%s, holder %d)" % (label, got[1], p.pid))
            check(got[2].lower().endswith(".exe"), "%s: and its executable (%s)" % (label, got[2]))
            check(str(p.pid) in got[3] and folder.lower() in got[3].lower(), "%s: its message names the folder and the holder" % label)
        else:
            check(folder.lower() in got[3].lower(), "%s: its message names the folder" % label)
        check(run("try", other).split("|")[0] == "1", "%s: another folder is not refused" % label)
        t0 = time.monotonic()
        waiter = subprocess.Popen(cmd + ["try", folder, "8000"], stdout=subprocess.PIPE, text=True, encoding="utf-8")
        time.sleep(1.0)
        p.stdin.close()   # (the holder ends while the second one waits)
        p.wait(timeout=60)
        out = waiter.communicate(timeout=60)[0].strip().split("|")
        check(out[0] == "1" and time.monotonic() - t0 < 8,
              "%s: one that waits gets the folder of a holder that ends meanwhile" % label)
    finally:
        if p.poll() is None:
            p.kill()
    p, said = hold(folder)
    p.kill()   # (TerminateProcess: the mutex is abandoned)
    p.wait(timeout=60)
    check(run("try", folder).split("|")[0] == "1", "%s: the folder of a holder that was killed is taken" % label)


def game_spellings(exe, cwds):
    """(Triforce) the game's lock name is one for each spelling of a folder whatever the working directory: "\\\\?\\UNC\\" is
    \\\\server\\share (it was "UNC\\server\\share" under the working directory), a drive alone is its root (it was the working
    directory on that drive), and the folder of a game at the root of a drive is "C:\\", not "C:" (host/hos_main.c)"""

    def out(mode, path, cwd):
        r = subprocess.run([exe, mode, path], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120,
                           cwd=cwd)
        return r.stdout.strip()

    def names(spellings):
        return {out("name", s, c) for s in spellings for c in cwds}

    unc = names(["\\\\?\\UNC\\server\\share\\Game", "\\\\?\\unc\\server\\share\\Game\\", "\\\\server\\share\\Game",
                 "//server/share/Game/"])
    check(len(unc) == 1, "game: one name for \\\\?\\UNC\\server\\share\\Game and \\\\server\\share\\Game, from any working "
                         "directory %s" % sorted(unc))
    check(not unc & names(["\\\\server\\share\\Other", "\\\\?\\UNC\\server\\other\\Game"]),
          "game: another name for another share's folder")
    drive = os.path.splitdrive(cwds[0])[0][:1] or "C"   # (the working directories' own drive: "X:" alone meant them)
    root = names([drive + ":", drive + ":\\", drive + ":\\\\", drive.lower() + ":/", "\\\\?\\" + drive + ":\\",
                  "\\\\?\\" + drive + ":"])
    check(len(root) == 1, "game: one name for %s: and %s:\\ (and %s:\\\\, \\\\?\\%s:\\), from any working directory %s"
          % (drive, drive, drive, drive, sorted(root)))
    check(not root & names([cwds[0]]), "game: a drive's root is not the working directory on that drive")
    for path, want in (("C:\\Triforce.exe", "C:\\"), ("C:\\Games\\Triforce\\Triforce.exe", "C:\\Games\\Triforce"),
                       ("\\\\?\\D:\\Triforce.exe", "\\\\?\\D:\\"), ("\\\\server\\share\\Triforce.exe", "\\\\server\\share"),
                       ("C:/Games/Triforce.exe", "C:/Games")):
        got = out("folder", path, cwds[0])
        check(got == want, "game: the folder of %s is %s (%s)" % (path, want, got))


def check_game_source():
    main = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    game = main[main.index("static int game_main(int argc, char **argv) {"):]
    call = game.index("one_game_per_folder(argc, argv, restart)")
    check(game.index('getenv("HOS_RESTART_OF")') < call < game.index("triforce_log.%d.txt"),
          "hos_main.c: the folder's lock is taken after a restart's wait and before the log is rotated and opened")
    body = main[main.index("static int one_game_per_folder("):main.index("int main(int argc, char **argv) {")]
    check('getenv("HOS_SINGLE_INSTANCE")' in body and '!strcmp(e, "0")' in body, "hos_main.c: HOS_SINGLE_INSTANCE=0 leaves the lock out")
    check("hos_game_path_w(" in body, "hos_main.c: the lock is named after the game folder (hos_game_path_w: the folder above bin)")
    check("hos_instance_folder_of(folder);" in body,
          "hos_main.c: the game's folder is hos_instance_folder_of's (a drive's root keeps its separator)")
    check("MessageBoxW(" in body and "--prepare-shaders" in body and "HOS_HEADLESS" in body,
          "hos_main.c: a refused copy says so in a message box, and on stdout for --prepare-shaders and HOS_HEADLESS")
    check('if (!strcmp(argv[i], "--prepare-shaders")) return 3;' in game[call:call + 600],
          "hos_main.c: a refused --prepare-shaders run ends as one with no Vulkan (3: the game prepares at its start)")
    docs = (ROOT / "docs" / "development.md").read_text(encoding="utf-8")
    check("`HOS_SINGLE_INSTANCE=0`" in docs and "`HOOKSHOT_SINGLE_INSTANCE=0`" in docs,
          "docs/development.md's switches table has HOS_SINGLE_INSTANCE and HOOKSHOT_SINGLE_INSTANCE")


def check_builder_source():
    app = (ROOT / "builder" / "HookshotBuilder" / "App.xaml.cs").read_text(encoding="utf-8-sig")
    start = app[app.index("protected override void OnStartup("):app.index("static int Cli(")]
    check(start.index("Instance.Take(tools.Root)") < start.index("Cli(e.Args") < start.index("base.OnStartup(e)"),
          "App.xaml.cs: the builder asks for its tools' lock before a --cli build and before its window")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc")
    a = ap.parse_args()
    check_game_source()
    check_builder_source()
    if os.name != "nt":
        print("NOT RUN: the locks themselves (Windows only)")
    else:
        cc = shutil.which("clang") or a.cc
        import setup_cs
        csc = setup_cs.find_csc()
        with tempfile.TemporaryDirectory(prefix="triforce-instance-") as tmp:
            game_dir, other = os.path.join(tmp, "Game Folder"), os.path.join(tmp, "Lab Game")
            os.makedirs(game_dir), os.makedirs(other)
            Path(tmp, "f.c").write_text(C_FIXTURE, encoding="utf-8")
            exe = os.path.join(tmp, "instance_fixture.exe")
            subprocess.run([cc, "-O1", "-I", str(ROOT / "host"), os.path.join(tmp, "f.c"), "-o", exe, "-luser32"], check=True,
                           timeout=300)
            game_spellings(exe, [game_dir, other])
            exercise("game", [exe], game_dir, other, True)
            if not csc:
                why = "no C# compiler here (tests/setup_cs.py): the builder's lock was not run"
                if os.environ.get("CI"):
                    raise SystemExit(why)
                print("NOT RUN: " + why)
            else:
                Path(tmp, "f.cs").write_text(CS_FIXTURE, encoding="utf-8")
                bexe = os.path.join(tmp, "builder_fixture.exe")
                subprocess.run([csc, "/nologo", "/target:exe", "/langversion:latest", "/nullable:enable", "/out:" + bexe,
                                "/reference:" + os.path.join(setup_cs.FRAMEWORK, "System.dll"),
                                str(ROOT / "builder" / "HookshotBuilder" / "Instance.cs"), os.path.join(tmp, "f.cs")],
                               check=True, timeout=300, stdout=subprocess.DEVNULL)
                exercise("builder", [bexe], game_dir, other, False)
    if fails:
        raise SystemExit("%d failed" % len(fails))
    print("all passed")


if __name__ == "__main__":
    main()
