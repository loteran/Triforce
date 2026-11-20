"""The files that need no translated code are compiled while the game is being translated (Windows).

The lift was one process on Windows (its emit has workers there now, nx2/emit_pool.py, on half the processors at
most): for its two minutes every other processor was idle, and the compile that followed
had libopus and both renderers to do on top of the game's own files (~520 of ~630 files, a ninth of the compile's time).
nx2/make_game.py now starts nx2/build.py with RX_EARLY=1 beside the lift: only those files, nothing linked, and the
build proper then finds their objects made.

Read from the source (a compile needs the compiler and minutes): which files the early run takes and that it never
reads what the lift is writing, that it stops before any link, and that make_game starts it before the lift and waits
for it before the build.

    python -I -B tests/test_early_compile.py
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = (ROOT / "nx2/build.py").read_text(encoding="utf-8")
MAKE = (ROOT / "nx2/make_game.py").read_text(encoding="utf-8")
MAIN = BUILD[BUILD.index("\ndef main():"):]


class TheEarlyRun(unittest.TestCase):
    def test_it_takes_only_the_files_that_need_no_lifted_code(self):
        self.assertIn('EARLY = os.environ.get("RX_EARLY") == "1"', BUILD)
        keep = re.search(r'if EARLY:[^\n]*\n\s+jobs = \[j for j in jobs if os\.path\.basename\(j\[2\]\)\.startswith\(\((.*?)\)\)\]', MAIN)
        self.assertIsNotNone(keep, "the early run no longer picks its files by their objects' names")
        self.assertEqual(sorted(re.findall(r'"(\w+)"', keep.group(1))), ["gpu_", "opus_", "vk_"])
        # (those names are the three groups' own, and the runtime's and the game's objects have others)
        for made in ('"opus_" + rel.replace', '"gpu_" + name + ".o"', '"vk_" + name + ".o"', '"host_" + os.path.basename(s)'):
            self.assertIn(made, MAIN)
        # (and none of the three groups' keys holds the lifted code's headers, which the lift is writing then)
        for group in ("oflags, b\"opus-1.5.2\"", "gflags, gdeps"):
            self.assertIn(group, MAIN)
        gdeps = MAIN[MAIN.index("gdeps = "):MAIN.index("\n", MAIN.index("gdeps = "))]
        self.assertNotIn(" deps", gdeps.replace("gdeps", ""))

    def test_it_reads_nothing_the_lift_is_writing(self):
        self.assertIn('deps = b"" if EARLY else', MAIN)
        self.assertIn("chunks = [] if EARLY else sorted(", MAIN)
        early_srcs = MAIN[MAIN.index("    if EARLY:   # (the lift's files are on their way"):]
        self.assertTrue(early_srcs.split("\n")[1].strip() == "srcs = []")
        self.assertLess(MAIN.index("srcs = []"), MAIN.index("costs = chunk_costs(gen)"))
        guarded = MAIN[MAIN.index("    if not EARLY:\n        migrate_stamps("):MAIN.index("chunks = [] if EARLY")]
        self.assertEqual(guarded.count("migrate_stamps("), 2)

    def test_it_links_nothing(self):
        compiled = MAIN.index('print("compiled in %.0fs, %d failed"')
        stop = MAIN.index("    if EARLY:   # (nothing to link yet", compiled)
        self.assertIn("return 0", MAIN[stop:stop + 200])
        for link in ("objs.rsp", "lld_link()", '"-fuse-ld=lld"'):
            self.assertGreater(MAIN.index(link, compiled), stop, link)


class TheBuildStartsItBesideTheLift(unittest.TestCase):
    def test_before_the_lift_and_done_before_the_build(self):
        body = MAKE[MAKE.index("\ndef main("):]
        early, lift, done, key, build = (body.index(text) for text in (
            "early = EarlyCompile(work, env, a.jobs)", 'run(PY + ["nx2/lift.py", "--standalone"', "early.finish()",
            "write_lift_key(work, key)", 'run(PY + ["nx2/build.py", "--standalone", "--jobs", str(a.jobs)], env, compiled)'))
        self.assertTrue(early < lift < done < key < build)
        self.assertIn("atexit.register(early.stop)", body)

    def test_the_same_settings_as_the_build_and_windows_only(self):
        cls = MAKE[MAKE.index("class EarlyCompile:"):MAKE.index("class Prompts:")]
        self.assertIn('if os.name != "nt" or os.environ.get("HOOKSHOT_EARLY_COMPILE") == "0":', cls)
        self.assertIn("e.update(env)", cls)             # (RX_RELEASE and the rest: the same flags, so the same object keys)
        self.assertIn('e["RX_EARLY"] = "1"', cls)
        self.assertIn('PY + ["nx2/build.py", "--standalone", "--jobs", str(max(1, jobs - 1))]', cls)   # (one processor left to the lift)
        self.assertIn("the build compiles what is missing", cls)


if __name__ == "__main__":
    unittest.main()
