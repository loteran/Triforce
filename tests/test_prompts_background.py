"""The button prompts are drawn beside the lift and the compile, and kept when nothing they are made from changed.

nx2/prompt_assets.py took 87 s at the end of every build and every update with nothing else running: it now starts once
the game files are read (nx2/make_game.py class Prompts), writes into <out>/prompts.new, and the finish puts that folder
in place. A folder made from the same tools and game files (prompts_key) is kept and nothing is run.

Game-free: a stand-in for prompt_assets.py that writes one file, in a folder of its own.

    python -I -B tests/test_prompts_background.py
"""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
sys.path.insert(0, str(ROOT / "third_party/a64-kit/tools"))
spec = importlib.util.spec_from_file_location("make_game_prompts_background", ROOT / "nx2/make_game.py")
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)

# (what stands for the drawing: one file per run into --out, and a count of the runs beside this script; with a file
# named "fail" beside it, it writes half of its output and fails)
DRAW = """import os, sys
out = sys.argv[sys.argv.index("--out") + 1]
here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(here, "runs"), "a") as f:
    f.write("x")
os.makedirs(os.path.join(out, "switch"))
with open(os.path.join(out, "switch", "External.bfotf"), "w") as f:
    f.write("drawn in run %d" % os.path.getsize(os.path.join(here, "runs")))
print("prompts: switch")
sys.exit(1 if os.path.exists(os.path.join(here, "fail")) else 0)
"""


class PromptsInTheBackground(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="prompts-background-")
        root = Path(self.temp.name)
        self.tools, self.out, self.work = root / "tools", root / "game", root / "work"
        for folder in (self.tools, self.out / "romfs", self.work):
            folder.mkdir(parents=True)
        (self.tools / "prompt_assets.py").write_text(DRAW, encoding="utf-8")
        (self.out / ".triforce_romfs").write_text("unpacked by this builder: game.nup 1\n")
        self.patches = [patch.object(make_game, "HERE", str(self.tools)),
                        patch.object(make_game, "PY", [sys.executable, "-I", "-B"]),
                        patch.object(make_game.importlib.util, "find_spec", return_value=object())]
        for p in self.patches:
            p.start()

    def tearDown(self):
        for p in self.patches:
            p.stop()
        self.temp.cleanup()

    def build(self):
        """One build's part: started after the read, finished at the end. What it said, and what the prompts hold"""
        said, drawn = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(said), contextlib.redirect_stderr(drawn):
            prompts = make_game.Prompts(str(self.out / "romfs"), str(self.out), str(self.work))
            started = prompts.proc is not None
            prompts.finish()
            prompts.stop()   # (what the build's end does in any case: nothing left to stop or take away)
        made = self.out / "prompts" / "switch" / "External.bfotf"
        return said.getvalue() + drawn.getvalue(), started, made.read_text() if made.exists() else None

    def runs(self):
        counted = self.tools / "runs"
        return counted.stat().st_size if counted.exists() else 0

    def test_made_once_then_kept_until_what_they_are_made_from_changes(self):
        said, started, made = self.build()
        self.assertTrue(started)
        self.assertEqual(made, "drawn in run 1")
        self.assertIn("prompts: switch", said)                      # (the drawing's own lines are still said)
        self.assertFalse((self.out / "prompts.new").exists())
        said, started, made = self.build()                          # (an update with the same tools and game files)
        self.assertFalse(started)
        self.assertEqual((made, self.runs()), ("drawn in run 1", 1))
        self.assertIn("as the last build made them", said)
        (self.out / ".triforce_romfs").write_text("unpacked by this builder: other.nup 2\n")   # (another dump)
        self.assertEqual(self.build()[1:], (True, "drawn in run 2"))
        (self.tools / "prompt_draw.py").write_text("# a change to the drawing\n")              # (newer tools)
        self.assertEqual(self.build()[1:], (True, "drawn in run 3"))
        self.assertEqual(self.build()[1:], (False, "drawn in run 3"))

    def test_the_keyboard_and_mouse_faces_tools_are_part_of_what_they_are_made_from(self):
        # ((Triforce) nx2/prompt_keys.py draws that face, its labels in a font of third_party/fonts: a change to either
        # draws the prompts again; the menus' other fonts in that folder are none of the prompts')
        fonts = Path(self.temp.name) / "third_party" / "fonts" / "libertinus"
        fonts.mkdir(parents=True)
        (fonts / "LibertinusSans-Bold.ttf").write_bytes(b"the labels' font")
        (fonts / "LibertinusSerif-Semibold.ttf").write_bytes(b"a font of the menus'")
        (self.tools / "prompt_keys.py").write_text("# the keys' icons\n")
        with patch.object(make_game, "ROOT", self.temp.name):
            self.assertEqual(self.build()[1:], (True, "drawn in run 1"))
            self.assertEqual(self.build()[1:], (False, "drawn in run 1"))
            (self.tools / "prompt_keys.py").write_text("# the keys' icons, changed\n")
            self.assertEqual(self.build()[1:], (True, "drawn in run 2"))
            (fonts / "LibertinusSans-Bold.ttf").write_bytes(b"the labels' font, another release")
            self.assertEqual(self.build()[1:], (True, "drawn in run 3"))
            (fonts / "LibertinusSerif-Semibold.ttf").write_bytes(b"a font of the menus', changed")
            self.assertEqual(self.build()[1:], (False, "drawn in run 3"))
        body = (ROOT / "nx2/make_game.py").read_text(encoding="utf-8")
        self.assertIn('"prompt_keys.py"', body[body.index("def prompts_key("):body.index("class EarlyCompile")])
        self.assertTrue((ROOT / "third_party/fonts/libertinus/LibertinusSans-Bold.ttf").is_file())
        keys = (ROOT / "nx2/prompt_keys.py").read_text(encoding="utf-8")
        self.assertIn('"third_party", "fonts", "libertinus", "LibertinusSans-Bold.ttf"', keys)

    def test_the_players_own_folders_are_drawn_at_every_build(self):
        (self.out / ".triforce_romfs").unlink()   # (romfs and exefs folders linked in: no stamp to tell them by)
        self.assertIsNone(make_game.prompts_key(str(self.out)))
        self.assertEqual(self.build()[1:], (True, "drawn in run 1"))
        self.assertEqual(self.build()[1:], (True, "drawn in run 2"))
        self.assertFalse((self.out / "prompts" / make_game.PROMPT_KEY).exists())

    def test_a_drawing_that_fails_leaves_no_prompts(self):
        self.build()
        (self.out / ".triforce_romfs").write_text("unpacked by this builder: other.nup 2\n")
        (self.tools / "fail").write_text("")
        said, started, made = self.build()
        self.assertTrue(started)
        self.assertIsNone(made)                                      # (the game shows its own icons, as before)
        self.assertIn("were not made: the game shows its own", said)
        self.assertFalse((self.out / "prompts.new").exists())
        (self.tools / "fail").unlink()
        self.assertEqual(self.build()[1:], (True, "drawn in run 3"))   # (and the next build draws them again)

    def test_a_build_that_stops_takes_its_drawing_with_it(self):
        (self.tools / "prompt_assets.py").write_text("import time\ntime.sleep(60)\n", encoding="utf-8")
        with contextlib.redirect_stdout(io.StringIO()):
            prompts = make_game.Prompts(str(self.out / "romfs"), str(self.out), str(self.work))
        (self.out / "prompts.new").mkdir(exist_ok=True)
        prompts.stop()
        self.assertIsNotNone(prompts.proc.poll())
        self.assertFalse((self.out / "prompts.new").exists())

    def test_without_fonttools_nothing_is_started(self):
        with patch.object(make_game.importlib.util, "find_spec", return_value=None):
            said, started, made = self.build()
        self.assertEqual((started, made, self.runs()), (False, None, 0))
        self.assertIn("no fontTools", said)

    def test_the_build_starts_them_after_the_read_and_ends_them_in_the_finish(self):
        body = (ROOT / "nx2/make_game.py").read_text(encoding="utf-8")
        body = body[body.index("\ndef main("):]
        read, started, lift, compile_, finish = (body.index(text) for text in (
            'say("stage", "read"', "prompts = Prompts(romfs, out, work)", 'say("stage", "lift"', 'say("stage", "compile"',
            "prompts.finish()"))
        self.assertTrue(read < started < lift < compile_ < finish)
        self.assertIn("atexit.register(prompts.stop)", body)


if __name__ == "__main__":
    unittest.main()
