"""The Debug tab's Replay helper threads row: Off, or the parallel replay tried on 2 to 8 threads, which is what
RX_VK_PARALLEL_AUTO=<n> does (gpu/vk/vk_replay.cpp), or Automatic, the count gpu/vk/replay_threads.h gives the PC.
Automatic by default (that header says what was fixed for it), read at the start, and both switches still go over it. An
options file's own value means what it meant: 0 is Off, 1 to 7 are 2 to 8 threads; only a file without the key, or no
file, takes the new default. One line of the log says at the start what was chosen.

gpu/vk/vk_replay.cpp's own par_threads_max, par_auto and par_threads are cut out and run against the row's start value and
the two switches; the row's number for a thread count is gpu/vk/vk_menu.h's replay_threads_of, and what the row says is the
menu's own functions. RX_VK_EVENT_EARLY, the lab's other switch there, has no row.

    python -I -B tests/test_replay_threads_row.py [--cxx g++]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def function(source, start):
    at = source.find(start)
    check(at >= 0, "%s is missing" % start.rstrip(" {("))
    return source[at:source.index("\n}\n", at) + 3]


FIXTURE = r'''#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
#include "@ROOT@/gpu/vk/replay_threads.h"
int replay_threads_at_start = replay_threads::kDefaultRow;
int stand_in_automatic = 0;   /* (vk_replay.cpp's asks the device and the processors: replay_threads::automatic, checked below) */
int replay_threads_automatic() { return stand_in_automatic; }
int told = 0;   /* (the log's line at the start: one for each way the count is chosen) */
static void logf(const char* f, ...) { told += std::strstr(f, "vk: replay helper threads: ") == f; }
@OF@
@TOLD@
@MAX@
@AUTO@
bool par_auto_on = false;
@THREADS@
@ENV@
@TEXT@
@HINT@
// <row> <threads at most> <auto: the tuner decides> <threads before the tuner's first change> <which switch the row's line names, or ->
int main(int argc, char** argv) {
    if (argc < 7) return 2;
    /* (no row given: the options file does not name it, and the row is what it is by default) */
    if (std::strcmp(argv[1], "none")) replay_threads_at_start = atoi(argv[1]);
    stand_in_automatic = atoi(argv[6]);
    CHECK(par_threads_max() == atoi(argv[2]));
    CHECK(par_auto() == (atoi(argv[3]) != 0));
    CHECK(par_threads() == atoi(argv[4]));   /* (Auto starts on the replay thread alone: the trials turn the others on) */
    if (par_auto()) {
        par_auto_on = true;
        CHECK(par_threads() == par_threads_max());
    }
    /* one line of the log says what was chosen, unless Automatic chose (vk_replay.cpp's own line, the stand-in's here) */
    CHECK(told == (replay_threads_at_start == 8 && !getenv("RX_VK_PARALLEL") && !getenv("RX_VK_PARALLEL_AUTO") ? 0 : 1));
    const char* line = hn_replay_threads(replay_threads_at_start, -1);
    if (!std::strcmp(argv[5], "-")) CHECK(replay_threads_at_start == 8 ? line && std::strstr(line, "The default") : !line);
    else CHECK(line && std::strstr(line, argv[5]) && (std::strstr(argv[5], "AUTO") || !std::strstr(line, "AUTO")));
    /* the row's numbers and names: no "1 thread" (that is the replay thread alone), 2 to 8 for 1 to 7 */
    CHECK(replay_threads_of(0) == 0 && replay_threads_of(-1) == 0 && replay_threads_of(8) == 0 && replay_threads_of(99) == 0);
    for (int v = 1; v <= 7; v++) {
        char want[32];
        std::snprintf(want, sizeof want, "Auto, %d threads", v + 1);
        CHECK(replay_threads_of(v) == v + 1 && !std::strcmp(tx_replay_threads(v), want));
    }
    CHECK(!std::strcmp(tx_replay_threads(0), "Off") && !std::strcmp(tx_replay_threads(9), "Off") && !std::strcmp(tx_replay_threads(-1), "Off"));
    CHECK(!std::strcmp(tx_replay_threads(8), "Automatic") && replay_threads::kRowAutomatic == 8);
    /* an options file from before: its values 0 to 7 stand for the threads they stood for, whatever Automatic would give */
    for (int v = -1; v <= 7; v++)
        for (int a = 0; a <= 3; a++) CHECK(replay_threads::of_row(v, a) == replay_threads_of(v));
    CHECK(replay_threads::of_row(8, 0) == 0 && replay_threads::of_row(8, 2) == 2 && replay_threads::of_row(8, 3) == 3 && replay_threads::of_row(9, 3) == 0);
    /* and with no key the row is Automatic (replay_threads.h: the audit's holes closed, the owner's choice) */
    CHECK(replay_threads::kDefaultRow == replay_threads::kRowAutomatic);
    /* why, for the log's line: one reason for each outcome */
    using replay_threads::why;
    CHECK(std::strstr(why(8, false), "four cores or fewer") && std::strstr(why(12, false), "2 with") && std::strstr(why(16, false), "3 with"));
    CHECK(std::strstr(why(32, true), "AMD") && std::strstr(why(4, true), "AMD"));
    /* Automatic's rule: by the cores (half the logical processors), none on AMD's driver for RDNA 1 and 2 */
    using replay_threads::automatic;
    CHECK(automatic(0, false) == 0 && automatic(1, false) == 0 && automatic(4, false) == 0 && automatic(8, false) == 0 && automatic(9, false) == 0);
    CHECK(automatic(10, false) == 2 && automatic(12, false) == 2 && automatic(15, false) == 2);
    CHECK(automatic(16, false) == 3 && automatic(24, false) == 3 && automatic(32, false) == 3 && automatic(128, false) == 3);
    for (unsigned n = 0; n <= 256; n++) {
        CHECK(automatic(n, true) == 0);
        CHECK(automatic(n, false) == 0 || automatic(n, false) == 2 || automatic(n, false) == 3);   /* (never a count nobody measured) */
        if (n) CHECK(automatic(n, false) >= automatic(n - 1, false));
    }
    CHECK(hn_replay_threads(3, 0) && hn_replay_threads(3, 1) && !hn_replay_threads(3, 2) && !hn_replay_threads(0, 2));
    CHECK(hn_replay_threads(8, 0) && hn_replay_threads(8, 1) && hn_replay_threads(8, 2) && !hn_replay_threads(8, 3));
    std::puts("ok");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    vk = ROOT / "gpu" / "vk"
    replay = (vk / "vk_replay.cpp").read_text(encoding="utf-8")
    menu = (vk / "vk_menu.cpp").read_text(encoding="utf-8")
    header = (vk / "vk_menu.h").read_text(encoding="utf-8")
    top = function(replay, "int par_threads_max() {")
    order = [top.index('getenv("RX_VK_PARALLEL")'), top.index('getenv("RX_VK_PARALLEL_AUTO")'), top.index("replay_threads_of(replay_threads_at_start)")]
    check(order == sorted(order), "par_threads_max: the row is asked before a switch")
    of = re.search(r"constexpr int replay_threads_of\(int row\) \{[^\n]*\}\n", header)
    check(of, "vk_menu.h: replay_threads_of is missing")
    rule = (vk / "replay_threads.h").read_text(encoding="utf-8")
    check(re.search(r"\bint replay_threads = 8;", header) and "inline constexpr int kDefaultRow = kRowAutomatic;" in rule
          and "inline constexpr int kRowAutomatic = 8;" in rule
          and "int replay_threads_at_start = replay_threads::kDefaultRow;" in menu
          and "static_assert(Options().replay_threads == replay_threads::kDefaultRow" in menu,
          "the Replay helper threads row is not Automatic by default, in the options and for a start without an options file")
    # an options file's own value is taken as it is: no version of the file turns a 0 into the default
    init = function(menu, "void menu_init() {")
    check("opts.replay_threads =" not in init, "menu_init changes the Replay helper threads an options file names")
    # the log's line at the start, for each way the count is chosen, and how to turn the threads off
    check(top.count("par_threads_told(") == 3 and 'logf("vk: replay helper threads: ' in function(replay, "int par_threads_told(int threads, const char* by) {"),
          "par_threads_max no longer says at the start what was chosen by a switch or the row")
    check("replay_threads_at_start = opts.replay_threads;" in function(menu, "void menu_init() {"),
          "menu_init does not keep the Replay helper threads the game started with")
    # ...and it has read the file before the first replay asks (par_threads_max keeps what it found then): a file's Off is
    # Off from the start, not the default's Automatic
    check(function(replay, "bool replay_start() {").startswith("bool replay_start() {\n    menu_init();\n"),
          "replay_start no longer reads the options file first: the first replay could take the row's default over the file's Off")
    # Automatic: the rule's count for this PC's processors and its driver, asked after both switches
    automatic = top.index("if (replay_threads_at_start == replay_threads::kRowAutomatic) return replay_threads_automatic();")
    check(order[1] < automatic < order[2], "par_threads_max: Automatic is not asked after the switches, before the fixed counts")
    auto = function(replay, "int replay_threads_automatic() {")
    check("replay_threads::automatic(logical, amd_older)" in auto and "std::thread::hardware_concurrency()" in auto and
          "amd_older_compiler(facts)" in auto and "facts.driver_info = drv.driverInfo;" in auto,
          "replay_threads_automatic no longer asks the rule with the processors and the driver")
    check('logf("vk: replay helper threads: Automatic gives this PC %d (%u logical processors: %s). %s"' in auto and
          "replay_threads::why(logical, amd_older)" in auto and "RX_VK_PARALLEL=0" in auto and "Off on the Debug tab" in auto,
          "replay_threads_automatic no longer says in the log how many threads this PC gets, why, and how to turn them off")
    # the row: on the Debug tab among the Developer rows, a restart, and not the Quality preset's to turn Custom
    rows = menu[menu.index("Row rows[] = {"):]
    rows = rows[:rows.index("\n};")]
    row = re.search(r'\{TAB_DEBUG, "Replay helper threads", &opts\.replay_threads, 0, 8, 1, tx_replay_threads,.*?hn_replay_threads, true, true\},', rows, re.S)
    check(row and "Applies after restart" in row.group(0) and "Off: the replay thread alone, always" in row.group(0) and
          "Used with DLSS or FSR on too" in row.group(0),
          "the Replay helper threads row is not a Developer row of the Debug tab that takes a restart and says what Off is")
    check(rows.index('"Developer", nullptr') < rows.index('"Descriptor reuse", &opts.descriptor_reuse') < rows.index('"Sampler cache", &opts.sampler_cache')
          < rows.index('"Replay helper threads", &opts.replay_threads'),   # ((Triforce, v0.5.2) no Restart row after it: the pause menu's Quit
          "the Replay helper threads row is not beside Descriptor reuse and Sampler cache, under the Developer heading")
    for tab in ("TAB_DISPLAY", "TAB_GRAPHICS", "TAB_AUDIO", "TAB_CONTROLS", "TAB_SYSTEM"):
        check('{%s, "Replay helper threads"' % tab not in rows, "the Replay helper threads row is on a tab players see")
    # the lab's other switch stays a switch
    check("RX_VK_EVENT_EARLY" not in menu and "event_early" not in menu.lower() and "RX_VK_EVENT_EARLY" in replay,
          "RX_VK_EVENT_EARLY has a row, or is gone from vk_replay.cpp")
    # the Debug tab draws a row of more than two values as a choice (its Developer rows were Off and On alone). ((Triforce,
    # 11-17) The tab is drawn by draw_options' own loop now: only a row of On and Off is halves there)
    draw = function(menu, "void draw_options(")
    check("} else if (r.text == tx_onoff && !fixed) {" in draw and "choice_cell(c, va, vb, r.text(*r.value), selected, left, right, fixed);" in draw
          and "if (cur_tab == TAB_DEBUG) listed = debug_list(list);" in draw,
          "the Debug tab draws the Replay helper threads row as Off and On halves")
    guide = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check("| `replay_helper_threads` | 0 Off, 1 to 7 the parallel replay tried on 2 to 8 threads, 8 Automatic (the default" in guide and
          "`RX_VK_PARALLEL_AUTO=<2..8>`" in guide and "`RX_VK_PARALLEL=0`: never" in guide,
          "docs/development.md does not say the row's numbers, its default and the switches")
    check("| Replay helper threads | Automatic, Off, 2 to 8 threads (the Debug tab)" in (ROOT / "readme.md").read_text(encoding="utf-8"),
          "readme.md's settings table has no Replay helper threads row")

    fixture = FIXTURE.replace("@ROOT@", ROOT.as_posix())
    for mark, text in (("@OF@", of.group(0)), ("@MAX@", top), ("@AUTO@", function(replay, "bool par_auto() {")),
                       ("@TOLD@", function(replay, "int par_threads_told(int threads, const char* by) {")),
                       ("@THREADS@", function(replay, "int par_threads() {")),
                       ("@ENV@", function(menu, "const char* replay_threads_env() {")),
                       ("@TEXT@", function(menu, "const char* tx_replay_threads(int v) {")),
                       ("@HINT@", function(menu, "const char* hn_replay_threads(int v, int variant) {"))):
        fixture = fixture.replace(mark, text)
    plain = {k: v for k, v in os.environ.items() if not k.startswith("RX_VK_PARALLEL")}
    # (row, RX_VK_PARALLEL, RX_VK_PARALLEL_AUTO[, what Automatic gives this PC: 3 unless said]) -> threads at most, auto, threads
    # at the start, the switch the row's line names
    cases = (
        (("none", None, None), (3, 1, 0, "-")),       # no options file, or one without the key: Automatic, the PC's count
        (("none", None, None, 2), (2, 1, 0, "-")),
        (("none", None, None, 0), (0, 0, 0, "-")),    # ...which is none on a PC the rule gives none
        (("none", None, "3"), (3, 1, 0, "RX_VK_PARALLEL_AUTO")),
        (("none", "0", None), (0, 0, 0, "RX_VK_PARALLEL")),   # the switch that turns them off whatever the row is
        ((0, None, None), (0, 0, 0, "-")),            # an options file that says 0: off, the replay thread alone, as it was
        ((1, None, None), (2, 1, 0, "-")),            # the row: Auto on 2 to 8 threads, as RX_VK_PARALLEL_AUTO=<n>
        ((2, None, None), (3, 1, 0, "-")),
        ((7, None, None), (8, 1, 0, "-")),
        ((8, None, None), (3, 1, 0, "-")),            # Automatic: the PC's count, tried as a fixed count is
        ((8, None, None, 2), (2, 1, 0, "-")),
        ((8, None, None, 0), (0, 0, 0, "-")),         # ...and off where the rule gives none
        ((8, None, "5"), (5, 1, 0, "RX_VK_PARALLEL_AUTO")),   # the switches over Automatic too
        ((8, None, "0"), (0, 0, 0, "RX_VK_PARALLEL_AUTO")),
        ((8, "4", None), (4, 0, 4, "RX_VK_PARALLEL")),
        ((9, None, None), (0, 0, 0, "-")),            # a value past the row: off
        ((-1, None, None), (0, 0, 0, "-")),
        ((0, None, "3"), (3, 1, 0, "RX_VK_PARALLEL_AUTO")),   # the switch alone, as before
        ((5, None, "3"), (3, 1, 0, "RX_VK_PARALLEL_AUTO")),   # the switch over the row
        ((5, None, "0"), (0, 0, 0, "RX_VK_PARALLEL_AUTO")),   # ...off too
        ((5, None, "1"), (1, 0, 1, "RX_VK_PARALLEL_AUTO")),   # (1: one thread, the replay's own: not parallel, as before)
        ((0, "4", None), (4, 0, 4, "RX_VK_PARALLEL")),        # always parallel, as before
        ((3, "4", None), (4, 0, 4, "RX_VK_PARALLEL")),        # over the row
        ((3, "0", None), (0, 0, 0, "RX_VK_PARALLEL")),
        ((3, "6", "2"), (6, 0, 6, "RX_VK_PARALLEL")),         # RX_VK_PARALLEL over both
        ((0, "99", None), (8, 0, 8, "RX_VK_PARALLEL")),       # eight at most
    )
    with tempfile.TemporaryDirectory(prefix="replay-threads-") as tmp:
        src, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
        src.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
        for (row, parallel, auto, *given), want in cases:
            env = dict(plain)
            if parallel is not None:
                env["RX_VK_PARALLEL"] = parallel
            if auto is not None:
                env["RX_VK_PARALLEL_AUTO"] = auto
            r = subprocess.run([str(exe), str(row)] + [str(w) for w in want] + [str(given[0] if given else 3)], env=env,
                               capture_output=True, text=True)
            check(r.returncode == 0, "row %s, RX_VK_PARALLEL=%s, RX_VK_PARALLEL_AUTO=%s: %s" % (row, parallel, auto, (r.stderr or r.stdout).strip()))
    print("PASS: the Replay helper threads row is RX_VK_PARALLEL_AUTO's, off by default, under both switches; Automatic's rule; an "
          "options file from before means what it meant")


if __name__ == "__main__":
    main()
