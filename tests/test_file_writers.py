"""The renderer's writer threads at the process's end (handoff §8): gpu/vk/file_writers.h and file_whole.h.

The window's close ends the game through ExitProcess, which stops every other thread wherever it is: a thread in the middle of
a file left part of it. The headers are compiled as they are into a small program whose writer thread writes a file of 2.5 MB
in pieces (about 250 ms), while the main thread, as the game's exit does (vk_replay.cpp option_set, HOS_OPT_EXITING), calls
stop and then ends the process at once (_exit: ExitProcess on Windows, every other thread stopped where it is). Checked on
the file each run leaves:
- with the exit's wait (2 s) the writer's file is the new one, whole;
- with a wait too short for it the write is cut, and the file is the old one, whole (written as .part, then put in place);
- the same write made in place, as before (and with RX_VK_WRITERS_WAIT_MS=0, no wait), is torn by the same end: what the
  test would see if either part were missing;
- a write that would begin after the end has begun does not, on another thread; one inside a write already under way on
  the same thread goes on (a save's own small files), and so do the ending thread's own;
- stop with 0 (RX_VK_WRITERS_WAIT_MS=0) stops nothing.
And the source: every file-writing thread of its own named in file_writers.h holds a Writing over its write (the order
file, the pipeline cache's save in play, the PNG writer, the frame log, the hang report, the predicted pipelines' list),
the prepared record and the .for files are written whole, the exit hook stops them before anything else, and
docs/development.md has the switch.

    python -I -B tests/test_file_writers.py [--cxx clang++]
"""
import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
fails = []

OLD = b"old file, whole\n" * 4096          # 64 KB
PIECE = 64 * 1024
PIECES = 40                                # 2.5 MB, ~5 ms a piece


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        fails.append(what)


FIXTURE = r"""
#include "file_whole.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
static std::atomic<int> began{0};
static const int kPiece = %(piece)d, kPieces = %(pieces)d;
static std::vector<char> piece(int i) { return std::vector<char>((size_t)kPiece, (char)('A' + i %% 26)); }
// (as the game's writers: the whole file to .part, then in place; or, in_place, written over the file itself as before)
static void writer(std::string path, bool in_place) {
    file_writers::Writing w;
    if (!w) { began = -1; return; }
    const std::string part = in_place ? path : path + ".part";
    FILE* f = fopen(part.c_str(), "wb");
    began = 1;
    for (int i = 0; i < kPieces; i++) {
        const std::vector<char> p = piece(i);
        fwrite(p.data(), 1, p.size(), f);
        fflush(f);
        std::this_thread::sleep_for(std::chrono::milliseconds(6));
    }
    fclose(f);
    if (!in_place) file_writers::replace_file(part, path);
}
int main(int argc, char** argv) {
    const std::string mode = argv[1], path = argv[2];
    const int wait = atoi(argv[3]);
    if (mode == "rules") {   // (no process end here: the rules of Writing and stop)
        int bad = 0;
        { file_writers::Writing a; if (!a || file_writers::g_busy != 1) bad |= 1; }
        if (file_writers::g_busy != 0) bad |= 2;
        file_writers::Stopped none = file_writers::stop(0);
        if (file_writers::stopping() || none.left || none.refused) bad |= 4;   // (0: nothing stopped, as before)
        std::atomic<int> nested_ok{0};
        std::atomic<bool> go{false}, held{false};
        std::thread outer([&] {   // (a write under way when the end begins: what it opens inside goes on)
            file_writers::Writing o;
            held = true;
            while (!go) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            file_writers::Writing inner;
            nested_ok = inner ? 1 : 2;
            nested_ok = nested_ok == 1 && file_writers::write_whole(path, "inner", 5) ? 3 : nested_ok.load();
        });
        while (!held) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::thread stopper([&] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); go = true; });
        const file_writers::Stopped s = file_writers::stop(2000);
        outer.join(), stopper.join();
        if (nested_ok != 3) bad |= 8;
        if (s.left != 0 || s.waited_ms < 20) bad |= 16;   // (it waited for the outer write to end)
        int refused = 0;
        std::thread late([&] { file_writers::Writing l; refused = l ? 0 : 1; });   // (begun after the end, elsewhere: refused)
        late.join();
        if (!refused || !file_writers::stopping()) bad |= 32;
        file_writers::Writing own;   // (the ending thread's own: goes on)
        if (!own) bad |= 64;
        if (!file_writers::write_whole(path + ".own", "own", 3)) bad |= 128;
        printf("rules %%d\n", bad);
        return bad;
    }
    const bool in_place = mode == "in_place";
    std::thread t(writer, path, in_place);
    t.detach();
    while (!began) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));   // (the end comes in the middle of the write)
    const file_writers::Stopped s = file_writers::stop(wait);
    printf("stopped left %%d waited %%.0f\n", s.left, s.waited_ms);
    fflush(stdout);
    _exit(0);   // (ExitProcess on Windows: every other thread stopped where it is, as at the game's close)
}
"""


def run_fixture(cxx):
    with tempfile.TemporaryDirectory(prefix="triforce-writers-") as tmp:
        src, exe = Path(tmp, "w.cpp"), Path(tmp, "w.exe")
        src.write_text(FIXTURE % {"piece": PIECE, "pieces": PIECES}, encoding="utf-8")
        subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-pthread", "-I" + str(VK), str(src), "-o", str(exe)],
                       check=True, timeout=300)
        new = b"".join(bytes([65 + i % 26]) * PIECE for i in range(PIECES))

        def end(mode, wait):
            path = Path(tmp, "file_%s_%d.bin" % (mode, wait))
            path.write_bytes(OLD)
            r = subprocess.run([str(exe), mode, str(path), str(wait)], capture_output=True, text=True, timeout=60)
            return path.read_bytes(), r.stdout.strip()

        got, said = end("whole", 2000)
        check(got == new, "the exit waits for a write under way: the new file, whole (%s)" % said)
        got, said = end("whole", 1)
        check(got == OLD, "a write the wait is too short for is cut: the old file, whole (%s)" % said)
        got, said = end("in_place", 1)
        check(got not in (OLD, new), "a write in place cut by the same end is torn (%d bytes): the end does cut it" % len(got))
        got, said = end("in_place", 0)
        check(got not in (OLD, new), "with no wait (RX_VK_WRITERS_WAIT_MS=0) it is cut as before (%d bytes)" % len(got))
        r = subprocess.run([str(exe), "rules", str(Path(tmp, "rules.bin")), "0"], capture_output=True, text=True, timeout=60)
        check(r.returncode == 0, "Writing and stop: nested writes and the ending thread's own go on, a late one elsewhere is "
                                 "refused, stop(0) stops nothing (%s)" % r.stdout.strip())


def check_source():
    def text(rel):
        return (ROOT / rel).read_text(encoding="utf-8")

    def fn(src, marker, end_marker=None):
        at = src.index(marker)
        return src[at:src.index(end_marker, at)] if end_marker else src[at:at + 4000]

    draw, replay, hang = text("gpu/vk/vk_draw.cpp"), text("gpu/vk/vk_replay.cpp"), text("gpu/vk/vk_hangdump.cpp")
    order = fn(draw, "void order_write_maybe() {", "std::thread(write).detach();")
    check(order.index("file_writers::Writing writing;") < order.index('fopen(part.c_str(), "wb")'),
          "vk_draw.cpp order_write_maybe: the order file's write holds a Writing")
    save = fn(draw, "void pipeline_cache_save(", "std::thread(save).detach();")
    check(save.index("file_writers::Writing writing;") < save.index("pcache_play_save(main_built)") < save.index("MoveFileExA(tmp.c_str(), pcache_path.c_str()"),
          "vk_draw.cpp pipeline_cache_save: the save on its own thread holds a Writing over both files")
    check('file_writers::write_whole(pcache_path + ".for"' in save, "vk_draw.cpp: the main cache's .for file is written whole")
    check('file_writers::write_whole(pcache_play_path + ".for"' in fn(draw, "bool pcache_play_save(", "/// ((Triforce) The main file in place"),
          "vk_draw.cpp: the play cache's .for file is written whole")
    check("file_writers::write_whole(path, r, sizeof r);" in fn(draw, "void prepared_write(", "PrepareWhy prepare_check("),
          "vk_draw.cpp prepared_write: the prepared record is written whole")
    check("file_writers::Writing w; w && predicted_file" in fn(draw, "void predicted_done(", "void predict_run("),
          "vk_draw.cpp predicted_done: each key of the predicted list is written under a Writing")
    png = fn(replay, "bool write_png(", "chunk(\"IEND\"")
    check(png.index("file_writers::Writing writing;") < png.index('fopen(temporary.c_str(), "wbx")'),
          "vk_replay.cpp write_png: the pictures' threads hold a Writing over the file")
    flog = fn(replay, "void framelog_write(FILE* f) {", "void framelog_enqueue(")
    check(flog.index("file_writers::Writing writing;") < flog.index("fwrite(rec.data()"),
          "vk_replay.cpp framelog_write: each record is written under a Writing, and the log ends at the exit")
    dump = fn(hang, "bool dump(", "/// The game's own window")
    check(dump.index("file_writers::Writing writing;") < dump.index('fopen(part.c_str(), "w")') < dump.index("file_writers::replace_file(part, path)"),
          "vk_hangdump.cpp dump: the hang report is written whole under a Writing")
    check("if (file_writers::stopping()) return;" in fn(hang, "void hang_watch_start() {"), "vk_hangdump.cpp: the hang watch ends at the exit")
    hook = fn(replay, "t.option_set = [](int id, int value) {")
    check(hook.index("file_writers::stop(file_writers::exit_wait_ms())") < hook.index("framegen_exiting()") < hook.index("dlssg::shutdown()"),
          "vk_replay.cpp option_set: HOS_OPT_EXITING waits for the writers first")
    check("`RX_VK_WRITERS_WAIT_MS=<ms>`" in text("docs/development.md"), "docs/development.md's switches table has RX_VK_WRITERS_WAIT_MS")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx")
    a = ap.parse_args()
    check_source()
    cxx = (shutil.which("clang++") if os.name == "nt" else None) or a.cxx or "g++"
    run_fixture(cxx)
    if fails:
        raise SystemExit("%d failed" % len(fails))
    print("all passed")


if __name__ == "__main__":
    main()
