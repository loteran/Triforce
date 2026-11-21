"""Checkpoint saves of the pipeline cache during the shader preparation's long builds (gpu/vk/warm_checkpoint.h; vk_draw.cpp
warm_libraries, precompile_run, pcache_checkpoint_save, PcacheCheckpoints; no game or GPU).

A cold driver's part made about 91,600 stage libraries in two hours on an RX 7900 XT, and the pipeline cache was written only
when it ended or was stopped in order: a PC that restarted 57 minutes in had nothing of it on the disk. The header is compiled
and run as it is: RX_VK_WARM_SAVE_MIN (10 unset, 0 none), the card's rule (not on Intel while the workers build unless
RX_VK_PCACHE_PLAY_SAVE=1; =0 on no card, as pipeline_cache_save's save in play), when a checkpoint is due (the minutes since
the last save and kMinBuilt built since it: no save for nothing), and a checkpoint that wrote nothing asked again a minute
later. Then the source: warm_libraries and precompile_run look at it in their watch loops and wait for one under way once
their workers are joined; the save goes through pipeline_cache_save(true) (the carry's cap and the bound before the data is
fetched, the .tmp put in place, .for and .base) after a RAM check looked at now, on a thread of its own, and logs only when
something was written; and nothing in it says the part is made (the prepare's record, lib_check's, the journal, the carry's
close). The save itself, a carried session's checkpoint kept capped at the start after a crash, is run by
tests/test_pipeline_cache_save.py (case checkpoint).
python -I -B tests/test_warm_checkpoint.py --cxx clang++
"""
import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <cstdio>
#include "warm_checkpoint.h"
#define CHECK(x) do { if (!(x)) { std::printf("line %d: %s\n", __LINE__, #x); return 1; } } while (0)
using namespace warm_checkpoint;
int main() {
    // (RX_VK_WARM_SAVE_MIN: 10 unset, empty or not a number; 0 or below none; at most a day)
    CHECK(minutes(nullptr) == 10 && minutes("") == 10 && minutes("x") == 10 && minutes("10") == 10);
    CHECK(minutes("0") == 0 && minutes("-5") == 0 && minutes("1") == 1 && minutes("30") == 30 && minutes("99999") == kMaxMin);
    // (the card: any but Intel's unset; RX_VK_PCACHE_PLAY_SAVE=1 on Intel too, =0 on none)
    CHECK(card_ok(0x1002, nullptr) && card_ok(0x10DE, nullptr) && !card_ok(0x8086, nullptr));
    CHECK(card_ok(0x8086, "1") && !card_ok(0x1002, "0") && !card_ok(0x10DE, "0") && !card_ok(0x8086, "0"));
    // (due: the minutes past and kMinBuilt built since the last save, the count moved just now; off, or not on this card,
    // never)
    const uint64_t MIN = 60000;
    Clock c;
    CHECK(!due(10, true, c, 10 * MIN - 1, 50000, 10 * MIN - 1) && due(10, true, c, 10 * MIN, 1000, 10 * MIN));
    CHECK(!due(10, true, c, 10 * MIN, 999, 10 * MIN));
    CHECK(!due(0, true, c, 1000 * MIN, 90000, 1000 * MIN) && !due(10, false, c, 1000 * MIN, 90000, 1000 * MIN));
    CHECK(!due(10, true, c, 120 * MIN, 0, 120 * MIN));   // (nothing built, no save)
    // (a part stuck in one build, the 1700 since the last save built before it: the count still for kQuietMs, no save, and
    // its line is not the one that keeps Setup's silence watch from seeing it)
    CHECK(kQuietMs == 2 * MIN && due(10, true, c, 20 * MIN, 5000, 18 * MIN) && !due(10, true, c, 20 * MIN, 5000, 18 * MIN - 1));
    CHECK(!due(10, true, c, 20 * MIN, 5000, 20 * MIN + 1));   // (a time of the count's ahead of now: not due, no wrap)
    Moved m;
    m.look(1000, 0);
    CHECK(m.at_ms == 0);   // (nothing built yet: not moved)
    m.look(2000, 5);
    m.look(3000, 5);
    CHECK(m.at_ms == 2000 && m.built == 5);
    m.look(4000, 6);
    CHECK(m.at_ms == 4000 && m.built == 6);
    // (saved: counted from there)
    c = saved(10 * MIN, 9000);
    CHECK(c.from_ms == 10 * MIN && c.from_built == 9000);
    CHECK(!due(10, true, c, 19 * MIN, 30000, 19 * MIN) && !due(10, true, c, 20 * MIN, 9999, 20 * MIN));
    CHECK(due(10, true, c, 20 * MIN, 10000, 20 * MIN));
    // (wrote nothing at 20 min: asked again a minute later, what was built since the last save still counted)
    const Clock s = skipped(c, 10, 20 * MIN);
    CHECK(s.from_built == 9000 && !due(10, true, s, 21 * MIN - 1, 20000, 21 * MIN - 1) && due(10, true, s, 21 * MIN, 20000, 21 * MIN));
    CHECK(skipped(Clock{}, 1, 5 * MIN).from_ms == 5 * MIN && due(1, true, skipped(Clock{}, 1, 5 * MIN), 6 * MIN, 1000, 6 * MIN));
    // (a clock ahead of the time, or of the count, is not due: no wrap)
    CHECK(!due(10, true, Clock{30 * MIN, 0}, 20 * MIN, 5000, 20 * MIN) && !due(10, true, Clock{0, 5000}, 30 * MIN, 4000, 30 * MIN));
    // (room: the size asked and room for what is built meanwhile, at least kRoomMin or a 32nd, never wrapping)
    const size_t MB = 1u << 20;
    CHECK(room(0) == kRoomMin && room(600 * MB) == 664 * MB && room(2600 * MB) == 2600 * MB + 2600 * MB / 32);
    CHECK(room(SIZE_MAX - 1) == SIZE_MAX && room(SIZE_MAX) == SIZE_MAX && kFetchTries >= 2);
    // (the disk: the data and kDiskSpare free beside the old file; not known, as before: no refusal)
    const uint64_t GB = 1ull << 30;
    CHECK(disk_fits(true, 4 * GB, 3 * GB) && !disk_fits(true, 4 * GB - 1, 3 * GB) && !disk_fits(true, GB / 2, 1));
    CHECK(disk_fits(false, 0, 3 * GB) && kChunk > 0);
    return 0;
}
"""


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def need(where, text, parts):
    for p in parts:
        if p not in text:
            raise SystemExit(f"{where}: missing {p!r}")


def in_order(where, text, parts):
    at = -1
    for p in parts:
        i = text.find(p, at + 1)
        if i < 0:
            raise SystemExit(f"{where}: {p!r} missing or out of order")
        at = i


def check_source():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    need("vk_draw.cpp", draw, ['#include "warm_checkpoint.h"'])

    # ---- the save: a RAM check looked at now, then the preparation's own save, whose rules hold
    save = between(draw, "size_t pcache_checkpoint_save() {", "\n}\n")
    in_order("pcache_checkpoint_save", save, [
        "const auto use = pcache_use();",
        "vkGetPipelineCacheData(dev.device, use.cache, &n, nullptr)",
        "if (!memory_ram_fits_now(std::max(warm_checkpoint::room(n), play ? warm_checkpoint::room(play) : 0))) return 0;",
        "Now() { pcache_checkpoint_now = true; }",
        "~Now() { pcache_checkpoint_now = false; }",
        "pipeline_cache_save(true);",
        "return (main_now != main_was ? main_now : 0) + (play_now != play_was ? play_now : 0);"])
    # (pipeline_cache_save's rules on that path: Writing, the carry's cap before the bound, both before the data is fetched,
    # the .tmp put in place, .for and the carry's history beside it)
    pcs = between(draw, "void pipeline_cache_save(bool force) {", "\n}\n")
    in_order("pipeline_cache_save", pcs, [
        "static std::atomic<bool> busy{false};",
        "const bool checkpoint = force && pcache_checkpoint_now;",
        "const file_writers::Writing writing;",
        "if (!pcache_play_save(main_built) && pcache_preparing.load() == 0) return;",
        "if (!pcache_carry_save_ok(n)) return;",
        "if (!pcache_bound::within(n, pcache_bound_bytes)) {",
        # (a checkpoint: not once a stop is asked, nor with the disk short; fetched with room, asked again past it, the cap and
        # the bound again on what was fetched)
        "if (checkpoint && (prepare_stopped() || !pcache_disk_fits(n))) return;",
        "if (checkpoint) n = warm_checkpoint::room(n);",
        "std::vector<uint8_t> data(n);",
        "VkResult got = vkGetPipelineCacheData(dev.device, pcache, &n, data.data());",
        "for (int t = 1; checkpoint && got == VK_INCOMPLETE && t < warm_checkpoint::kFetchTries; t++) {",
        "if (!pcache_carry_save_ok(n) || !pcache_bound::within(n, pcache_bound_bytes)) return;",
        "if (got == VK_SUCCESS && pcache_carry_save_ok(n) && pcache_bound::within(n, pcache_bound_bytes)) {",
        'const std::string tmp = pcache_path + ".tmp";',
        # (its parts given up between them on a stop or on the carried cache's drop, and not flushed for a drop: the
        # rebuild waits for this thread in checkpoints.end())
        "ok = !(checkpoint && (prepare_stopped() || pcache_carry_over.load())) && fwrite(data.data() + at, 1, len, f) == len;",
        "if (checkpoint) ok = ok && !pcache_carry_over.load() && pcache_file_flush(f);",
        "ok = fclose(f) == 0 && ok;",
        "if (checkpoint && prepare_stopped()) ok = false;",
        # (nor once the carried cache was dropped while it wrote: the early drop, or past its limit on another save)
        "if (checkpoint && pcache_carry_over.load()) ok = false;",
        # (a carried session's history before the data, pcache_carry.h begun; the file's .own)
        "if (ok && !keep_old) ok = pcache_carry_saving(n, want);",
        "if (ok) pcache_own_mark();",
        "MoveFileExA(tmp.c_str(), pcache_path.c_str(),",
        "MOVEFILE_REPLACE_EXISTING | (checkpoint ? MOVEFILE_WRITE_THROUGH : 0))",
        'file_writers::write_whole(pcache_path + ".for", want, 16);',
        "pcache_carry_saved(n, want);",
        "remove(tmp.c_str());"])
    # (the play file's fetch, a checkpoint's in the precompile: with room too, and flushed)
    play = between(draw, "bool pcache_play_save(int64_t main_built) {", "\n}\n")
    in_order("pcache_play_save", play, [
        "const bool checkpoint = pcache_checkpoint_now;",
        "if (checkpoint) n = warm_checkpoint::room(n);",
        "std::vector<uint8_t> data(n);",
        "for (int t = 1; checkpoint && got == VK_INCOMPLETE && t < warm_checkpoint::kFetchTries; t++) {",
        "if (got != VK_SUCCESS || !n) return w.main;",
        "if (checkpoint) ok = ok && pcache_file_flush(f);",
        "MOVEFILE_REPLACE_EXISTING | (checkpoint ? MOVEFILE_WRITE_THROUGH : 0)"])
    saving = between(draw, "bool pcache_carry_saving(size_t n, const char* stamp) {", "\n}\n")
    in_order("pcache_carry_saving", saving, ["if (!pcache_carried || !pcache_side.open) return true;",
                                             "pcache_carry::put(pcache_carry::saved(pcache_side, n, stamp), out);",
                                             'file_writers::write_whole(pcache_path + ".base", out, sizeof out);'])
    # (a checkpoint's file of the part before a crash is not taken for an older build's: <file>.own, pcache_from_own)
    own = between(draw, "void pcache_own_mark() {", "\n}\n")
    need("pcache_own_mark", own, ["if (pcache_carried || (pcache_from_file && !pcache_from_own)) {",
                                  "pcache_own_stamp()", "file_writers::write_whole(own, stamp, 16)"])
    init = between(draw, "void pcache_init() {", "\n}\n")
    in_order("pcache_init", init, ["pcache_from_file = pcache && ci.initialDataSize > 0;",
                                   "pcache_from_own = pcache_from_file && !pcache_carried && pcache_own_named();"])
    named = between(draw, "bool pcache_own_named() {", "\n}\n")
    need("pcache_own_named", named, ['fopen((pcache_path + ".own").c_str(), "rb")', "pcache_own_stamp()",
                                     "fread(own, 1, 16, f) == 16 && !memcmp(own, want, 16)"])
    need("pcache_own_stamp", draw, ['uint64_t pcache_own_stamp() { return libraries_stamp(gpl_wanted() && !getenv("RX_VK_NO_WARM")); }'])
    # (the carry's history as the session has it, open for a carried one: a checkpoint keeps it open, as a stopped run's save)
    carry = between(draw, "void pcache_carry_saved(size_t n, const char* stamp) {", "\n}\n")
    need("pcache_carry_saved", carry, ["pcache_side = pcache_carry::saved(pcache_side, n, stamp);"])
    memory = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    fits = between(memory, "bool memory_ram_fits_now(uint64_t bytes) {", "\n}\n")
    need("memory_ram_fits_now", fits, ["ram_now(&avail, &total)", "mem_parts::copy_fits(avail, bytes)"])
    need("vk_internal.h", (ROOT / "gpu/vk/vk_internal.h").read_text(encoding="utf-8"),
         ["bool memory_ram_fits_now(uint64_t bytes);"])

    # ---- the checkpoints' look: the switch, the card, due, a thread of its own, a line only when something was written
    cp = between(draw, "struct PcacheCheckpoints {", "\n};\n")
    need("PcacheCheckpoints", cp, [
        'warm_checkpoint::minutes(getenv("RX_VK_WARM_SAVE_MIN"))',
        'warm_checkpoint::card_ok(dev.props.vendorID, getenv("RX_VK_PCACHE_PLAY_SAVE"))',
        "~PcacheCheckpoints() { end(); }"])
    look = between(cp, "void look(size_t built) {", "\n    }\n    void end() {")
    in_order("PcacheCheckpoints::look", look, [
        "if (!minutes || !card) return;",
        "moved.look(now, built);",
        "if (busy.load() || !warm_checkpoint::due(minutes, card, clock, now, built, moved.at_ms)) return;",
        "busy = true;",
        "saving = std::thread(",
        "wrote = pcache_checkpoint_save();",
        "if (wrote) {",
        'logf("vk: %s saved for a restart: %zu MB in %.1f s (%zu %s; RX_VK_WARM_SAVE_MIN=0: only at the end)"',
        "clock = warm_checkpoint::saved(now, built);",
        "clock = warm_checkpoint::skipped(clock, minutes, now);",
        "busy = false;"])
    if look.count("logf(") != 2 or "checkpoint save failed" not in look:
        raise SystemExit("PcacheCheckpoints::look: a line besides the save's and its failure's (a stuck part must go silent)")
    # (the save's own lines for a checkpoint: once each, and only for one that was due, so with the count moving)
    for text in ("the pipeline cache's checkpoint could not fetch it whole", "the pipeline cache's checkpoint waits:"):
        at = draw.index(text)
        if "if (!told.exchange(true))" not in draw[at - 200:at]:
            raise SystemExit("a checkpoint's line is said more than once: " + text)
    # (honest: a checkpoint says nothing of the part being made, and does not close a carried session's history)
    for part, text in (("pcache_checkpoint_save", save), ("PcacheCheckpoints", cp)):
        for word in ("prepared_write", "lib_check_made", "warm_end", "warm_journal", "pcache_carry_done", "pcache_carry_close",
                     "built_write", "pcache_saved_now", "pcache_side"):
            if word in text:
                raise SystemExit(f"{part}: {word}: a checkpoint must not say the part is made")
    # (no priority of its own: tests/test_prepare_timing.py counts prepare_priority's uses)
    if "SetThreadPriority" in cp or "prepare_priority" in cp:
        raise SystemExit("PcacheCheckpoints: its thread takes no priority of its own")

    # ---- warm_libraries: looked at in the watch loop, with the carry's look; one under way waited for once the workers are
    # joined, before the part's own end and its save
    warm = between(draw, "bool warm_libraries(void (*progress)(size_t done, size_t total)) {", "\n}\n")
    loop = between(warm, "while (done.load() < total && !prepare_stopped() && !pcache_carry_over.load()) {",
                   "for (std::thread& t : threads) t.join();")
    in_order("warm_libraries' watch loop", loop, ["Sleep(100);", "pcache_carry_early(carry_from, done.load(), total, libs.load());",
                                                  "carry_sampled = true;", "pcache_carry_check(true);", 'timing(" so far");',
                                                  "checkpoints.look(libs.load());"])
    # (the carry's sample: the workers past it wait for its projection, not the watch loop, which keeps the checkpoints due-checked
    # meanwhile; checkpoints.look is at the loop's level, after the early look, not inside its branch)
    if "Sleep(5)" in loop or "while (!carry_sampled" in loop:
        raise SystemExit("warm_libraries' watch loop waits for the carry's sample: its checkpoints would not be looked at")
    early = between(loop, "if (!carry_sampled.load() && done.load() >= carry_sample) {", "}")
    if "checkpoints" in early:
        raise SystemExit("warm_libraries: the checkpoints are looked at only with the carry's sample")
    in_order("warm_libraries", warm, [
        "if (pcache_carry_check(false)) {",   # (a carried cache past its limit already: nothing built, no checkpoint)
        'PcacheCheckpoints checkpoints("the driver\'s part so far", "stage libraries");',
        "while (done.load() < total && !prepare_stopped() && !pcache_carry_over.load()) {",
        "checkpoints.look(libs.load());",
        "for (std::thread& t : threads) t.join();",
        "checkpoints.end();",
        'timing("");',
        "if (prepare_stopped()) {",
        "if (pcache_carry_check(false)) {",
        "const bool saved = pcache_saved_now();"])
    if warm.count("checkpoints.look(") != 1:
        raise SystemExit("warm_libraries: the checkpoints are looked at outside its watch loop")

    # ---- precompile_run: the same, before the workers' state goes and before the end's save and record
    pre = between(draw, "void precompile_run(void (*progress)(size_t done, size_t total)) {", "\n}\n")
    in_order("precompile_run", pre, [
        'PcacheCheckpoints checkpoints("precompile: the pipelines so far", "pipelines");',
        "while (done.load() < total && !prepare_stopped()) {",
        "checkpoints.look(done.load());",
        "checkpoints.end();",
        "for (std::thread& t : threads) t.join();",
        "delete pre;",
        "pipeline_cache_save(true);",
        "built_write(built_stamp, built);"])

    dev_md = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    if "| `RX_VK_WARM_SAVE_MIN=<minutes>` |" not in dev_md:
        raise SystemExit("RX_VK_WARM_SAVE_MIN is not in docs/development.md's switches")
    rows = dev_md.index("| `RX_VK_PCACHE_PLAY_FILE=0`"), dev_md.index("| `RX_VK_WARM_SAVE_MIN=<minutes>` |")
    if not 0 < rows[1] - rows[0] < 2000:
        raise SystemExit("RX_VK_WARM_SAVE_MIN's row is not beside the other pipeline cache rows")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("clang++") or "g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="warm-checkpoint-") as tmp:
        tmp = Path(tmp)
        (tmp / "t.cpp").write_text(FIXTURE, encoding="utf-8")
        exe = tmp / "warm-checkpoint"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "gpu/vk"), str(tmp / "t.cpp"),
                        "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"warm_checkpoint.h: {r.stdout.strip()}")
    print("PASS: the preparation saves the pipeline cache every RX_VK_WARM_SAVE_MIN minutes while it builds, through the "
          "preparation's own save, and never says the part is made")


if __name__ == "__main__":
    main()
