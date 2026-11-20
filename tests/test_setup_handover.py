"""A Triforce Setup update that cannot start is undone (setup/Guard.cs Handover; no Setup is started, no install touched).

Setup replaces itself by renaming its running exe aside and the new one into its place, then starts the new one and
closed at once. A new Setup that could not start (a damaged download of the right size, a virus scanner's block, an
error at its start on that PC) was then all the player had under their shortcut, with the Setup that worked lying
beside it as TriforceSetup.old.exe.

The old Setup now stays, out of sight, until the new one says its window is up; one that cannot be started or ends at
once with an error is replaced by the old exe again, in one step. Compiles Guard.cs with tests/setup_handover_test.cs
and runs it with real processes and files: what is taken for up, failed, or unknown (which is left alone: a Setup that
ended without an error, one that runs on without the sign, the sign of another folder's or of the real install's
Setup); the swap undone when its second step fails; a running program renamed aside and put back over a file that is
no program, after which it starts under its name again; and a new Setup that does run, which Windows will not let be
written over. Reads the source for the window's side.

    python -I -B tests/test_setup_handover.py
"""
import sys
from pathlib import Path

RUN_ALL_SKIP = {"linux": "Triforce Setup is a Windows program (.NET Framework 4.8)"}

sys.path.insert(0, str(Path(__file__).resolve().parent))
import setup_cs  # noqa: E402


def check_source():
    shell = setup_cs.source("setup/Shell.cs")
    merge = shell[shell.index("async Task Merge()"):shell.index("void Ui(Action a)")]
    own = merge[merge.index("// 1. Setup itself"):merge.index("// 2. the tools")]
    if not own.index("Handover.Swap(fresh, self, old);") < own.index("await StartNewSetup(self, old, release.Tag);"):
        raise SystemExit("setup/Shell.cs Merge: Setup's own update no longer swaps and starts the new one through Guard.cs's Handover")
    if "File.Move(" in own or "Relaunch(" in own:
        raise SystemExit("setup/Shell.cs Merge: Setup's own update renames or restarts past Guard.cs's Handover")
    setup_cs.need(own, "if (!relaunched && SetupNewer(release) && release.SetupUrl != null)", "setup/Shell.cs Merge: a Setup that could not start on this PC is fetched again")
    setup_cs.need(shell, 'r.SetupUrl != null && Install.Get("SetupSkip") != r.Tag;', "setup/Shell.cs SetupNewer: a Setup that could not start on this PC counts as an update again")
    setup_cs.need(setup_cs.source("setup/Shell.Home.cs"), "var setupNewer = latest != null && SetupNewer(latest);",
                  "setup/Shell.Home.cs: the start page offers a Setup that could not start on this PC as an update, at every start")
    start = shell[shell.index("async Task StartNewSetup("):shell.index("public void ComeForward()")]
    order = ("Handover.Expect(Install.InstalledDir ?? Install.SelfDir, Install.TestRoot)", "Relaunch(self, true, stay: true)", "Window.Hide();", "Handover.Watch(next, up,",
             "if (outcome != Handover.Outcome.Failed || sessionEnding || !Instance.TakeAgain())", "Handover.Restore(old, self)",
             'if (back) Install.Set("SetupSkip", tag);', "Window.Show();", "throw new NotStartedException(")
    at = -1
    for needle in order:
        if needle not in start or start.index(needle) < at:
            raise SystemExit("setup/Shell.cs StartNewSetup: missing or out of its order: " + needle)
        at = start.index(needle)
    setup_cs.need(start, "if (scriptFile != null) { Relaunch(self, true); return; }", "setup/Shell.cs StartNewSetup: a scripted test run now stays beside the Setup it started")
    setup_cs.need(shell, "if (relaunched) Window.Loaded += (_, _) => Handover.Up(Install.InstalledDir ?? Install.SelfDir, Install.TestRoot);",
                  "setup/Shell.cs: the new Setup no longer says that its window is up")
    relaunch = shell[shell.index("Process? Relaunch("):shell.index("bool sessionEnding;")]
    if not relaunch.index("Instance.Release();") < relaunch.index("Process.Start(") < relaunch.index("if (stay) return next;") < relaunch.index("Window.Close();"):
        raise SystemExit("setup/Shell.cs Relaunch: the Setup that waits for its successor closes before it has heard of it")


def main():
    check_source()
    setup_cs.run_fixture("setup_handover_test.cs")


if __name__ == "__main__":
    main()
