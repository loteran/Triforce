"""A build that fails in its compile keeps the files that did compile (nx2/make_game.py compile_alone_failed).

make_game deleted every compiled object after any failed build, so that a stale object could not fail the link at every
try (issue 1197). A compiler that runs out of memory, or a full disk, is the common failure, and it then cost the next
build all ~600 files again. Each object is stamped and cached under the hash of its source and flags only once its
compiler ended well, so after a compile that failed before any link the ones there are whole: they stay, and only a
build that failed at a link, or ended without saying how its compile went, loses them.

Game-free: build.py's own lines, authored.

    python -I -B tests/test_failed_compile_keeps_objects.py
"""
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
sys.path.insert(0, str(ROOT / "third_party/a64-kit/tools"))
spec = importlib.util.spec_from_file_location("make_game_failed_compile", ROOT / "nx2/make_game.py")
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)


class WhichFailureKeepsTheObjects(unittest.TestCase):
    def test_a_failed_compile_keeps_them(self):
        self.assertTrue(make_game.compile_alone_failed(["compiled in 412s, 2 failed"]))
        self.assertTrue(make_game.compile_alone_failed(["compiled in 9s, 1 failed"]))

    def test_a_failed_link_does_not(self):
        self.assertFalse(make_game.compile_alone_failed(["compiled in 412s, 0 failed", "link FAILED (3s), 20 undefined symbols"]))
        self.assertFalse(make_game.compile_alone_failed(["compiled in 412s, 0 failed", "triforce_vk.dll link FAILED, 4 undefined symbols"]))
        # (never both in one run, but if a link is ever tried after failed files it still decides)
        self.assertFalse(make_game.compile_alone_failed(["compiled in 412s, 1 failed", "link FAILED (3s), 1 undefined symbols"]))

    def test_a_build_that_did_not_say_how_its_compile_went_does_not(self):
        self.assertFalse(make_game.compile_alone_failed([]))                               # (stopped, or failed before the compile)
        self.assertFalse(make_game.compile_alone_failed(["compiled in 412s, 0 failed"]))   # (failed after a clean compile, not at a link)

    def test_the_lines_are_build_pys_own(self):
        build = (ROOT / "nx2/build.py").read_text(encoding="utf-8")
        self.assertIn('print("compiled in %.0fs, %d failed" % (time.time() - t0, still), flush=True)', build)
        self.assertGreaterEqual(build.count("link FAILED"), 3)
        # (an object is stamped and cached only after its compiler returned 0)
        one = build[build.index("def compile_one(job):"):build.index("\ndef main():")]
        self.assertLess(one.index("if r.returncode:"), one.index('open(stamp, "w").write(key)\n    cache_put(obj, key)'))

    def test_the_failed_build_asks_before_it_deletes(self):
        body = (ROOT / "nx2/make_game.py").read_text(encoding="utf-8")
        body = body[body.index('run(PY + ["nx2/build.py", "--standalone"'):body.index("compile_seconds = time.time() - compile_started")]
        asked, deleted = body.index("if compile_alone_failed(said):"), body.index('os.path.join(work, "objcache")')
        self.assertLess(asked, deleted)
        # (the lift is forgotten either way: a lifted file cut short fails the same way at every try otherwise)
        self.assertLess(body.index("os.remove(key_file)"), asked)


if __name__ == "__main__":
    unittest.main()
