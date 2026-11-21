"""The game's texture data compaction goes through the host, which counts its cycles and may hold them to a pace
(host/oot_tex_compact.c, nx2/oot_patches.py patch_tex_compact, HOS_OOT_TEX_COMPACT_MS; no game).

1. The rule of when an ask with the compaction idle runs the game's step (hos_oot_tex_compact_runs, cut out of the host's file
   and compiled alone): always with the switch unset or 0, never with -1, and with a pace only once the time of the next
   cycle has come.
2. The host's call (source): the state is read before the game's step; only an idle compaction is ever held, and held as the
   planner's "nothing to move" (the result 0); otherwise the game's own step runs; a cycle started sets the next one's time.
3. The lift's patch: the one call of the step in the compaction thread's handler is made through the host, once, with its
   declaration; a text patched before is left as it is; a text with the call twice, or without it, is not touched; and it is
   among the supported build's patches, with a warning when its place is not found.

    python -I -B tests/test_oot_tex_compact.py [--cc gcc]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def in_order(text, parts, where):
    at = 0
    for part in parts:
        found = text.find(part, at)
        if found < 0:
            raise SystemExit("%s: %r is missing, or not after the part before it (update the test with the code)" % (where, part))
        at = found + len(part)


RULE_TEST = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
RULE
int main(void) {
    /* (unset or 0: as shipped, whatever the clock) */
    CHECK(hos_oot_tex_compact_runs(0, 0, 0) && hos_oot_tex_compact_runs(0, 5, 1000));
    /* (-1: no cycle is started) */
    CHECK(!hos_oot_tex_compact_runs(-1, 0, 0) && !hos_oot_tex_compact_runs(-1, 1000, 5));
    /* (a pace: only once the next cycle's time has come) */
    CHECK(!hos_oot_tex_compact_runs(1000, 999, 1000) && hos_oot_tex_compact_runs(1000, 1000, 1000));
    CHECK(hos_oot_tex_compact_runs(1000, 1001, 1000) && hos_oot_tex_compact_runs(250, 1, 0));
    puts("PASS rule: as shipped unless set, never with -1, a pace by the next cycle's time");
    return 0;
}
'''

# (what, the rule's text, its replacement): each must make the rule's test fail
MUTATIONS = [
    ("as shipped unless set", "    if (!ms) return 1;\n", ""),
    ("-1 starts none", "    if (ms < 0) return 0;\n", ""),
    ("held until the next cycle's time", "    return now >= next;", "    return 1;"),
    ("run once the time has come", "    return now >= next;", "    return now > next;"),
]


def check_source(host):
    call = block(host, "void hos_oot_tex_compact(Ctx *c, A64Fn game) {")
    in_order(call, ["const uint64_t mgr = c->x[0];", "const uint64_t rec = recomp_load64(c, mgr + COMPACT);",
                    "idle = readable(rec) && (uint8_t)recomp_load8(c, rec + STATE) == IDLE;",
                    "if (idle && !hos_oot_tex_compact_runs(ms, now, next)) {", "c->x[0] = 0;", "} else {", "game(c);",
                    "if (idle && (c->x[0] & 1)) {", "if (ms > 0) next = now + (int64_t)ms * 1000000;"], "oot_tex_compact.c hos_oot_tex_compact")
    if call.count("game(c);") != 1 or call.count("c->x[0] = 0;") != 1:
        raise SystemExit("oot_tex_compact.c: the game's step is called, or its result set, in a place this test does not know")
    if 'getenv("HOS_OOT_TEX_COMPACT_MS")' not in host:
        raise SystemExit("oot_tex_compact.c: the switch is not read")


def check_patch():
    import oot_patches
    old, new, decl = oot_patches.TEX_COMPACT_OLD, oot_patches.TEX_COMPACT_NEW, oot_patches.TEX_COMPACT_DECL
    if "0x807da430ULL; f_807da480(c);" not in old or "hos_oot_tex_compact(c, f_807da480);" not in new or "0x807da430ULL" not in new:
        raise SystemExit("oot_patches.py: the compaction's call is not the handler's one call of the step")
    head = '#include "a64.h"\nvoid f_807da3f0(Ctx *__restrict A64_NOESC c) {\n'
    text = head + old + "\n}\nvoid other(Ctx *c) {\nc->x[30] = 0x807d88a4ULL; f_807da480(c);\n}\n"
    out, found = oot_patches.patch_tex_compact(text)
    if found != 1 or new not in out or old in out or out.count(decl) != 1 or "0x807d88a4ULL; f_807da480(c);" not in out:
        raise SystemExit("patch_tex_compact: the handler's call is not made through the host, or another caller's is")
    again, found = oot_patches.patch_tex_compact(out)
    if found != 1 or again != out:
        raise SystemExit("patch_tex_compact: a text patched before is patched again")
    for bad in (head + "}\n", head + old + "\n" + old + "\n}\n", old + "\n"):   # (no call, the call twice, no include to declare at)
        out, found = oot_patches.patch_tex_compact(bad)
        if found or out != bad:
            raise SystemExit("patch_tex_compact: a text without the one call and the one include is touched")
    src = (ROOT / "nx2" / "oot_patches.py").read_text(encoding="utf-8")
    apply = src[src.index("def apply(gen, *, protect_hearts=False, required=False):"):]
    in_order(apply, ["if protect_hearts:   # (found by address: the supported build's functions)",
                     "compacted, found = patch_tex_compact(out)", "tex_compact_sites += found", "out = compacted",
                     "if protect_hearts and tex_compact_sites != 1:"], "oot_patches.py apply")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    host = (ROOT / "host" / "oot_tex_compact.c").read_text(encoding="utf-8")
    check_source(host)
    check_patch()
    print("PASS sources: only an idle compaction is held, as the planner's nothing to move; the lift's one call through the host")
    rule = block(host, "int hos_oot_tex_compact_runs(int ms, int64_t now, int64_t next) {")
    with tempfile.TemporaryDirectory(prefix="oot-tex-compact-") as tmp:
        d = Path(tmp)

        def run(name, text):
            c, exe = d / (name + ".c"), d / (name + ".exe")
            c.write_text(RULE_TEST.replace("RULE", text), encoding="utf-8")
            r = subprocess.run([a.cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if r.returncode:
                return None, r.stdout
            r = subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
            return r.returncode == 0, r.stdout
        ok, out = run("rule", rule)
        if not ok:
            raise SystemExit("the rule's test failed:\n" + out)
        print(out.strip())
        for i, (what, old, new) in enumerate(MUTATIONS):
            if rule.count(old) != 1:
                raise SystemExit("mutation %r: its text is not once in the rule (update the test with the host's file)" % what)
            ok, out = run("mutant%d" % i, rule.replace(old, new))
            if ok:   # (a mutant that does not build has failed too)
                raise SystemExit("mutation %r: the rule's test still passed without it" % what)
        print("PASS mutations: %d parts of the rule, each taken out, fail its test" % len(MUTATIONS))


if __name__ == "__main__":
    main()
