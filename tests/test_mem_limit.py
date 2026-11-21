"""The game's memory held to what it is told it has (host/hos_mem.c, host/hos_svc.c, HOS_MEM_LIMIT; no game).

1. The rule (hos_mem_fits, cut out of the host's file and compiled alone): with no limit everything fits; with one, a range
   fits while the count with its new pages is within the limit, and not past it.
2. The sources: the count is kept where the game's pages and physical maps are noted and where they go; only the pages of a
   range the game does not hold yet count; AllocateMemoryPages and MapPhysicalMemory ask before they commit, and answer a
   refusal with the results the console gives; UsedMemorySize is the count only with the limit on, and never more than the
   total; unset, the limit is none.

    python -I -B tests/test_mem_limit.py [--cc gcc]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "host"


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
    const uint64_t MB = 1ull << 20;
    /* (no limit: as before, whatever is held) */
    CHECK(hos_mem_fits(0, 0, 0) && hos_mem_fits(20000 * MB, 4096, 0));
    /* (a limit: within it, up to it, not past it) */
    CHECK(hos_mem_fits(9000 * MB, 100 * MB, 9809 * MB) && hos_mem_fits(9709 * MB, 100 * MB, 9809 * MB));
    CHECK(!hos_mem_fits(9709 * MB, 100 * MB + 4096, 9809 * MB) && !hos_mem_fits(9809 * MB, 4096, 9809 * MB));
    /* (pages the game holds already cost nothing: a range of them fits at the limit, and past it) */
    CHECK(hos_mem_fits(9809 * MB, 0, 9809 * MB) && hos_mem_fits(11000 * MB, 0, 9809 * MB));
    puts("PASS rule: no limit unless set; within the limit and up to it, not past it; held pages cost nothing");
    return 0;
}
'''

MUTATIONS = [
    ("no limit unless set", "return !limit || !fresh || used + fresh <= limit;", "return !fresh || used + fresh <= limit;"),
    ("held pages cost nothing", "return !limit || !fresh || used + fresh <= limit;", "return !limit || used + fresh <= limit;"),
    ("not past the limit", "return !limit || !fresh || used + fresh <= limit;", "return !limit || !fresh || used <= limit;"),
    ("up to the limit", "return !limit || !fresh || used + fresh <= limit;", "return !limit || !fresh || used + fresh < limit;"),
]


def check_source(mem, svc):
    note = block(mem, "void hos_mem_note(int what, uint64_t addr, uint64_t size) {")
    if note.count("__atomic_add_fetch(&census_game_bytes, (int64_t)gc_change(&census, GC_GAME, lo, hi, 1)") != 1:
        raise SystemExit("hos_mem.c hos_mem_note: the game's pages and maps are not counted where they are noted")
    if note.count("__atomic_sub_fetch(&census_game_bytes, (int64_t)gc_change(&census, GC_GAME, lo, hi, 0)") != 3:
        raise SystemExit("hos_mem.c hos_mem_note: the count does not fall at each of the three places the game's pages go")
    if "gc_change(&census, GC_GAME" in note.replace("(int64_t)gc_change(&census, GC_GAME", ""):
        raise SystemExit("hos_mem.c hos_mem_note: the game's pages change somewhere without the count")
    limit = block(mem, "int64_t hos_mem_limit(void) {")
    in_order(limit, ['getenv("HOS_MEM_LIMIT")', "n <= 0 || !hos_mem_census_on() ? 0 : n == 1 ? (int64_t)hos_total_memory : (int64_t)n << 20;"],
             "hos_mem.c hos_mem_limit")
    room = block(mem, "int hos_mem_room(uint64_t addr, uint64_t size) {")
    in_order(room, ["if (!limit || !size || addr + size < addr || addr + size > GC_SPACE) return 1;",
                    "const uint64_t held = gc_count(&census, GC_GAME, addr, addr + size);",
                    "const uint64_t fresh = size > held ? size - held : 0, used = hos_mem_used();",
                    "if (hos_mem_fits(used, fresh, (uint64_t)limit)) return 1;", "return 0;"], "hos_mem.c hos_mem_room")
    pages = block(mem, "HLE(_ZN2nn2os19AllocateMemoryPagesEmm) {")
    in_order(pages, ["if (!hos_guest_range(a, n)) {", "if (!hos_mem_room(a, n)) {", "RET(0x1A01);", "hos_commit(a, n);",
                     "hos_mem_note(HOS_MEM_PAGES, a, n);"], "hos_mem.c AllocateMemoryPages")
    in_order(svc, ["static uint64_t hos_mem_used_told(void) {", "if (!hos_mem_limit()) return hos_program_size + hos_heap_size;",
                   "return used < hos_total_memory ? used : hos_total_memory;", "case 7: return hos_mem_used_told();",
                   "case 22: return hos_mem_used_told();", "case 0x2C: { /* MapPhysicalMemory(address, size) */",
                   "if (!hos_mem_room(c->x[0], c->x[1])) {", "c->x[0] = 0x10801;", "const int committed = hos_commit(c->x[0], c->x[1]);"],
             "hos_svc.c")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    mem = (HOST / "hos_mem.c").read_text(encoding="utf-8")
    check_source(mem, (HOST / "hos_svc.c").read_text(encoding="utf-8"))
    print("PASS sources: the count kept with the census's notes, a range asked before its commit, the console's refusals, "
          "UsedMemorySize the count only with the limit on")
    rule = block(mem, "int hos_mem_fits(uint64_t used, uint64_t fresh, uint64_t limit) {")
    with tempfile.TemporaryDirectory(prefix="mem-limit-") as tmp:
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
            if ok:
                raise SystemExit("mutation %r: the rule's test still passed without it" % what)
        print("PASS mutations: %d parts of the rule, each taken out, fail its test" % len(MUTATIONS))


if __name__ == "__main__":
    main()
