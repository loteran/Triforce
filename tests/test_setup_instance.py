"""One Triforce Setup at a time for an install (setup/Guard.cs Instance; no Setup is started, no install touched).

Two Setups on one folder each downloaded the tools into the same files, unpacked them over each other and built into the
same game folder. The first one now holds a lock named after its folder; a second start asks it to the front and ends.

Compiles Guard.cs with tests/setup_instance_test.cs and runs it: the lock's name (one for a folder however it is typed,
another for another folder and for a test run's TRIFORCE_TEST_ROOT), and the lock itself between real processes: a
second start refused and its call heard, the folder let go for a successor and taken again, and a holder that was
killed. Reads the source for what only a started Setup would show: that the lock is taken before the window is made and
before an uninstall, and let go before the Setup that takes over is started.

    python -I -B tests/test_setup_instance.py
"""
import sys
from pathlib import Path

RUN_ALL_SKIP = {"linux": "Triforce Setup is a Windows program (.NET Framework 4.8)"}

sys.path.insert(0, str(Path(__file__).resolve().parent))
import setup_cs  # noqa: E402


def check_source():
    main = setup_cs.source("setup/Setup.cs")
    main = main[main.index("static int Main("):main.index("/// This build's release tag")]
    setup_cs.need(main, "var home = Install.InstalledDir ?? Install.SelfDir;", "the lock is no longer named after the folder Setup works in")
    take = "Instance.Take(home, Install.TestRoot)"
    if main.count(take) != 3:
        raise SystemExit("setup/Setup.cs: the uninstall, --convert-saves and the window no longer each take the folder's lock, with the test folder")
    if not main.index(take) < main.index("Install.Uninstall("):
        raise SystemExit("setup/Setup.cs: an uninstall no longer asks for the lock first: it would remove the tools under an open Setup")
    if not main.rindex(take) < main.index("new Application") < main.index("new Shell(args)"):
        raise SystemExit("setup/Setup.cs: the window is made before the lock is asked for: a second Setup would start its work")
    for needle, why in (
            ("if (shots < 0)", "--snapshot, which only draws the pages, now waits for the lock"),
            ("Instance.CallForward(home, Install.TestRoot)", "a second start no longer asks the first to the front"),
            ("Instance.Listen(() => w.Window.Dispatcher.BeginInvoke(new Action(w.ComeForward)));", "the first Setup no longer listens for a second start"),
            ("w.Window.Closed += (_, _) => Instance.Release();", "the lock is no longer let go when the window has closed")):
        setup_cs.need(main, needle, "setup/Setup.cs: " + why)
    shell = setup_cs.source("setup/Shell.cs")
    relaunch = shell[shell.index("Process? Relaunch("):shell.index("bool sessionEnding;")]
    if not relaunch.index("Instance.Release();") < relaunch.index("Process.Start("):
        raise SystemExit("setup/Shell.cs: the Setup that takes over (after an install or its own update) is started while this one "
                         "still holds the folder: it would take itself for a second Setup and end")
    setup_cs.need(relaunch, "catch (Exception) { Instance.TakeAgain(); throw; }", "setup/Shell.cs: a Setup whose successor did not start goes on without the lock")
    setup_cs.need(setup_cs.source("setup/build.ps1"), '"$here\\Guard.cs"', "setup/build.ps1 no longer compiles Guard.cs into Setup")


def main():
    check_source()
    setup_cs.run_fixture("setup_instance_test.cs")


if __name__ == "__main__":
    main()
