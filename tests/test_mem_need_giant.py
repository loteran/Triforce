"""The memory gate counts a file with giant functions for what its compiler really takes (nx2/build.py mem_need).

Measured with HOOKSHOT_MEM_LOG on a build of v0.4.0 (clang 23.1.5): a chunk that is one 5 MB function took 6.2 GB and was
counted at 1.9; chunks holding functions of 2 to 3.6 MB took 3.1 and 3.7 GB, counted at 1.4 and 1.5; the hot pieces that
hold functions near 2 MB took 2.5 to 2.7 GB, counted at 1.5. Started beside the others on a PC without that much memory,
Windows pages and Linux stops the compiler. A function of GIANT_BYTES or more is now counted by the square of its size.

Authored files of the measured shapes (no game code): the estimate must cover what was measured, and must not grow for
the ordinary chunks, where it was right.

    python -I -B tests/test_mem_need_giant.py
"""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
sys.path.insert(0, str(ROOT / "third_party/a64-kit/tools"))
os.environ.setdefault("OOT_WORK", tempfile.gettempdir())
spec = importlib.util.spec_from_file_location("build_mem_need_giant", ROOT / "nx2/build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)

MB = 1 << 20


def lifted(folder, name, functions):
    """A file of lifted functions of these sizes (MB each), as the lift writes their heads"""
    path = os.path.join(folder, name)
    with open(path, "wb") as f:
        for i, size in enumerate(functions):
            head = b"\nvoid f_%08x(Ctx *__restrict A64_NOESC c) {\n" % (0x80000000 + i)
            f.write(head + b" " * (int(size * MB) - len(head) - 2) + b"}\n")
    return path


class GiantFunctions(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mem-need-")
        self.folder = self.temp.name
        build._giants = True   # (a PC short of memory: giants_counted, tested below)

    def tearDown(self):
        self.temp.cleanup()

    def need(self, name, functions):
        return build.mem_need(lifted(self.folder, name, functions))

    def test_the_measured_giants_are_covered(self):
        # (name, the functions' sizes as measured, what its compiler took, what it was counted at before)
        for name, functions, took, before in (
                ("code_023.c", [5.05] + [0.00005] * 1000, 6209, 1918),            # one function is the whole chunk
                ("code_023_1.c", [3.58] + [0.00002] * 1000, 3088, 1475),
                ("code_022_1.c", [2.91, 1.5, 0.9] + [0.004] * 240, 3707, 1443),
                ("code_021_1.c", [1.83, 1.6, 1.3] + [0.014] * 12, 2465, 1101)):
            got = self.need(name, functions)
            self.assertGreaterEqual(got, took - 250, "%s: counted %d MB, took %d" % (name, got, took))
            self.assertGreater(got, before, name)

    def test_a_hot_piece_with_functions_near_2_mb(self):
        self.assertGreaterEqual(self.need("code_hot_4.c", [1.96, 1.2, 0.6] + [0.0006] * 4200), 2730 - 250)
        # (and one without any: the flat count, as before)
        self.assertEqual(self.need("code_hot_2.c", [0.65, 0.5] + [0.003] * 3900), build.COMPILER_MB)

    def test_ordinary_chunks_are_counted_as_before(self):
        # (24 MB of functions under the giant size: 1.46 to 1.5 GB measured, counted 1.6 to 1.8 before and now)
        got = self.need("code_028.c", [0.31] + [0.0026] * 9300)
        self.assertTrue(1500 <= got <= 1800, got)
        got = self.need("code_015.c", [1.1, 0.9] + [0.0034] * 6350)   # (large functions, none a giant)
        self.assertTrue(1500 <= got <= 2100, got)

    def test_only_where_memory_is_short(self):
        # (with memory to spare the giants are counted as before: counting them in full there halved the compilers at once)
        said = __import__("io").StringIO()
        with __import__("contextlib").redirect_stdout(said):
            for free, env, want in ((21000, None, False), (9000, None, True), (None, None, False), (9000, "0", False), (64000, "1", True)):
                build._giants = None
                build.free_mb = lambda free=free: free
                if env is None:
                    os.environ.pop("HOOKSHOT_MEM_GIANT", None)
                else:
                    os.environ["HOOKSHOT_MEM_GIANT"] = env
                self.assertEqual(build.giants_counted(), want, (free, env))
                self.assertEqual(build.giants_counted(), want)   # (decided once)
        os.environ.pop("HOOKSHOT_MEM_GIANT", None)
        self.assertEqual(said.getvalue().count("the memory gate: giant functions"), 5)
        build._giants = False
        self.assertEqual(self.need("code_023.c", [5.05] + [0.00005] * 1000), int(400 + 50 * 5.1 + 250 * 5.05))
        self.assertEqual(self.need("code_hot_4.c", [1.96, 1.2, 0.6] + [0.0006] * 4200), build.COMPILER_MB)

    def test_the_other_files(self):
        self.assertEqual(build.mem_need(os.path.join(self.folder, "vk_draw.cpp")), 1000)
        self.assertEqual(build.mem_need(os.path.join(self.folder, "hos_main.c")), 400)


if __name__ == "__main__":
    unittest.main()
