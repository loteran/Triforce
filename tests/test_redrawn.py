"""(Triforce, 11-18) A draw may leave a texture it samples for the next frame (the new-texture budget) only when every target it
writes was written in the frame before too and its program writes no memory (gpu/vk/redrawn.h; vk_draw.cpp draw_may_defer).

The game composes each text box once into a target of its own and keeps it: a compose in a frame over the budget kept the dummy
for the whole session (empty title buttons, option values, dialogue boxes). The rule's cases: a scene target drawn every frame is
redrawn before its pass's mark and after it (prev_written); a target composed for the first time is not, for its first draw nor
its second in that frame (begin_pass marks it written before the second draw builds its descriptors); one composed again 300
frames later is not; a two-frame gap is not; contents begun anew (released, or never the GPU's) have no frame before. And the
source keeps every gate: kept_texture's look and its kept dummy, texture_view_of's handle cache, the whole-draw reuse and the
signature hit.

python -I -B tests/test_redrawn.py
"""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"

FIXTURE = r"""
#include "redrawn.h"
#include <cstdio>
static int fails;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
struct T { uint64_t prev = 0, last = 0; bool gpu = false, released = false; };
static void write(T& t, uint64_t f) {   // (as texture_written: prev before last, read before gpu/released are set)
    t.prev = redrawn::prev_on_write(t.prev, t.last, t.gpu && !t.released, f);
    t.last = f, t.gpu = true, t.released = false;
}
static bool redrawn_now(const T& t, uint64_t f) { return redrawn::drawn_before(t.prev, t.last, t.gpu && !t.released, f, 1); }
static bool redrawn_in(const T& t, uint64_t f, uint64_t w) { return redrawn::drawn_before(t.prev, t.last, t.gpu && !t.released, f, w); }
int main() {
    {   // (a scene target, drawn every frame from frame 10 on)
        T s;
        for (uint64_t f = 10; f < 20; f++) write(s, f);
        CHECK(redrawn_now(s, 20));    // (frame 20, its first draw: before begin_pass marks it)
        write(s, 20);
        CHECK(redrawn_now(s, 20));    // (its second draw in frame 20: after the mark)
        write(s, 20);
        CHECK(redrawn_now(s, 20));
        CHECK(redrawn_now(s, 21));
    }
    {   // (a text box composed for the first time at frame 50)
        T b;
        CHECK(!redrawn_now(b, 50));   // (its first draw)
        write(b, 50);
        CHECK(!redrawn_now(b, 50));   // (its second draw: marked written already, but not in the frame before)
        write(b, 50);
        CHECK(!redrawn_now(b, 50));
        CHECK(redrawn_now(b, 51));    // (were it drawn again in the next frame, that frame's draws could leave textures)
        // (composed again 300 frames later)
        CHECK(!redrawn_now(b, 350));
        write(b, 350);
        CHECK(!redrawn_now(b, 350));
    }
    {   // (a two-frame gap)
        T g;
        write(g, 100);
        CHECK(!redrawn_now(g, 102));
        write(g, 102);
        CHECK(!redrawn_now(g, 102));
    }
    {   // (released after its last write: its contents begin anew)
        T r;
        write(r, 200);
        r.released = true;
        CHECK(!redrawn_now(r, 201));
        write(r, 201);
        CHECK(!redrawn_now(r, 201));
        write(r, 202);
        CHECK(redrawn_now(r, 202) && redrawn_now(r, 203));
    }
    {   // ((Triforce, 11-19) the window: a target drawn every second or third frame counts as drawn again; a first compose never)
        T a;
        write(a, 300);
        CHECK(redrawn_in(a, 302, 4) && redrawn_in(a, 304, 4) && !redrawn_in(a, 305, 4));
        CHECK(!redrawn_in(a, 302, 1));   // (v0.5.2's rule)
        write(a, 302);
        CHECK(redrawn_in(a, 302, 4));    // (its second draw in frame 302: the write in 300 is within the window)
        T fresh;
        CHECK(!redrawn_in(fresh, 400, 4));
        write(fresh, 400);
        CHECK(!redrawn_in(fresh, 400, 4));   // (a text box's first compose, its later draws in the same frame too)
        CHECK(redrawn::kWindow == 4);
        T z;   // (a window of 0 taken as 1)
        write(z, 10);
        CHECK(redrawn_in(z, 11, 0) && !redrawn_in(z, 12, 0));
        T future;   // (a last write marked after this frame: not before it)
        write(future, 900);
        CHECK(!redrawn_in(future, 899, 4));
    }
    if (!fails) std::printf("PASS\n");
    return fails != 0;
}
"""


class RedrawnTests(unittest.TestCase):
    def test_rule(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("no C++ compiler")
        with tempfile.TemporaryDirectory(prefix="triforce-redrawn-") as tmp:
            tmp = Path(tmp)
            (tmp / "t.cpp").write_text(FIXTURE, encoding="utf-8")
            exe = tmp / "t.exe"
            build = subprocess.run([compiler, "-std=c++17", "-Wall", "-O1", "-I", str(VK), str(tmp / "t.cpp"), "-o", str(exe)],
                                   text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], text=True, capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_gates_in_the_source(self):
        draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
        tex = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
        # (prev_written kept at every write, before last_written moves)
        self.assertRegex(tex, r"tex->prev_written = redrawn::prev_on_write\(tex->prev_written, tex->last_written, "
                              r"tex->gpu_written && !tex->released, frame_no\);\n    tex->last_written = frame_no;")
        # (the draw's decision: every colour target and the depth target redrawn, no memory written)
        self.assertIn("bool may = redrawn_off || big || !program_writes_memory(p);", draw)
        self.assertIn("for (int i = 0; i < at.n && may && !redrawn_off && !big; i++) may = texture_redrawn(at.color[i]);", draw)
        self.assertIn("if (may && !redrawn_off && !big) may = texture_redrawn(at.depth);", draw)
        # ((Triforce, 11-19) only small targets under the rule: the text boxes and labels are 262x104 .. 656x113 and 90x34 .. 631x94
        self.assertIn("return v >= 0 ? (uint64_t)v : 100000ull;", draw)
        self.assertGreater(100000, 656 * 113)
        self.assertGreater(100000, 631 * 94)
        self.assertLessEqual(100000, 480 * 270)
        # (the one gate every deferral path checks, set from it)
        self.assertIn("upload_defer_ok = !compute && draw_may_defer;", draw)
        self.assertEqual(len(re.findall(r"upload_defer_ok = !compute", draw)), 1)
        # (no kept dummy, nor a kept texture owed its bytes, for a draw that may not defer)
        self.assertIn("if (took && !compute && !draw_may_defer && (!e->out[1] || texture_owes(", draw)
        # (the handle cache's dummy only for a look that may defer; an owed texture looked at again by one that may not)
        self.assertIn("(it->second.tex_epoch == texture_dummy_generation() && upload_defer_ok)", draw)
        self.assertIn("if (it->second.tex && !upload_defer_ok && texture_owes(it->second.tex)) {", draw)
        # (no whole-draw reuse nor signature hit for such a draw in a frame that left textures)
        self.assertIn("const bool strict = !draw_may_defer && texture_frame_left_any();", draw)
        self.assertIn("const bool same_gens = !no_reuse && !strict &&", draw)
        self.assertIn("const int sig_mode = !same_gens && !no_reuse && !strict ? desc_sig_mode() : 0;", draw)
        # (the decision comes before the reuse checks read it)
        self.assertLess(draw.index("draw_may_defer = may;"), draw.index("const bool same_gens = !no_reuse"))
        # (every deferral path still checks the gate)
        for fn in ("bool new_texture_later(const Texture&) {", "bool new_texture_waits() {", "bool create_later(bool for_sampling) {",
                   "bool upload_again_later(const Texture& tex) {", "bool upload_defer(const Texture& tex) {"):
            at = tex.index(fn)
            self.assertIn("upload_defer_ok", tex[at:at + 600], fn)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
