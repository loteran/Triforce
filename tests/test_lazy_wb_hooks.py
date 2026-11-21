"""The lazy write-back's hooks in the renderer (gpu/vk/vk_memory.cpp "lazy write-back"; source read, no game, no GPU).

The page states themselves are tests/test_wb_lazy_state.py's. This holds the renderer to the rest of the design
(docs/lazy-writeback.md):

- off is off: every hook asks lazy_on() first (the Debug tab's row, RX_VK_LAZY_WB), the table is used nowhere but in the
  lazy write-back's own section, and memory_end_submission's part of it hangs on one number that is 0 with the row off;
- every GPU read of guest memory through a pool's import pays first: each copy out of an import outside the write-back
  itself has a lazy_settle before it in its function, gpu_buffer settles, and every binding gpu_buffer_bind gives the
  import goes through in_guest;
- code that gives a mirror or an import up settles first (mirror_free, retire_pool, pieces_set_direct, imports_give_back),
  and import_pool's commit and import run between memory_lazy_io_begin and its end;
- the payment needs neither pools_mu nor replay_mu and logs nothing (its thread may hold either, or the log's lock);
- (Triforce) each payment copies with the command buffer of the payer's slot it holds (wb_lazy_payers.h,
  RX_VK_LAZY_PAYERS), the submit's own copy with one apart from those, and the slots are made before they are handed out;
- the fault handler is put first among the vectored handlers, on Windows only;
- vk_replay.cpp's submit_recording tells it of every recording on the queue, under the queue's lock, in both of its ways.

    python -I -B tests/test_lazy_wb_hooks.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
problems = []


def check(cond, what):
    if not cond:
        problems.append(what)


def function(text, name):
    """The body of the function defined at column 0 whose head names name( (its first such definition)"""
    m = re.search(r"^[A-Za-z][^\n;{}]*\b" + re.escape(name) + r"\([^;{}]*\)\s*(const\s*)?\{[^\n]*\n", text, re.M)
    if not m:
        problems.append("no definition of %s in the file" % name)
        return ""
    end = text.find("\n}\n", m.end())
    return text[m.start():end + 3]


def enclosing(text, at):
    """The column-0 definition around position at: (its head, where it starts)"""
    heads = [m for m in re.finditer(r"^[A-Za-z][^\n;]*\)\s*(const\s*)?\{[^\n]*\n", text[:at], re.M)
             if not m.group(0).startswith("namespace")]
    if not heads:
        return "", 0
    h = heads[-1]
    if text.find("\n}\n", h.end()) < at:   # (not inside that one: the scan lost its place)
        return "", 0
    return h.group(0), h.start()


def main():
    mem = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    start = mem.index("// ---- lazy write-back (Triforce;")
    end = mem.index("\n}\n", mem.index("void memory_lazy_submitted() {")) + 3
    section, outside = mem[start:end], mem[:start] + mem[end:]

    # ---- off is off
    check("lz->" not in outside, "the lazy write-back's table is used outside its section of vk_memory.cpp")
    check(re.search(r"inline void lazy_settle\(uint64_t lo, uint64_t hi, wb_lazy::Why why\) \{\n    if \(lazy_on\(\) && hi > lo\)",
                    section), "lazy_settle does not ask lazy_on() first")
    for name, first in (("memory_lazy_io_begin", "if (!lazy_on()) return false;"), ("memory_lazy_submitted", "if (!lazy_on()) return;"),
                        ("lazy_report", "if (!lazy_on()) return;")):
        body = function(section, name)
        check(body.split("{\n", 1)[-1].lstrip().startswith(first), "%s does not begin with %s" % (name, first))
    check("void memory_lazy_io_end(bool held) {\n    if (held) lz->io_end();" in section,
          "memory_lazy_io_end ends more than its own begin")
    collect = function(mem, "memory_collect_written")
    check("const uint64_t lz_no = lazy_on() ? ++lz_collects_begun : 0;" in collect and
          collect.index("lz_collects_begun") < collect.index("host->collect_written(&pages)"),
          "memory_collect_written does not number its collection, before the sweep and only with the row on")
    capture = function(mem, "capture_make")
    check("const uint64_t lz_no = lazy_on() ? ++lz_caps_made : 0;" in capture and "if (lz_no) lazy_capture_taken(pages, n, lz_no);" in capture
          and capture.index("++lz_caps_made") < capture.index("host->collect_written_in("),
          "capture_make does not number its capture before it collects, and name its pages after, with the row on alone")
    wb = function(mem, "memory_end_submission")
    check("const uint64_t lz_seq = lazy_on() && lazy_made() ? lz_ended.load() + 1 : 0;" in wb,
          "memory_end_submission's lazy part does not hang on lz_seq, 0 with the row off")
    for use in re.findall(r"[^\n]*\blz(?:_events|_ended|_seq)\b[^\n]*", wb):
        if "const uint64_t lz_seq" in use or "lz_events;" in use or "lz_events.clear()" in use:
            continue
        check(re.search(r"if \(lz_seq", use), "memory_end_submission uses the lazy write-back without lz_seq: " + use.strip())
    check(wb.count("lazy_runs(") == 1 and "lazy_report();" in wb, "memory_end_submission does not go through lazy_runs once, "
          "or does not log the lazy write-back's line")
    runs_at, wbd_at = wb.find("lazy_runs("), wb.find("if (diff && !p->shadow")
    check(0 <= runs_at < wbd_at and wb.find("clip_resident(*p, runs);") < runs_at,
          "lazy_runs is not where the runs are cut and clipped and before they are copied or diffed")

    # ---- every GPU read of guest memory through an import pays first
    for m in re.finditer(r"vkCmdCopyBuffer\([^,]+, *(\w+)(->|\.)buffer\b", mem):
        head, begun = enclosing(mem, m.start())
        name = re.search(r"(\w+)\(", head).group(1) if head else "?"
        if name == "memory_end_submission":   # (the shadows' fills: of the runs lazy_runs has cut the owed pages out of)
            continue
        before = mem[begun:m.start()] if head else ""
        check("lazy_settle(" in before, "%s copies out of an import (%s) with no lazy_settle before it" % (name, m.group(0)))
    gbuf = function(mem, "gpu_buffer")
    check(re.search(r"lazy_settle\(guest, guest \+ size, wb_lazy::BY_GPU\);[^\n]*\n\s*return \{p->buffer", gbuf),
          "gpu_buffer gives the import without settling the range")
    bind = function(mem, "gpu_buffer_bind")
    for m in re.finditer(r"\{p->buffer, off, size, 0\}", bind):
        lead = bind[max(0, m.start() - 20):m.start()]
        check(lead.endswith("in_guest(") or lead.endswith("in_guest(BufferRef"),
              "gpu_buffer_bind gives the import without in_guest: ..." + bind[m.start() - 40:m.end()])
    check("lazy_settle(guest, guest + size, wb_lazy::BY_GPU);" in bind, "gpu_buffer_bind's in_guest does not settle")

    # ---- giving up
    for name in ("mirror_free", "retire_pool", "imports_give_back"):
        check("wb_lazy::BY_GIVEUP" in function(mem, name), name + " does not settle what it gives up")
    psd = function(mem, "pieces_set_direct")
    check("BY_GIVEUP" in psd and psd.index("BY_GIVEUP") < psd.index("__atomic_fetch_and(&p.res_bits"),
          "pieces_set_direct takes a piece out of the mirror before settling it")
    imp = function(mem, "import_pool")
    check("lazy_io{memory_lazy_io_begin(ptr, bytes)}" in imp and imp.index("memory_lazy_io_begin") < imp.index("VirtualAlloc("),
          "import_pool commits and imports without memory_lazy_io_begin first")

    # ---- the payment: no renderer lock, no log
    for name in ("lazy_gpu_begin", "lazy_gpu_submit", "lazy_copy", "lazy_check", "lazy_pay", "lazy_settle_slow", "lazy_veh"):
        body = function(section, name)
        check("logf(" not in body, name + " logs: a payment's thread may hold the log's lock")
        check("pools_mu" not in body and "replay_mu" not in body, name + " takes pools_mu or replay_mu")
    # ---- (Triforce) the payers' slots (wb_lazy_payers.h, RX_VK_LAZY_PAYERS): each payment copies with the command buffer of
    # the slot it holds, never one another payment or the submit's own copy may be recording into at the same time
    pay = function(section, "lazy_pay")
    check("const wb_lazy::Payers::Hold slot(lz_payers);" in pay and pay.count("lz_gpu[slot.slot]") == 2 and
          "std::mutex lz_gpu_mu" not in section and "lz_gpu[0]" not in section,
          "lazy_pay does not copy (and check) with the command buffer of the payer's slot it holds")
    check(pay.find("Payers::Hold slot(") < pay.rfind("lz_gpu[slot.slot]") < pay.find("lz->end_pay("),
          "lazy_pay ends the payment before its copy")
    check("constexpr int kLzSubmitGpu = wb_lazy::kMaxPayers;" in section and "} lz_gpu[wb_lazy::kMaxPayers + 1];" in section,
          "the submit's own command buffer is not apart from the payers' slots")
    check("lazy_copy(lz_gpu[kLzSubmitGpu], jobs, true)" in function(section, "memory_lazy_submitted"),
          "memory_lazy_submitted does not copy with the submit's own command buffer")
    made = function(section, "lazy_made")
    check('wb_lazy::payers_from(getenv("RX_VK_LAZY_PAYERS"))' in made and "lazy_gpu_make(lz_gpu[payers], lz_mode == 2)" in made and
          "lz_payers.init(payers);" in made and "lazy_gpu_make(lz_gpu[kLzSubmitGpu], false)" in made,
          "lazy_made does not make a slot for each payer RX_VK_LAZY_PAYERS asks for, and the submit's own")
    check(made.find("lz_payers.init(payers);") > made.find("while (payers < want"), "lazy_made gives out slots before they are made")
    check("lz_payers.waited()" in function(section, "lazy_report"), "the 600-submit line does not say how often a payment waited for a slot")
    check("`RX_VK_LAZY_PAYERS" in (ROOT / "docs/development.md").read_text(encoding="utf-8"),
          "docs/development.md's switches table has no RX_VK_LAZY_PAYERS")
    veh = section[section.index("LONG CALLBACK lazy_veh"):]
    check(section[:section.index("LONG CALLBACK lazy_veh")].rstrip().endswith("#ifdef _WIN32") or
          "#ifdef _WIN32\n/// The process's first vectored handler" in section, "lazy_veh is not Windows-only")
    decide = function(section, "lazy_decide")
    check("#ifdef _WIN32\n    if (ch.mode) AddVectoredExceptionHandler(1, lazy_veh);" in decide,
          "the fault handler is not put first, on Windows only, once it is on")
    check(mem.count("AddVectoredExceptionHandler(") == 1, "another vectored handler in vk_memory.cpp")
    check("wb_lazy::choose(" in decide and 'menu_row_at_start("RX_VK_LAZY_WB", "lazy_write-back", true, 5)' in decide,
          "lazy_decide is not the row (lazy_write-back=) and RX_VK_LAZY_WB through wb_lazy::choose, on by default since "
          "options_version 5")
    check("lazy_decide();" in function(mem, "memory_init"), "memory_init does not decide")

    # ---- the queue
    sub = function(replay, "submit_recording")
    lock = sub.find("std::lock_guard lk(dev.present_mu);")
    calls = [m.start() for m in re.finditer(r"memory_lazy_submitted\(\);", sub)]
    check(lock >= 0 and len(calls) == 2 and all(c > lock for c in calls), "submit_recording does not tell the lazy write-back of "
          "its recording in both ways (batched and not), under dev.present_mu")
    batch = sub[sub.find("if (batch_on()) {"):]
    check(batch.find("memory_lazy_submitted();") < batch.find("return;"), "the batched submit returns before it tells")

    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    opts = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    row = re.search(r'\{TAB_DEBUG, "Lazy write-back", &opts\.lazy_write_back, 0, 1, 1, tx_onoff,(.*?)\},\n', menu, re.S)
    check(row and row.group(1).rstrip().endswith("hn_lazy_write_back, true, true"),
          "no Lazy write-back row on the Debug tab that applies after a restart and leaves the preset alone")
    if row:
        check(menu.index('"Developer", nullptr') < row.start() < menu.index('constexpr int kRowsEnd'),   # ((Triforce, 11-19) the table's end)
              "the Lazy write-back row is not under the Debug tab's Developer heading")
    check("int lazy_write_back = 1;" in opts, "the Lazy write-back row is not on by default (options_version 5)")
    hint = re.search(r"MENU_HINTS\(hn_lazy_write_back, 0,(.*?)\)\n", menu, re.S)
    check(hint and "AMD" in hint.group(1) and "RX_VK_LAZY_WB=1" in hint.group(1), "the row's hint does not say it stays off on AMD's driver")
    check("#ifndef _WIN32   // (Triforce: the lazy write-back's fault handler pays on Windows only, vk_memory.cpp)\n"
          "           rows[i].value != &opts.lazy_write_back &&" in menu, "the row is shown outside Windows")
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check("| Lazy write-back |" in readme, "readme.md's settings table has no Lazy write-back row")
    check("`RX_VK_LAZY_WB" in dev, "docs/development.md's switches table has no RX_VK_LAZY_WB")

    if problems:
        print("\n".join("FAIL: " + p for p in problems))
        sys.exit(1)
    print("PASS: the lazy write-back's hooks: off is off, every import read and give-up settles, the payment takes no renderer "
          "lock and logs nothing, the queue tells it of every submit")


if __name__ == "__main__":
    main()
