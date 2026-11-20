"""The pipelines made ahead in the order the last sessions' draws first needed them (gpu/vk/pipe_order.h; vk_draw.cpp
graphics_pipeline, order_met, order_wanted, warm_hold, warm_link_take, warm_variant, pipelines_warm, warm_index_set,
order_index_set_locked, order_write_maybe, pipelines_release_tick; no game, no GPU).

With the programs read ahead in the sessions' order the save's load-in frame read none of its programs any more (lab round 37),
but still made its pipelines on the draws' threads, 86 to 92 ms of it: the warm (pipe_warm.h) made the pipelines the notes name
for each program read, every one any session built (1964 links, 54 taken), and its bounds let go the nearest. Now each session
writes the order its draws first needed their pipelines in beside the notes, and the next one's warm makes those, and only
those, for each program variant the read-ahead reads, holding their links by place, the farthest let go beyond the bound.

Compiles and runs tests/test_pipe_order.cpp (the header: the file with its own magic, the index by variant, where the draws
are, the links held with their bound and what goes behind the draws, the libraries let go with them, and a synthetic S1
session through the program read-ahead's rules beside the notes' warm as it was), and reads the source for what needs the
game: that a draw's first need of a pipeline goes into the order after the warm's link for it was taken, that with an order
the warm takes the order's pipelines alone and hands their places on, that one past the draws or beyond the bound is not
made, that the index is built where the warm's notes are and only then is the order used, that the warm does not follow the
draws with an order, that the order is written beside the notes from the presents through a file of its own on a thread of
its own, and that the switch is in the development guide (tests/test_pipe_warm.py runs vk_draw.cpp's held links and their
libraries; tests/test_stall_watch.py the stall line's counts).

    python -I -B tests/test_pipe_order.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def function(text, head):
    at = text.index(head)
    return text[at:text.index("\n}\n", at)]


def check_source():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    gp = function(draw, "PipeEntry* graphics_pipeline(")
    found = gp[gp.index("auto it = pipelines.find(key);"):gp.index("pcache_init();")]
    if "!found->looked.exchange(true)) st_pipe_looked++, order_met(key);" not in found:
        raise SystemExit("vk_draw.cpp graphics_pipeline: a draw's first need of a pipeline the precompile made is not in the order")
    take = gp.index("VkPipeline linked = warm_link_take(key);")
    met = gp.index("order_met(key);   // ((Triforce, wp18)")
    if not take < met < gp.index("auto job = std::make_shared<Job>();"):
        raise SystemExit("vk_draw.cpp graphics_pipeline: a new pipeline goes into the order before the warm's link for it is "
                         "taken (the draws' move could let it go), or not at all")
    om = function(draw, "void order_met(uint64_t key) {")
    for need in ("if (!pipe_order_on()) return;", "pipe_order_rec.add(key);",
                 "!pipe_order_front.met(pipe_order_index.at(key))", "pipe_order_links.behind(from, gone);",
                 "order_gone_locked(gone, true);"):
        if need not in om:
            raise SystemExit("vk_draw.cpp order_met lacks: " + need)
    if om.index("std::lock_guard lk(gpl_mu);") < om.index("from = pipe_order_front.from();"):
        raise SystemExit("vk_draw.cpp order_met: gpl_mu taken under pipe_order_mu")
    wanted = function(draw, "bool order_wanted(int64_t place) {")
    if "pipe_order_front.passed(place)" not in wanted or "pipe_order_links.room(place)" not in wanted:
        raise SystemExit("vk_draw.cpp order_wanted: one past the draws or beyond the links' bound is made")
    var = function(draw, "void warm_variant(Program& p, FILE* in, const std::vector<uint32_t>& recs, bool links, const int64_t* places")
    if var.index("if (place >= 0 && !order_wanted(place)) continue;") > var.index("pipe_note_at(in, rec, *n)") or \
            "warm_hold(key, link, lib_keys, m, libs, place);" not in var:
        raise SystemExit("vk_draw.cpp warm_variant: the order's place not asked before the note is read, or not handed to the hold")
    pw = function(draw, "void pipelines_warm(std::vector<std::unique_ptr<Program>>& variants, int64_t at) {")
    used = pw.index("if (pipe_order_used.load(std::memory_order_relaxed)) {")
    if not used < pw.index("pipe_order_index.take(mix(p->key, p->variant))") < pw.index("if (warm_index.empty()) return;") or \
            "warm_variant(*p, in, recs, links, places.empty() ? nullptr : places.data());" not in pw:
        raise SystemExit("vk_draw.cpp pipelines_warm: with an order, not its pipelines alone, or their places not handed on")
    index = function(draw, "void warm_index_set(")
    if index.index("order_index_set_locked(notes, at);") < index.index("pipe_warm::index_add(warm_index"):
        raise SystemExit("vk_draw.cpp warm_index_set: the order is not indexed where the warm's notes are")
    oi = function(draw, "void order_index_set_locked(")
    for need in ("if (!pipe_order_on()) return;", "keys = order_before_locked();", "pipe_order_index.build(keys,",
                 "*rec = at[it->second];", "pipe_order_used.store(pipe_order_index.named > 0"):
        if need not in oi:
            raise SystemExit("vk_draw.cpp order_index_set_locked lacks: " + need)
    if "return on && !pipe_order_used.load(std::memory_order_relaxed);" not in function(draw, "bool warm_follow() {"):
        raise SystemExit("vk_draw.cpp warm_follow: the warm follows the draws with an order")
    if "pipe_order::kLibsMax" not in function(draw, "size_t warm_libs_most() {") or \
            "pipe_warm::lib_room(gpl_warm_only.size(), warm_libs_most())" not in function(draw, "VkPipeline warm_part("):
        raise SystemExit("vk_draw.cpp warm_part: the order's bound of libraries is not the warm's with an order")
    write = function(draw, "void order_write_maybe() {")
    for need in ("std::unique_lock lk(pipe_order_mu, std::try_to_lock);", "pipe_order_rec.due(", "pipe_order::merge(now, "
                 "order_before_locked())", 'path + ".part"', "MoveFileExA(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)",
                 "std::thread(write).detach();", "pipe_order_writing.store(false"):
        if need not in write:
            raise SystemExit("vk_draw.cpp order_write_maybe lacks: " + need)
    if '"triforce_pipeline_order.bin"' not in function(draw, "std::string pipe_order_path() {") or \
            "pipe_notes_path()" not in function(draw, "std::string pipe_order_path() {"):
        raise SystemExit("vk_draw.cpp pipe_order_path: not triforce_pipeline_order.bin beside the pipeline notes")
    tick = function(draw, "void pipelines_release_tick(uint64_t presents) {")
    if tick.index("order_write_maybe();") > tick.index("if (!on ||"):
        raise SystemExit("vk_draw.cpp pipelines_release_tick: the order is not written from the presents")
    if 'pipe_order::on(getenv("RX_VK_PIPELINE_ORDER"))' not in function(draw, "bool pipe_order_on() {"):
        raise SystemExit("vk_draw.cpp pipe_order_on does not read RX_VK_PIPELINE_ORDER")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    if "`RX_VK_PIPELINE_ORDER=0`" not in dev:
        raise SystemExit("docs/development.md: RX_VK_PIPELINE_ORDER is not in the switches table")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="pipe-order-") as tmp:
        exe = Path(tmp) / "pipe-order.exe"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "gpu/vk"),
                        str(ROOT / "tests/test_pipe_order.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: the pipelines made ahead in the sessions' order")


if __name__ == "__main__":
    main()
