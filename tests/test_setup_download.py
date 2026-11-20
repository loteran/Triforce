"""Triforce Setup's downloads go on from where they were cut off, and are checked before they are used (setup/Guard.cs
Download; no Setup is started and no network is used).

A download that broke halfway started over from its first byte at the next try (the 284 MB of the tools), a connection
that went silent was waited for without end, and the file was checked against the server's Content-Length alone.

Compiles Guard.cs with tests/setup_download_test.cs and runs it against a server that is an object in the test program:
a cut download keeps its part and the next one asks for the rest (Range) with the server's mark for the file
(If-Range); the whole file comes instead, and is not joined on, when the server knows no ranges, the file was published
again, the part was begun for another address or size, or nothing says what it is a part of; a size other than the
release's, a file that came short or long; Cancel and a silent server end a read that waits on the network; room is
asked for what is still to come. Reads the source for the window's side: both downloads go through it, a tools zip that
is there whole is not fetched again, one that does not unpack is.

    python -I -B tests/test_setup_download.py
"""
import sys
from pathlib import Path

RUN_ALL_SKIP = {"linux": "Triforce Setup is a Windows program (.NET Framework 4.8)"}

sys.path.insert(0, str(Path(__file__).resolve().parent))
import setup_cs  # noqa: E402


def check_source():
    shell = setup_cs.source("setup/Shell.cs")
    merge = shell[shell.index("async Task Merge()"):shell.index("void Ui(Action a)")]
    for needle, why in (
            ("await Download.ToFile(http, release.SetupUrl, fresh, release.SetupSize,", "Setup's own update is no longer fetched through Guard.cs's Download, with the size the release lists"),
            ("await Download.ToFile(http, release.ToolsUrl, zip, release.ToolsSize,", "the tools are no longer fetched through Guard.cs's Download, with the size the release lists"),
            ("var whole = release.ToolsSize > 0 && Length(zip) == release.ToolsSize;", "a tools zip that is there whole is fetched again"),
            ("catch (Exception e) when (e is not NoRoomException && !Space.Full(e))", "a zip that does not unpack is kept, and tried again at every start"),
            ('Directory.GetFiles(cache, "Triforce-Tools-*")', "the parts other releases' downloads left are kept for ever")):
        setup_cs.need(merge, needle, "setup/Shell.cs Merge: " + why)
    if "GetAsync(" in merge or "Release.Download" in shell + setup_cs.source("setup/Setup.cs"):
        raise SystemExit("setup: a download goes past Guard.cs's Download")


def main():
    check_source()
    setup_cs.run_fixture("setup_download_test.cs")


if __name__ == "__main__":
    main()
