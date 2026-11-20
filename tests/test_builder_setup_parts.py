"""The Python builder's install of a release (builder/py/hookshot_builder/setup_parts.py; no network, no real release).

Checked here, in a temporary HOOKSHOT_SETUP_BASE with authored zips:
- a zip member's name: a device's name (with or without an extension, COM1 to COM9, LPT1 to LPT9), a name that ends in a dot
  or a space, a colon or a drive's letter, a path from the root or through "..", a character Windows refuses: such a zip is
  refused whole and nothing of it is written (zipfile's extractall dropped some quietly and wrote the file under another name);
- the room on the disk asked before a download's first byte and before an unpacking's first file;
- the installed version is the rollback point: the folders an update moves over from it (reuse_part) are written down first,
  and a cancel, a failure, or an install cut off and run again moves them back, so the version still installed is whole and
  the unfinished one is gone; (Triforce) and so is what a cut-off install of another release moved (every versions\\*.part
  with a reused.txt is undone at the start of an install, not only the release's own).

    python -I -B tests/test_builder_setup_parts.py
"""
import io
import os
import shutil
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder" / "py"))
from hookshot_builder import paths, setup_parts as sp  # noqa: E402

SHA_A, SHA_B = "a" * 64, "b" * 64


def zip_bytes(names):
    data = io.BytesIO()
    with zipfile.ZipFile(data, "w") as z:
        for n in names:
            z.writestr(n, b"x")
    return data.getvalue()


class Names(unittest.TestCase):
    def test_refused(self):
        for name in ("NUL", "nul.txt", "tools/CON", "a/Aux.dll", "COM1", "lpt9.log", "com\u00b9.txt", "a/CONIN$", "prn .txt",
                     "a/b.", "a/b ", "a./b", "c:/x", "C:x", "a/b:stream", "/etc/x", "\\x", "../x", "a/../../x", "a\\..\\x",
                     "a/b<c", "a/b?", "a/\x01", "", "  "):
            with self.subTest(name=name):
                self.assertIsNotNone(sp.unsafe_member(name), name)

    def test_taken(self):
        for name in ("tools/python/python.exe", "a/b/", "./a", "COM10", "LPT0", "console.txt", "nulx", "a.b.c", "CONFIG",
                     "HookshotBuilder/tools/nx2/make_game.py"):
            with self.subTest(name=name):
                self.assertIsNone(sp.unsafe_member(name), name)

    def test_zip_refused_whole(self):
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "out"
            dest.mkdir()
            with zipfile.ZipFile(io.BytesIO(zip_bytes(["good.txt", "sub/NUL.txt", "late.txt"]))) as z:
                with self.assertRaises(sp.SetupError) as e:
                    sp.check_zip(z, str(dest), "Part 1 of 1")
            self.assertIn("device", str(e.exception))
            self.assertEqual(list(dest.iterdir()), [])
            with zipfile.ZipFile(io.BytesIO(zip_bytes(["good.txt", "sub/fine.txt"]))) as z:
                sp.check_zip(z, str(dest), "Part 1 of 1")

    def test_room(self):
        with tempfile.TemporaryDirectory() as tmp:
            with mock.patch.object(sp.shutil, "disk_usage", lambda d: mock.Mock(free=100 << 20)):
                sp.room(os.path.join(tmp, "not", "made", "yet"), 60 << 20, "x")   # (60 MB and the 32 MB margin fit in 100)
                with self.assertRaises(sp.SetupError) as e:
                    sp.room(tmp, 80 << 20, "part 1 of 2")
            self.assertIn("no room for part 1 of 2", str(e.exception))

    def test_download_asks_room_first(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = sp.Part(SHA_A, str(5 << 30), ".", "-", "-", "https://github.com/x/y/releases/download/t/HookshotBuilder-python-a.zip")
            got = []
            with mock.patch.object(sp.shutil, "disk_usage", lambda d: mock.Mock(free=1 << 30)), \
                    mock.patch.object(sp.Http, "get", lambda *a, **k: got.append(a)):
                with self.assertRaises(sp.SetupError):
                    sp.download_part(lambda s: None, lambda f: None, p, 1, 1, os.path.join(tmp, SHA_A + ".zip"), None)
            self.assertEqual(got, [], "a download began without room for it")
            self.assertFalse(os.path.exists(os.path.join(tmp, SHA_A + ".zip.part")))


class Rollback(unittest.TestCase):
    """v1 installed, with the python part in a folder of its own; v2 lists the same python part (moved over) and a new one"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name) / "base"
        env = mock.patch.dict(os.environ, {"HOOKSHOT_SETUP_BASE": str(self.base)})
        env.start()
        self.addCleanup(env.stop)
        self.addCleanup(self.tmp.cleanup)
        v1 = self.base / "versions" / "v1"
        self.python = v1 / "HookshotBuilder" / "tools" / "python"
        self.python.mkdir(parents=True)
        (self.python / "python.exe").write_bytes(b"py")
        (v1 / "HookshotBuilder" / "tools" / "nx2").mkdir(parents=True)
        (v1 / "HookshotBuilder" / "tools" / "nx2" / "make_game.py").write_text("#", encoding="utf-8")
        (v1 / "installed.txt").write_text("v1", encoding="utf-8")
        (v1 / "parts.txt").write_text(SHA_A + "\n", encoding="utf-8")
        (self.base / "versions" / "current.txt").write_text("v1", encoding="utf-8")
        self.parts = [sp.Part(SHA_A, "10", ".", "-", "-", "https://github.com/x/y/releases/download/v2/HookshotBuilder-python-a.zip"),
                      sp.Part(SHA_B, "10", ".", "-", "-", "https://github.com/x/y/releases/download/v2/HookshotBuilder-b.zip")]
        self.work = self.base / "versions" / "v2.part"

    def whole(self):
        self.assertTrue((self.python / "python.exe").is_file(), "the installed version lost the folder it lent")
        self.assertFalse(self.work.exists(), "the unfinished version was left")

    def run_install(self, download):
        with mock.patch.object(sp, "read_manifest", lambda status, tag=None, cancel=None: ("v2", self.parts)), \
                mock.patch.object(sp, "download_part", download):
            return sp.install(lambda s: None)

    def test_cancel_moves_back(self):
        def cancelled(*a):
            self.assertTrue((self.work / "HookshotBuilder" / "tools" / "python" / "python.exe").is_file(), "the part was not moved over")
            self.assertTrue((self.work / sp.REUSED).is_file(), "the move was not written down first")
            raise sp.SetupError("cancelled")
        with self.assertRaises(sp.SetupError):
            self.run_install(cancelled)
        self.whole()

    def test_failure_moves_back(self):
        def broken(status, progress, p, i, n, zpath, cancel):
            Path(zpath).write_bytes(zip_bytes(["HookshotBuilder/AUX.txt"]))   # (a zip check_zip refuses)
        with mock.patch.object(sp, "sha256_file", lambda path: SHA_B):
            with self.assertRaises(sp.SetupError):
                self.run_install(broken)
        self.whole()

    def test_cut_off_then_run_again(self):
        # (an install that died after moving the part over: the next one moves it back before anything else)
        self.work.mkdir(parents=True)
        self.assertTrue(sp.reuse_part("v2", self.parts[0], str(self.work)))
        self.assertFalse(self.python.exists())
        seen = []

        def first(*a):
            seen.append((self.python / "python.exe").is_file())
            raise sp.SetupError("cancelled")
        with mock.patch.object(sp, "unpack_parts", first):
            with self.assertRaises(sp.SetupError):
                self.run_install(None)
        self.assertEqual(seen, [True], "the folder was not moved back before the new install began")
        self.whole()

    def cut_off_v2(self):
        """(Triforce) an install of v2 that died after moving the part over: v2.part holds v1's python and the note"""
        self.work.mkdir(parents=True)
        self.assertTrue(sp.reuse_part("v2", self.parts[0], str(self.work)))
        self.assertFalse(self.python.exists())

    def test_other_release_cut_off_then_install(self):
        # (Triforce) v2's install was cut off, and v3 is installed now: v2.part's folders go back to v1 too, before v3 begins
        self.cut_off_v2()
        seen = []

        def first(status, progress, tag, parts, work, downloads, cancel):
            seen.append((tag, (self.python / "python.exe").is_file()))
            raise sp.SetupError("cancelled")
        with mock.patch.object(sp, "read_manifest", lambda status, tag=None, cancel=None: ("v3", self.parts)), \
                mock.patch.object(sp, "unpack_parts", first):
            with self.assertRaises(sp.SetupError):
                sp.install(lambda s: None)
        self.assertEqual(seen, [("v3", True)], "v2.part kept v1's folder while v3 was installed")
        self.assertFalse((self.work / sp.REUSED).exists())
        self.assertTrue((self.python / "python.exe").is_file())

    def test_other_release_cut_off_then_installed_one(self):
        # (Triforce) v2's install was cut off and the release asked for is v1, installed: it is not installed again, and it
        # gets its folder back
        self.cut_off_v2()
        with mock.patch.object(sp, "read_manifest", lambda status, tag=None, cancel=None: ("v1", self.parts)), \
                mock.patch.object(sp, "unpack_parts", lambda *a: self.fail("v1 was installed again")), \
                mock.patch.object(sp, "prune", lambda tag, parts: None):
            tag, final = sp.install(lambda s: None)
        self.assertEqual(tag, "v1")
        self.assertTrue((self.python / "python.exe").is_file(), "v1 runs without the folder v2.part held")
        self.assertFalse((self.work / sp.REUSED).exists())

    def test_not_undone_is_kept(self):
        # (Triforce) a .part whose folders could not all go back stops the install, named, and is not deleted with them
        self.cut_off_v2()
        with mock.patch.object(sp, "undo_reuse", lambda work: False), \
                mock.patch.object(sp, "read_manifest", lambda status, tag=None, cancel=None: ("v3", self.parts)), \
                mock.patch.object(sp, "unpack_parts", lambda *a: self.fail("v3 began with v1's folder still lent")):
            with self.assertRaises(sp.SetupError) as e:
                sp.install(lambda s: None)
        self.assertIn(str(self.work), str(e.exception))
        self.assertTrue((self.work / "HookshotBuilder" / "tools" / "python" / "python.exe").is_file())
        self.assertTrue((self.work / sp.REUSED).is_file())

    def test_undo_every_reuse_leaves_others(self):
        # (Triforce) a .part without a note, and a file named .part, are not touched
        plain = self.base / "versions" / "v4.part"
        plain.mkdir(parents=True)
        (plain / "kept.txt").write_text("x", encoding="utf-8")
        (self.base / "versions" / "v5.part").write_text("a file", encoding="utf-8")
        self.cut_off_v2()
        self.assertIsNone(sp.undo_every_reuse())
        self.assertTrue((self.python / "python.exe").is_file())
        self.assertTrue((plain / "kept.txt").is_file())
        self.assertTrue((self.base / "versions" / "v5.part").is_file())

    def test_success_keeps_no_note(self):
        def fine(status, progress, p, i, n, zpath, cancel):
            Path(zpath).write_bytes(zip_bytes(["HookshotBuilder/tools/nx2/make_game.py", "HookshotBuilder/b.txt"]))
        with mock.patch.object(sp, "sha256_file", lambda path: SHA_B), mock.patch.object(sp, "prune", lambda tag, parts: None):
            tag, final = self.run_install(fine)
        self.assertEqual(tag, "v2")
        self.assertTrue(Path(final, "HookshotBuilder", "tools", "python", "python.exe").is_file())
        self.assertFalse(Path(final, sp.REUSED).exists())
        self.assertFalse(self.work.exists())


if __name__ == "__main__":
    unittest.main()
