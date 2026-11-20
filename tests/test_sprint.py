"""The sprint button (host/oot_sprint.h, nx2/oot_patches.py patch_sprint).

Link sprints while his input flags hold the dash input, which the game sets only out of a roll. With a button chosen,
the host puts the bit in while the button's sprint is on. The rules checked here, without the game:

- Off: nothing is taken from the controller and no sprint starts.
- Toggle with the left stick's click: a press in gameplay starts the sprint, the next ends it; the click never reaches
  the game in gameplay; standing still ends a sprint that had moved after 4 frames (60 Hz frames: 8 updates at 120 fps do
  not end it, 9 do), one that has not moved yet after 90; a state the list does not know ends it at once; ZL ends it.
- A press with no update of Link for 8 polls (a menu, a cutscene) is the game's and starts nothing.
- B: a sprint only for a press that begins while Link moves, kept from the game until it is let go; a press while
  standing is the game's own B, whole.
- Hold: on while the button is held, off when it is let go.
- Changing the button ends a sprint.

The patch: the two calls go in once each, a second run changes nothing, other code is left as it is.

python -I -B tests/test_sprint.py
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_patches  # noqa: E402

FIXTURE = r'''
#include <stdio.h>
#include "oot_sprint.h"

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { fails++; printf("FAIL line %d: %s\n", __LINE__, what); } } while (0)

static OotSprint s;
static void fresh(void) { OotSprint z = OOT_SPRINT_INIT; s = z; }
/* one frame: Link's update, then the controller's poll; the buttons the game gets */
static int on;
static uint64_t frame(int kind, int button, int hold, double frames, uint64_t buttons) {
    on = oot_sprint_step(&s, kind, button, hold, frames);
    oot_sprint_pad(&s, button, &buttons);
    return buttons;
}

int main(void) {
    const uint64_t LS = OOT_SPRINT_BIT_LS, B = OOT_SPRINT_BIT_B, ZL = OOT_SPRINT_BIT_ZL, A = 1;
    uint64_t got;

    /* the states */
    CHECK(oot_sprint_kind(1) == OOT_SPRINT_MOVE && oot_sprint_kind(13) == OOT_SPRINT_MOVE && oot_sprint_kind(68) == OOT_SPRINT_MOVE, "Walk, Jump, Clamber move");
    CHECK(oot_sprint_kind(35) == OOT_SPRINT_KEEP && oot_sprint_kind(253) == OOT_SPRINT_KEEP, "Slide, StepOn keep");
    CHECK(oot_sprint_kind(40) == OOT_SPRINT_WAIT, "Wait waits");
    CHECK(oot_sprint_kind(0) == OOT_SPRINT_STOP && oot_sprint_kind(-1) == OOT_SPRINT_STOP && oot_sprint_kind(999) == OOT_SPRINT_STOP, "anything else stops");

    /* off */
    fresh();
    got = frame(OOT_SPRINT_MOVE, OOT_SPRINT_OFF, 0, 1, LS | A);
    got = frame(OOT_SPRINT_MOVE, OOT_SPRINT_OFF, 0, 1, LS | A);
    CHECK(got == (LS | A) && !on, "off: the click is the game's, no sprint");

    /* toggle, the stick's click */
    fresh();
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, 0);
    got = frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, LS | A);     /* the press: seen by the next update */
    CHECK(got == A, "the click is kept from the game in gameplay");
    got = frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, LS);
    CHECK(on && got == 0, "the press started the sprint");
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, 0);
    CHECK(on, "it stays on with the button let go");
    for (int i = 0; i < 4; i++) frame(OOT_SPRINT_WAIT, OOT_SPRINT_LS, 0, 1, 0);
    CHECK(on, "four frames of standing do not end a sprint that moved");
    frame(OOT_SPRINT_WAIT, OOT_SPRINT_LS, 0, 1, 0);
    CHECK(!on, "the fifth does");
    /* the next press starts again, the one after ends it */
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, LS);
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, 0);
    CHECK(on, "on again");
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, LS);
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, 0);
    CHECK(!on, "the next press ends it");

    /* at 120 fps the standing time is the same 4 frames of 60 Hz */
    fresh();
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 0.5, 0);
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 0.5, LS);
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 0.5, 0);
    CHECK(on, "on at 120 fps");
    for (int i = 0; i < 8; i++) frame(OOT_SPRINT_WAIT, OOT_SPRINT_LS, 0, 0.5, 0);
    CHECK(on, "eight updates of half a frame do not end it");
    frame(OOT_SPRINT_WAIT, OOT_SPRINT_LS, 0, 0.5, 0);
    CHECK(!on, "the ninth does");

    /* a press while standing: on, and 90 frames to start moving */
    fresh();
    frame(OOT_SPRINT_WAIT, OOT_SPRINT_LS, 0, 1, 0);
    frame(OOT_SPRINT_WAIT, OOT_SPRINT_LS, 0, 1, LS);
    frame(OOT_SPRINT_WAIT, OOT_SPRINT_LS, 0, 1, 0);
    CHECK(on, "a press while standing arms the sprint");
    for (int i = 0; i < 89; i++) frame(OOT_SPRINT_WAIT, OOT_SPRINT_LS, 0, 1, 0);
    CHECK(on, "still armed after 89 frames");
    frame(OOT_SPRINT_WAIT, OOT_SPRINT_LS, 0, 1, 0);
    frame(OOT_SPRINT_WAIT, OOT_SPRINT_LS, 0, 1, 0);
    CHECK(!on, "not after 91");

    /* an unknown state, and ZL */
    fresh();
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, 0);
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, LS);
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, 0);
    frame(OOT_SPRINT_KEEP, OOT_SPRINT_LS, 0, 1, 0);
    CHECK(on, "a slide keeps it");
    frame(OOT_SPRINT_STOP, OOT_SPRINT_LS, 0, 1, 0);
    CHECK(!on, "a state the list does not know ends it");
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, LS);
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, ZL);
    CHECK(on, "on (ZL is seen by the next update)");
    got = frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, ZL);
    CHECK(!on && got == ZL, "targeting ends it, and ZL is the game's");

    /* ((Triforce, 11-18) a conversation: Link's update goes on in a state no sprint is in, and the click hides the text box
       there: it is the game's, and starts nothing) */
    fresh();
    frame(OOT_SPRINT_STOP, OOT_SPRINT_LS, 0, 1, 0);
    got = frame(OOT_SPRINT_STOP, OOT_SPRINT_LS, 0, 1, LS);
    CHECK(got == LS, "in a conversation the click reaches the game");
    got = frame(OOT_SPRINT_STOP, OOT_SPRINT_LS, 0, 1, LS);
    CHECK(!on && got == LS, "the whole press stays the game's, no sprint");
    got = frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, 0);
    got = frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, LS);
    CHECK(got == 0, "back in play the click is the sprint's again");
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, 0);
    CHECK(on, "and starts it");

    /* no update of Link: a menu. The press is the game's and starts nothing */
    fresh();
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, 0);
    for (int i = 0; i < 9; i++) { uint64_t b = 0; oot_sprint_pad(&s, OOT_SPRINT_LS, &b); }
    got = LS;
    oot_sprint_pad(&s, OOT_SPRINT_LS, &got);
    CHECK(got == LS, "in a menu the click reaches the game");
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, LS);
    CHECK(!on, "and starts no sprint when play goes on with it still held");
    got = frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, LS);
    CHECK(!on && got == LS, "the whole press stays the game's");

    /* B */
    fresh();
    frame(OOT_SPRINT_WAIT, OOT_SPRINT_B, 0, 1, 0);
    got = frame(OOT_SPRINT_WAIT, OOT_SPRINT_B, 0, 1, B);
    CHECK(got == B, "B while standing is the game's B");
    got = frame(OOT_SPRINT_WAIT, OOT_SPRINT_B, 0, 1, B);
    CHECK(!on && got == B, "and no sprint");
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_B, 0, 1, 0);
    got = frame(OOT_SPRINT_MOVE, OOT_SPRINT_B, 0, 1, B);
    CHECK(got == 0, "B while moving is kept from the game");
    got = frame(OOT_SPRINT_WAIT, OOT_SPRINT_B, 0, 1, B);
    CHECK(got == 0, "until it is let go, whatever Link does meanwhile");
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_B, 0, 1, 0);
    got = frame(OOT_SPRINT_WAIT, OOT_SPRINT_B, 0, 1, B);
    CHECK(got == B, "the next press, standing, is the game's again");

    /* hold */
    fresh();
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 1, 1, 0);
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 1, 1, LS);
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 1, 1, LS);
    CHECK(on, "hold: on while held");
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 1, 1, 0);
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 1, 1, 0);
    CHECK(!on, "hold: off when let go");

    /* another button chosen */
    fresh();
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, 0);
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, LS);
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, 0);
    CHECK(on, "on with the click");
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_B, 0, 1, 0);
    CHECK(!on, "choosing B ends it");
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_LS, 0, 1, LS);
    frame(OOT_SPRINT_MOVE, OOT_SPRINT_OFF, 0, 1, 0);
    CHECK(!on && !s.active && !s.pressed, "off leaves nothing on");

    printf(fails ? "sprint: FAILED\n" : "sprint: all checks passed\n");
    return fails != 0;
}
'''

LIFTED = (
    '#include "a64.h"\n'
    "void f_804caa80(Ctx *__restrict A64_NOESC c) {\n"
    "    c->x[10]=recomp_load64(c,c->x[10] + 0);\n"            # (the same load elsewhere in the function: left)
    "L_804ce790:;\n"
    + oot_patches.SPRINT_LINK_OLD +
    "if (((c->x[9] >> 20) & 1)) { goto L_804cfa90; }\n"
    "if (((c->x[10] >> 4) & 1)) { goto L_804ced50; }\n"
    "}\n"
    "\nvoid f_80c225d0(Ctx *__restrict A64_NOESC c) {\n"
    + oot_patches.SPRINT_STANCE_OLD +
    "}\n"
    "\nvoid f_80000000(Ctx *__restrict A64_NOESC c) {\n"       # (another function with the same lines: left)
    + oot_patches.SPRINT_LINK_OLD + oot_patches.SPRINT_STANCE_OLD +
    "}\n"
)


class SprintTest(unittest.TestCase):
    def test_rules(self):
        compiler = shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            self.skipTest("no C compiler")
        with tempfile.TemporaryDirectory(prefix="sprint-") as d:
            tmp = Path(d)
            (tmp / "t.c").write_text(FIXTURE, encoding="utf-8")
            binary = tmp / ("t.exe" if sys.platform == "win32" else "t")
            build = subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-O1", "-I", str(ROOT / "host"),
                                    str(tmp / "t.c"), "-o", str(binary)], text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("all checks passed", run.stdout)

    def test_patch(self):
        out, n = oot_patches.patch_sprint(LIFTED)
        self.assertEqual(n, 2)
        self.assertEqual(out.count("hos_oot_sprint_link(c,c->x[19],c->x[10]);"), 1)
        self.assertEqual(out.count("hos_oot_sprint_stance(c,c->x[19],c->x[8]);"), 1)
        self.assertEqual(out.count(oot_patches.SPRINT_DECL), 1)
        link = out.index("hos_oot_sprint_link(c,")
        self.assertLess(out.index("c->x[10]=recomp_load64(c,c->x[19] + 80);"), link)
        self.assertLess(link, out.index("if (((c->x[9] >> 20) & 1))"))          # (before both tests of the flags)
        stance = out.index("hos_oot_sprint_stance(c,")
        self.assertLess(stance, out.index("goto L_80c27f54;"))
        self.assertIn(oot_patches.SPRINT_LINK_OLD + oot_patches.SPRINT_STANCE_OLD, out)   # (the other function: as it was)
        self.assertEqual(oot_patches.patch_sprint(out), (out, 2))                # (a second run)

    def test_other_code_left(self):
        other = LIFTED.replace("c->x[10]=recomp_load64(c,c->x[19] + 80);", "c->x[10]=recomp_load64(c,c->x[19] + 88);", 1)
        out, n = oot_patches.patch_sprint(other)
        self.assertEqual(n, 1)                                                   # (the stance's alone)
        self.assertNotIn("hos_oot_sprint_link(c,", out)

    def test_wired(self):
        hid = (ROOT / "host/nn_hid.c").read_text(encoding="utf-8")
        self.assertIn("hos_oot_sprint_pad(&buttons);", hid)
        api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
        self.assertIn("HOS_OPT_SPRINT, HOS_OPT_SPRINT_HOLD", api)
        replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
        self.assertIn("if (id == HOS_OPT_SPRINT) return opts.sprint_button;", replay)
        self.assertIn("if (id == HOS_OPT_SPRINT_HOLD) return opts.sprint_mode;", replay)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
