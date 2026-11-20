"""NX2navi's packed RomFS is refused (nx2/make_game.py refuse_packed_romfs, setup/Shell.cs CheckSource).

NX2navi keeps the game's RomFS as one image, <its game folder>\\romfs\\romfs.bin (+ index.json). Chosen as Setup's romfs folder it
passed (only the exefs was checked), the game was built without its data and crashed at start (two players, 11-18). Now:
- a romfs folder holding romfs.bin is refused, with a message that names NX2navi and what to choose instead;
- a folder of the game's files is not;
- the check, the build from folders and an update each refuse it, and Setup's check passes the romfs folder.
"""

import importlib.util
import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_make_game():
    sys.path.insert(0, str(ROOT / "nx2"))
    try:
        spec = importlib.util.spec_from_file_location("make_game_packed_romfs", ROOT / "nx2" / "make_game.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path.remove(str(ROOT / "nx2"))


mg = load_make_game()


class PackedRomfsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="packed-romfs-")
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_nx2navi_folder_refused(self):
        romfs = self.dir / "NX2navi" / "romfs"
        romfs.mkdir(parents=True)
        (romfs / "romfs.bin").write_bytes(b"\0" * 64)
        (romfs / "index.json").write_text("{}")
        out = io.StringIO()
        with redirect_stdout(out), self.assertRaises(SystemExit) as e:
            mg.refuse_packed_romfs(str(romfs))
        self.assertEqual(e.exception.code, 1)
        line = out.getvalue().strip()
        self.assertTrue(line.startswith("@@error "), line)   # (what Setup shows under the game files: Setup.cs Check)
        self.assertIn("NX2navi", line)
        self.assertIn("romfs.bin", line)
        self.assertIn(".nup", line)

    def test_unpacked_folder_passes(self):
        romfs = self.dir / "romfs"
        (romfs / "System").mkdir(parents=True)
        (romfs / "System" / "AocMainField.byml").write_bytes(b"x")
        mg.refuse_packed_romfs(str(romfs))   # (no exit)
        mg.refuse_packed_romfs(None)         # (the .nup route: no folder)

    def test_every_route_refuses(self):
        src = (ROOT / "nx2" / "make_game.py").read_text(encoding="utf-8")
        self.assertIn("refuse_packed_romfs(a.romfs)   # (Setup passes", src)                 # the check
        self.assertIn("refuse_packed_romfs(romfs)   # (a folder built from NX2navi's", src)  # an update
        self.assertIn("refuse_packed_romfs(a.romfs)\n        refuse_dump_links(a.exefs)", src)  # the build from folders
        shell = (ROOT / "setup" / "Shell.cs").read_text(encoding="utf-8")
        self.assertIn('"--exefs", ExefsPath.Text, "--romfs", RomfsPath.Text', shell)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
