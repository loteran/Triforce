"""Triforce Setup asks for room on the disk before it needs it (setup/Guard.cs Space; no Setup is started).

A build could be started on a disk with too little room (the line under the form only turned red) and stopped somewhere
in its 45 minutes; the tools were downloaded and unpacked without any look at the disk, and a disk that filled left half
a tools folder and an error that named no cause.

Compiles Guard.cs, Setup.cs's Tools class (cut out of it) and tests/setup_space_test.cs, and runs them: the question
(what it takes with the margin against what is free, both numbers in the refusal), the disk asked for a folder that is
still to make, the error of a disk that filled, and the tools unpacked from authored zips into a temporary folder: whole;
refused before the first file when there is no room, the tools there untouched; a damaged zip's half removed. Reads the
source for the window's side: the Build button asks first and still lets the player go on, the tools' room is asked
before their download.

    python -I -B tests/test_setup_space.py
"""
import sys
from pathlib import Path

RUN_ALL_SKIP = {"linux": "Triforce Setup is a Windows program (.NET Framework 4.8)"}

sys.path.insert(0, str(Path(__file__).resolve().parent))
import setup_cs  # noqa: E402


def check_source():
    shell = setup_cs.source("setup/Shell.cs")
    click = shell[shell.index("BuildButton.Click += (_, _) =>"):shell.index('SetupBack = Part<Button>("SetupBack");')]
    if click.count("BuildIfRoom") != 2 or "StartBuild" in click:
        raise SystemExit("setup/Shell.cs: the Build button starts a build without the look at the disk")
    room = shell[shell.index("void BuildIfRoom()"):shell.index("void StartBuild()")]
    for needle, why in (
            ("Space.Free(OutPath.Text) is { } free && free < NeededBytes", "the build's room is no longer asked of the game folder's disk"),
            ("(free >> 30)", "the question no longer says how much is free"),
            ("(NeededBytes >> 30)", "the question no longer says how much a build takes"),
            ('"Build anyway", T(S.Back), StartBuild);', "the player can no longer go on at their own risk, as they could before the question"),
            ("else StartBuild();", "a disk with room no longer builds straight away")):
        setup_cs.need(room, needle, "setup/Shell.cs BuildIfRoom: " + why)
    merge = shell[shell.index("async Task Merge()"):shell.index("void Ui(Action a)")]
    tools = merge[merge.index("// 2. the tools"):merge.index("// 3. the game folder")]
    if not tools.index("await Task.Run(Tools.Sweep);") < tools.index('Space.Problem("the Triforce tools (a download of') < tools.index("release.ToolsUrl, zip,"):
        raise SystemExit("setup/Shell.cs Merge: the tools' room is no longer asked before their download, after the leavings of a cut "
                         "unpacking are gone")
    setup_cs.need(tools, "release.ToolsSize * ToolsUnpackedTimes", "setup/Shell.cs Merge: the room asked for no longer counts what the zip unpacks to")
    setup_cs.need(merge, "if (Space.Full(e))", "setup/Shell.cs Merge: a disk that filled in the middle is again told in Windows' words alone")
    setup_cs.need(shell, 'e is NoRoomException ? "Not enough room on the disk"', "setup/Shell.cs Failed: no room is again titled \"Something went wrong\"")


def main():
    check_source()
    tools = setup_cs.cut("setup/Setup.cs", "/// The Triforce tools beside the installed Setup", "/// nx2/make_game.py as a child process",
                         using=("System", "System.IO", "System.IO.Compression", "System.Linq"))
    # (Tools.Unpack checks the zip's names with Mods.Unsafe, the rule a mod's archive is checked by)
    setup_cs.run_fixture("setup_space_test.cs", extra=tools, sources=("setup/Mods.cs",))


if __name__ == "__main__":
    main()
