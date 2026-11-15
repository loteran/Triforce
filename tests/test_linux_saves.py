"""builder/linux/saves.py: finding this game's saves (NX2navi with and without a finished save-tx commit, a yuzu-style NAND,
Ryujinx) and importing one into Triforce's folder with a backup first. Fake trees only; nothing outside a temp folder."""
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "builder" / "linux"))
import saves  # noqa: E402

RUN_ALL_SKIP = {"windows": "the Linux builder's save import"}


def make_set(d, tag, slots=2, when=None):
    d = Path(d)
    (d).mkdir(parents=True, exist_ok=True)
    (d / "account.sav").write_text("account " + tag)
    (d / "option.sav").write_text("option " + tag)
    for i in range(slots):
        s = d / ("slot_%02d" % i)
        s.mkdir(exist_ok=True)
        (s / "caption.sav").write_text("c%d %s" % (i, tag))
        (s / "progress.sav").write_text("p%d %s" % (i, tag))
    if when:
        for p in d.rglob("*.sav"):
            os.utime(p, (when, when))
    return str(d)


class SavesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.env = {k: os.environ.get(k) for k in ("HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME")}
        os.environ["HOME"] = str(self.root / "home")
        os.environ["XDG_DATA_HOME"] = str(self.root / "home/.local/share")
        os.environ["XDG_CONFIG_HOME"] = str(self.root / "home/.config")
        self.never = lambda game=None: None

    def tearDown(self):
        for k, v in self.env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        self.tmp.cleanup()

    def test_navi_prefers_highest_ready(self):
        game = self.root / "navi"
        acc = "3e8ca380203d08fa72d61e5b69f8b1f2"
        make_set(game / "profile/save" / acc, "account")
        self.assertEqual(saves.navi_save(str(game)), str(game / "profile/save" / acc))
        make_set(game / "profile/save-tx/ready-0000000002", "r2")
        make_set(game / "profile/save-tx/ready-0000000010", "r10")
        (game / "profile/save-tx/ready-3").mkdir()   # (not a save set)
        self.assertEqual(saves.navi_save(str(game)), str(game / "profile/save-tx/ready-0000000010"))
        self.assertEqual(saves.find_in(str(game)), str(game / "profile/save-tx/ready-0000000010"))
        self.assertIsNone(saves.navi_save(str(self.root)))

    def test_find_nand_ryujinx_navi(self):
        data = self.root / "home/.local/share"
        make_set(data / "yuzu/nand/user/save/0000000000000000/ABCD" / saves.TITLE_ID, "yuzu", 3)
        make_set(self.root / "home/.var/app/dev.eden_emu.eden/data/eden/nand/user/save/0000000000000000/U" / saves.TITLE_ID, "eden", 1)
        make_set(self.root / "home/.config/Ryujinx/bis/user/save/0000000000000007/0", "ryu", 4)
        (self.root / "home/.config/Ryujinx/bis/user/save/0000000000000008/0").mkdir(parents=True)   # (another game's)
        make_set(self.root / "home/Games/zelda/profile/save/3e8ca380203d08fa72d61e5b69f8b1f2", "navi", 6)
        found = saves.find_saves()
        names = sorted(s.name for s in found)
        self.assertEqual(names, ["Eden (flatpak)", "NX2navi", "Ryujinx", "yuzu"])
        slots = {s.name: s.slots for s in found}
        self.assertEqual((slots["yuzu"], slots["Ryujinx"], slots["NX2navi"]), (3, 4, 6))

    def test_find_skips_triforces_own(self):
        mine = saves.triforce_folder()
        make_set(mine, "mine")
        self.assertEqual(saves.find_saves(), [])

    def test_import_with_backup(self):
        src = make_set(self.root / "src", "new", 3)
        dest = saves.triforce_folder()
        make_set(dest, "old", 1)
        backup = saves.import_saves(src, dest, running=self.never)
        self.assertTrue(backup.startswith(str(Path(dest).parents[1] / "save-backup")))
        self.assertEqual((Path(backup) / "option.sav").read_text(), "option old")
        self.assertEqual((Path(dest) / "option.sav").read_text(), "option new")
        self.assertEqual((Path(dest) / "slot_02/progress.sav").read_text(), "p2 new")
        self.assertFalse(os.path.exists(dest + ".new") or os.path.exists(dest + ".old"))
        # an import with nothing there: no backup
        shutil_dest = self.root / "fresh/save/x"
        self.assertIsNone(saves.import_saves(src, str(shutil_dest), running=self.never))
        self.assertEqual(saves.describe("", str(shutil_dest)).slots, 3)

    def test_two_imports_keep_both_backups(self):
        src = make_set(self.root / "src", "n")
        dest = saves.triforce_folder()
        make_set(dest, "old")
        b1 = saves.import_saves(src, dest, running=self.never)
        b2 = saves.import_saves(src, dest, running=self.never)
        self.assertNotEqual(b1, b2)

    def test_refuses_when_running_and_same_folder(self):
        src = make_set(self.root / "src", "n")
        dest = saves.triforce_folder()
        make_set(dest, "old")
        with self.assertRaises(RuntimeError):
            saves.import_saves(src, dest, running=lambda g=None: "eden")
        self.assertEqual((Path(dest) / "option.sav").read_text(), "option old")
        with self.assertRaises(RuntimeError):
            saves.import_saves(dest, dest, running=self.never)

    def test_running_game_by_folder(self):
        # (a copy of sleep started from inside the game folder counts as the game; the machine may run its own Triforce)
        import shutil
        import subprocess
        game = self.root / "game"
        game.mkdir()
        exe = game / "mygame"
        shutil.copy2(shutil.which("sleep"), exe)
        p = subprocess.Popen([str(exe), "30"])
        try:
            time.sleep(0.2)
            self.assertEqual(saves.running_game(str(game)), "mygame")
        finally:
            p.kill()
            p.wait()

    def test_legacy_folder_when_data_has_none(self):
        game = self.root / "game"
        legacy = make_set(game / "user/save" / saves.TRIFORCE_UID, "legacy")
        self.assertEqual(saves.triforce_folder(str(game)), legacy)
        make_set(saves.triforce_folder(), "xdg")
        self.assertNotEqual(saves.triforce_folder(str(game)), legacy)


if __name__ == "__main__":
    unittest.main()
