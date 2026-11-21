"""(Triforce, 11-18) Triforce Setup updates the tools in two parts (setup/Setup.cs Tools.ParseBaseList, HasBase, UnpackParts;
setup/Shell.cs ToolsInParts; setup/pack_tools.ps1 base.list and the three zips).

Every update fetched the whole 284 MB Triforce-Tools.zip, 95 % of it the compiler, Python and the libraries, which do not
change from one release to the next; at a release the download server's line was full and each player got 75 KB/s. Now an
update of tools that hold the release's base fetches the code part alone and moves the base over.

Compiles Guard.cs, Mods.cs and Setup.cs's Tools class (cut out of it) with tests/setup_tools_parts_test.cs and runs it in a
temporary folder: the base list read, and refused when it is another format, names a path out of the folder or has a line it
does not know; a base held taken (and remembered, then not read again), one with a file of other bytes, another size or gone
not; the code part with the base kept (the old code's files gone, a file a run added to the base kept), with the base
fetched; a refused code part and a base found short after it was moved over leaving the tools that were there; the whole
zip remembering its base. Reads the source: Merge tries the parts before the whole zip, the parts go through Guard.cs's
Download, and the packer writes the list and all three zips.

    python -I -B tests/test_setup_tools_parts.py
"""
import sys
from pathlib import Path

RUN_ALL_SKIP = {"linux": "Triforce Setup is a Windows program (.NET Framework 4.8)"}

sys.path.insert(0, str(Path(__file__).resolve().parent))
import setup_cs  # noqa: E402


def check_source():
    shell = setup_cs.source("setup/Shell.cs")
    merge = shell[shell.index("async Task Merge()"):shell.index("void Ui(Action a)")]
    setup_cs.need(merge, "release.ToolsCodeUrl != null && release.ToolsBaseUrl != null &&", "setup/Shell.cs Merge: the parts are not tried")
    setup_cs.need(merge, "await ToolsInParts(http, release, have, cache, token);", "setup/Shell.cs Merge: the parts are not tried")
    if merge.index("await ToolsInParts(") > merge.index("await Download.ToFile(http, release.ToolsUrl, zip"):
        raise SystemExit("setup/Shell.cs Merge: the whole zip is fetched before the parts are tried")
    parts = shell[shell.index("async Task<bool> ToolsInParts("):]
    parts = parts[:parts.index("\n    }\n")]
    for needle in ("await Download.ToFile(http, release.ToolsCodeUrl!, code, release.ToolsCodeSize,",
                   "await Download.ToFile(http, release.ToolsBaseUrl!, baseZip, release.ToolsBaseSize,",
                   "Tools.HasBase(b, Tools.Folder,", "Tools.UnpackParts(code, keep ? null : baseZip, b, release.Tag,"):
        setup_cs.need(parts, needle, "setup/Shell.cs ToolsInParts lacks: " + needle)
    if "GetAsync(" in parts:
        raise SystemExit("setup/Shell.cs ToolsInParts: a download goes past Guard.cs's Download")
    setup = setup_cs.source("setup/Setup.cs")
    for asset in ('ToolsCodeAsset = "Triforce-Tools-code.zip"', 'ToolsBaseAsset = "Triforce-Tools-base.zip"'):
        setup_cs.need(setup, asset, "setup/Setup.cs: no " + asset)
    pack = setup_cs.source("setup/pack_tools.ps1")
    for needle in ("'llvm-mingw', 'python', 'cpm', 'wgpu-native'", "triforce-tools-base`t1`nid`t$id`n",
                   "Write-ToolsZip (Join-Path $Out 'Triforce-Tools.zip') $all",
                   "Write-ToolsZip (Join-Path $Out 'Triforce-Tools-code.zip')", "Write-ToolsZip (Join-Path $Out 'Triforce-Tools-base.zip')"):
        setup_cs.need(pack, needle, "setup/pack_tools.ps1 lacks: " + needle)


def main():
    check_source()
    tools = setup_cs.cut("setup/Setup.cs", "/// The Triforce tools beside the installed Setup", "/// nx2/make_game.py as a child process",
                         using=("System", "System.IO", "System.IO.Compression", "System.Linq"))
    setup_cs.run_fixture("setup_tools_parts_test.cs", extra=tools, sources=("setup/Mods.cs",))


if __name__ == "__main__":
    main()
