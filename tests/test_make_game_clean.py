"""Game-free checks of make_game's failed-build wipe (which failures keep the lift and the object caches) and of --clean.

Run with: python -I tests/test_make_game_clean.py
"""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

SOURCE = Path(__file__).resolve().parents[1] / "nx2" / "make_game.py"
spec = importlib.util.spec_from_file_location("make_game_clean_under_test", SOURCE)
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)


class FailureTests(unittest.TestCase):
    def test_vulkan_files_only_keep_everything(self):
        lines = ["compiled in 12s, 2 failed",
                 "FAILED vk_ffx3.cpp (0 MB, 3s, alone too):", "fatal error: x.h: No such file",
                 "FAILED vk_ffx_fg.cpp (0 MB, 2s, alone too):", "fatal error: x.h: No such file"]
        self.assertFalse(make_game.failure_needs_wipe(lines)[0])

    def test_game_file_wipes(self):
        self.assertTrue(make_game.failure_needs_wipe(["FAILED vk_a.cpp (0 MB, 1s, alone too):", "FAILED code_021.c (24 MB, 9s, alone too):"])[0])

    def test_link_wipes(self):
        self.assertTrue(make_game.failure_needs_wipe(["link FAILED (3s), 20 undefined symbols", "FAILED vk_a.cpp (0 MB, 1s, alone too):"])[0])
        self.assertTrue(make_game.failure_needs_wipe(["libvk.so link FAILED, 2 undefined symbols"])[0])

    def test_unclassified_wipes(self):
        self.assertTrue(make_game.failure_needs_wipe([])[0])
        self.assertTrue(make_game.failure_needs_wipe(["Traceback (most recent call last):", "MemoryError"])[0])


class CleanTests(unittest.TestCase):
    def test_clean_removes_exactly_the_key_and_both_caches(self):
        with tempfile.TemporaryDirectory() as d:
            gen = os.path.join(d, "sa", "gen")
            os.makedirs(gen)
            os.makedirs(os.path.join(d, "objcache"))
            os.makedirs(os.path.join(d, "sa", "build", "obj"))
            for f in (os.path.join(gen, make_game.LIFT_KEY), os.path.join(gen, "code_001.c"), os.path.join(d, "objcache", "a.o"),
                      os.path.join(d, "sa", "build", "obj", "b.o"), os.path.join(d, "sa", "build", "game.so"), os.path.join(d, "times.json")):
                Path(f).write_text("x")
            make_game.clean_work(d)
            left = sorted(os.path.relpath(os.path.join(r, f), d) for r, _, fs in os.walk(d) for f in fs)
            self.assertEqual(left, sorted(["sa/gen/code_001.c", "sa/build/game.so", "times.json"]))
            self.assertFalse(os.path.exists(os.path.join(d, "objcache")))

    def test_flag_parses(self):
        with mock.patch.object(sys, "argv", ["make_game.py", "--clean"]):
            self.assertTrue(make_game.parse_args().clean)
        with mock.patch.object(sys, "argv", ["make_game.py"]):
            self.assertFalse(make_game.parse_args().clean)


if __name__ == "__main__":
    unittest.main()
