"""The damaged-texture check is done while the game files are unpacked, not by opening every file again afterwards.

An incomplete dump has .zs files that begin with zeros, and the game leaves out what uses them; the build replaces each by
a whole file of the same kind. To find them it opened all 102,185 .zs files after the unpack: 292 s of an 18 minute
build on a PC whose virus scanner looks at each file that is opened. The unpack has every file's first bytes in hand as it
writes them: nx2/nup.py extract now notes the damaged ones (ZS_CHECKED), and nx2/make_game.py takes them from that note.
A folder unpacked before, without a note, is still gone through file by file.

Game-free: a stand-in for the package reader, authored files.

    python -I -B tests/test_unpack_damage_check.py
"""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
sys.path.insert(0, str(ROOT / "third_party/a64-kit/tools"))
import nup  # noqa: E402

spec = importlib.util.spec_from_file_location("make_game_unpack_check", ROOT / "nx2/make_game.py")
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)

WHOLE = nup.ZSTD + b"a whole texture"
FILES = {"exefs": {"main": bytes(16)},
         "romfs": {"Model/a_Alb.bntx.zs": WHOLE, "Model/b_Alb.bntx.zs": bytes(32), "Model/c.txt": bytes(8),
                   "Model/d_Alb.bntx.zs": b"\x50\x2a\x4d\x18skippable", "Pack/e.zs": b"\0\0"}}


class Reader:
    """The package reader as extract uses it: the members of a part, and one member's bytes into a file (in two writes,
    as a large member arrives)"""
    def walk(self, part):
        return [(rel, (0, 0, len(data), 0, len(data), 0)) for rel, data in FILES[part].items()]

    def copy_to(self, r, g, name=""):
        data = next(d for files in FILES.values() for rel, d in files.items() if rel == name)
        g.write(data[:3])
        g.write(data[3:])


def unpack(folder, **more):
    zs = {}
    with contextlib.redirect_stdout(io.StringIO()):
        nup.extract(Reader(), {"exefs": "exefs", "romfs": "romfs"}, folder, zs=zs, **more)
    return zs


class UnpackNotesDamagedFiles(unittest.TestCase):
    WANT = {"files": 4, "damaged": ["Model/b_Alb.bntx.zs", "Pack/e.zs"]}

    def test_each_zs_file_is_looked_at_as_it_is_written(self):
        for quiet in (False, True):
            with tempfile.TemporaryDirectory(prefix="unpack-check-") as folder:
                self.assertEqual(unpack(folder, quiet=quiet), self.WANT)
                for part, files in FILES.items():
                    for rel, data in files.items():
                        self.assertEqual((Path(folder) / part / rel).read_bytes(), data)

    def test_files_that_were_there_already_are_read_back(self):
        with tempfile.TemporaryDirectory(prefix="unpack-check-") as folder:
            unpack(folder)
            with patch.object(Reader, "copy_to", side_effect=AssertionError("a member there already was written again")):
                self.assertEqual(unpack(folder), self.WANT)

    def test_without_the_dict_nothing_changes(self):
        with tempfile.TemporaryDirectory(prefix="unpack-check-") as folder:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(nup.extract(Reader(), {"exefs": "exefs", "romfs": "romfs"}, folder),
                                 (5 + 1, sum(len(d) for files in FILES.values() for d in files.values())))

    def test_the_exefs_alone_notes_nothing_of_the_romfs(self):
        with tempfile.TemporaryDirectory(prefix="unpack-check-") as folder:
            self.assertEqual(unpack(folder, only="exefs"), {"files": 0, "damaged": []})


class TheBuildTakesTheNote(unittest.TestCase):
    def folder(self, temp, note):
        out = Path(temp)
        unpack(str(out / ".game"))
        if note is not None:
            (out / ".game" / nup.ZS_CHECKED).write_text(note if isinstance(note, str) else json.dumps(note), encoding="utf-8")
        seen = make_game.unpack_seen(str(out / ".game"))
        os.replace(out / ".game" / "romfs", out / "romfs")
        (out / ".triforce_romfs").write_text("unpacked by this builder: game.nup 1\n")
        return out, seen

    def test_a_note_that_cannot_be_right_is_not_taken(self):
        with tempfile.TemporaryDirectory(prefix="unpack-check-") as temp:
            for note in (None, "not json", [1], {"files": 4}, {"files": "4", "damaged": []},
                         {"files": 4, "damaged": ["../outside.zs"]}, {"files": 4, "damaged": ["C:/x.zs"]},
                         {"files": 4, "damaged": ["Model\\b.zs"]}, {"files": 4, "damaged": ["Model/b.txt"]},
                         {"files": 4, "damaged": [7]}):
                folder = Path(temp) / "game"
                folder.mkdir(exist_ok=True)
                if note is None:
                    (folder / nup.ZS_CHECKED).unlink(missing_ok=True)
                else:
                    (folder / nup.ZS_CHECKED).write_text(note if isinstance(note, str) else json.dumps(note), encoding="utf-8")
                self.assertIsNone(make_game.unpack_seen(str(folder)), note)

    def test_the_damaged_files_get_their_stand_ins_and_no_file_is_opened_to_find_them(self):
        with tempfile.TemporaryDirectory(prefix="unpack-check-") as temp:
            out, seen = self.folder(temp, UnpackNotesDamagedFiles.WANT)
            self.assertEqual(seen, UnpackNotesDamagedFiles.WANT)
            said = io.StringIO()
            with patch.object(nup, "damaged_game_files", side_effect=AssertionError("every file was opened again")), \
                    contextlib.redirect_stdout(said):
                make_game.stand_in_damaged_files(str(out), str(out / "romfs"), seen)
            self.assertEqual((out / "romfs/Model/b_Alb.bntx.zs").read_bytes(), WHOLE)   # (a whole texture of its kind)
            self.assertEqual((out / "romfs/Pack/e.zs").read_bytes(), b"\0\0")            # (nothing similar beside it)
            self.assertEqual((out / ".triforce_romfs_checked").read_text(), (out / ".triforce_romfs").read_text())
            self.assertIn("as they were unpacked: 4 .zs files, 2 damaged", said.getvalue())
            self.assertIn("Your game dump is incomplete: 2 game files", said.getvalue())
            self.assertNotIn("@@progress", said.getvalue())
            # (checked under this unpack's stamp: not again)
            with patch.object(nup, "stand_in_damaged", side_effect=AssertionError("checked twice")):
                make_game.stand_in_damaged_files(str(out), str(out / "romfs"), seen)
                make_game.stand_in_damaged_files(str(out), str(out / "romfs"))

    def test_without_a_note_every_file_is_opened_as_before(self):
        with tempfile.TemporaryDirectory(prefix="unpack-check-") as temp:
            out, seen = self.folder(temp, None)
            self.assertIsNone(seen)
            with contextlib.redirect_stdout(io.StringIO()) as said:
                make_game.stand_in_damaged_files(str(out), str(out / "romfs"), seen)
            self.assertEqual((out / "romfs/Model/b_Alb.bntx.zs").read_bytes(), WHOLE)
            self.assertIn("every .zs file is opened", said.getvalue())
            self.assertIn("checked 4 of 4 files", said.getvalue())

    def test_the_two_ways_find_the_same_files(self):
        with tempfile.TemporaryDirectory(prefix="unpack-check-") as temp:
            zs = unpack(temp)
            romfs = os.path.join(temp, "romfs")
            self.assertEqual(sorted(os.path.relpath(p, romfs).replace(os.sep, "/") for p in nup.damaged_game_files(romfs)),
                             sorted(zs["damaged"]))


class TheNoteIsWrittenAndReadInTime(unittest.TestCase):
    def test_source(self):
        tool = (ROOT / "nx2/nup.py").read_text(encoding="utf-8")
        main = tool[tool.index("\ndef main():"):]
        self.assertIn("extract(nup, t, a.extract, a.only, zs=zs)", main)
        self.assertIn("ZS_CHECKED", main)
        build = (ROOT / "nx2/make_game.py").read_text(encoding="utf-8")
        body = build[build.index("\ndef main("):]
        read, moved, gone = (body.index(text) for text in ("unpacked_seen = unpack_seen(game)",
                                                           'shutil.move(os.path.join(game, "romfs"), romfs)',
                                                           "shutil.rmtree(game, ignore_errors=True)\n            shutil.rmtree(kept"))
        self.assertTrue(read < moved < gone, "the note is read after the folder it lies in is moved or deleted")
        self.assertIn("stand_in_damaged_files(out, romfs, unpacked_seen)", body)


if __name__ == "__main__":
    unittest.main()
