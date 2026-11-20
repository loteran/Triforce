"""The lazy write-back's page states (gpu/vk/wb_lazy_state.h; no game, no GPU).

With the Debug tab's Lazy write-back row on, a whole page a shader keeps writing and the CPU leaves alone is no longer
copied back to guest memory at every submission: the copy is owed, the page gets no access, and whoever touches it pays
first. The states and their rules are in a header of their own; tests/test_wb_lazy_state.cpp runs that header against a
model of memory (each page's access, checked and used in one step as the MMU does) and of the GPU's copies:

- rules: one step at a time, with protection compared against state after every step. A page is watched (read-only)
  before anything is owed, and owed only once a collection and a capture begun after the watch have named no CPU write
  to it; an owed run's copy is skipped; later writes join the one debt and one copy pays them, each piece with its
  newest; a read and a write of an owed page both pay first (the write does not drop the debt); a write to a watched
  page ends the watch with no copy; a second toucher of a page being paid waits for that copy and makes none; a payment
  between a recording's end and its submit leaves the rest PENDING until that submit; nothing is protected while a host
  call has memory in the kernel's hands, and its range is settled first; a touch holds its block for a second, doubling;
  giving a mirror up settles everything of it.
- stress: for a few seconds a recorder, three CPU readers and writers and a host-call thread run against one table. A
  reader must never see guest memory older than a submitted write, a GPU read neither, the kernel must never be handed a
  protected page, no page may be copied twice at once or from a mirror that was given up, no payment may put older bytes
  over a CPU write, and a fault on a protected page must always be recognised.
- (Triforce) the payers' slots (gpu/vk/wb_lazy_payers.h, RX_VK_LAZY_PAYERS; NX2navi's wb_lazy_unlocked): several payments
  copy and wait at once, each with a slot of its own. The rules: RX_VK_LAZY_PAYERS read as 1 to 4, 4 when not set; four
  payments hold four different slots and a fifth waits for one given back; one slot is one payment at a time. The stress
  test runs with four slots, where two payments have to have been in flight at once and no slot may be held twice, and
  again with one, the renderer's first way. A copy of the header whose slots are not marked taken has to fail the rules.

    python -I -B tests/test_wb_lazy_state.py [--cxx g++] [--seconds 3]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--seconds", default="3")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="wb-lazy-state-") as tmp:
        exe = Path(tmp) / ("wb-lazy-state.exe" if os.name == "nt" else "wb-lazy-state")
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                        "-I", str(ROOT / "gpu" / "vk"), str(ROOT / "tests/test_wb_lazy_state.cpp"), "-o", str(exe)] +
                       ([] if os.name == "nt" else ["-pthread"]), check=True)
        subprocess.run([str(exe)], check=True, timeout=60)
        subprocess.run([str(exe), "stress", args.seconds], check=True, timeout=120)   # (four payers' slots)
        subprocess.run([str(exe), "stress", "1", "1"], check=True, timeout=120)       # (one: as the renderer had it first)
        # (the stress test's teeth: the same test against a copy of the header that owes a watched page at its next write,
        # without waiting for the captures to catch up, has to fail, and by a CPU write found on an owed page)
        header = (ROOT / "gpu/vk/wb_lazy_state.h").read_text(encoding="utf-8")
        rule = "e.collects > s.collects_at && (!e.captures || e.caps_taken > s.caps_at)"
        if header.count(rule) != 1:
            raise SystemExit("gpu/vk/wb_lazy_state.h no longer has the rule that ends a watch in OWED: " + rule)
        broken = Path(tmp) / "broken"
        broken.mkdir()
        (broken / "wb_lazy_state.h").write_text(header.replace(rule, "true"), encoding="utf-8")
        exe2 = Path(tmp) / ("wb-lazy-broken.exe" if os.name == "nt" else "wb-lazy-broken")
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-D_CRT_SECURE_NO_WARNINGS", "-I", str(broken), "-I", str(ROOT / "gpu" / "vk"),
                        str(ROOT / "tests/test_wb_lazy_state.cpp"), "-o", str(exe2)] + ([] if os.name == "nt" else ["-pthread"]), check=True)
        r = subprocess.run([str(exe2), "stress", args.seconds], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
        if r.returncode == 0 or "CPU write" not in r.stdout:
            raise SystemExit("the stress test passes against a table that owes a page before the CPU's writes to it are known:\n" + r.stdout)
        print("the same stress against a table without the watch's rule fails, as it must")
        # ((Triforce) the payers' slots' teeth: a copy of wb_lazy_payers.h that hands out a slot without marking it taken has to
        # fail the rules (two payments in one slot's command buffer at once))
        payers = (ROOT / "gpu/vk/wb_lazy_payers.h").read_text(encoding="utf-8")
        taken = "free_bits &= ~(1u << i);"
        if payers.count(taken) != 1:
            raise SystemExit("gpu/vk/wb_lazy_payers.h no longer marks a slot taken with: " + taken)
        loose = Path(tmp) / "loose"
        loose.mkdir()
        (loose / "wb_lazy_payers.h").write_text(payers.replace(taken, ""), encoding="utf-8")
        exe3 = Path(tmp) / ("wb-lazy-loose.exe" if os.name == "nt" else "wb-lazy-loose")
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-D_CRT_SECURE_NO_WARNINGS", "-I", str(loose), "-I", str(ROOT / "gpu" / "vk"),
                        str(ROOT / "tests/test_wb_lazy_state.cpp"), "-o", str(exe3)] + ([] if os.name == "nt" else ["-pthread"]), check=True)
        r = subprocess.run([str(exe3)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        if r.returncode == 0 or "FAIL: payers: four payments hold four different slots" not in r.stdout:
            raise SystemExit("the rules pass against payers' slots that are never marked taken:\n" + r.stdout)
        print("the same rules against slots never marked taken fail, as they must")


if __name__ == "__main__":
    main()
