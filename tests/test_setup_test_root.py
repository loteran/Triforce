"""A test run of Triforce Setup (TRIFORCE_TEST_ROOT) builds inside its own folder, work folder and all (setup/Setup.cs
Runner and Install.Allowed; no Setup and no make_game.py is started).

TRIFORCE_TEST_ROOT keeps a look at Setup away from the PC's real install: a build or an update into a folder outside it
is refused. Its build was still started without --work, so make_game.py worked in the PC's real build folder
(%LOCALAPPDATA%\\Triforce\\build), where its first act is to stop whatever build is running there: a player's, or a
developer's own.

Compiles the Runner class and Install.Allowed, cut out of Setup.cs, with tests/setup_test_root_test.cs, whose program
stands in for the tools' Python and writes down what it was started with: a test run's build and update get the test
folder's own build folder (once, and not over one that was named), one into a folder outside is refused before anything
starts, and a player's build is given no --work at all. Reads the source for the uninstall's side: the folder a test
run's uninstall removes is the same one.

    python -I -B tests/test_setup_test_root.py
"""
import sys
from pathlib import Path

RUN_ALL_SKIP = {"linux": "Triforce Setup is a Windows program (.NET Framework 4.8)"}

sys.path.insert(0, str(Path(__file__).resolve().parent))
import setup_cs  # noqa: E402

USING = ("System", "System.Collections.Generic", "System.Diagnostics", "System.IO", "System.Linq", "System.Text", "System.Threading.Tasks")


def main():
    setup = setup_cs.source("setup/Setup.cs")
    setup_cs.need(setup, 'var work = TestRoot != null ? Path.Combine(TestRoot, "build") :',
                  "setup/Setup.cs Uninstall: a test run's uninstall no longer removes the build folder its builds work in")
    allowed = setup[setup.index("    /// May this run write there?"):setup.index("    static string LocalData =>")]
    runner = setup[setup.index("/// nx2/make_game.py as a child process"):setup.index("/// Folder picker")]
    extra = "".join("using %s;\n" % u for u in USING) + "namespace TriforceSetup;\nstatic partial class Install\n{\n" + allowed + "}\n\n" + runner
    setup_cs.run_fixture("setup_test_root_test.cs", extra=extra)


if __name__ == "__main__":
    main()
