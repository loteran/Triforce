"""What a mirror's write-back brings down is not copied up into it again (gpu/vk/vk_memory.cpp; no game, no GPU).

NX2navi's wb_no_reup (on by default since 0.8): the GPU's writes were stamped in its page tables at their fence, after
the pool's mirror had last been synced, so every page a mirror wrote back into guest memory looked written since and the
next sync copied it straight back up: about 57 of the 60 MiB copied up a frame outdoors on an RTX 4080 SUPER. Its fix
reads the CPU's stamps alone at a sync.

Triforce has the same by its own design, from before the port: the GPU's writes are stamped while their submission is
recorded (memory_stamp_gpu, from the descriptor build of a written storage buffer, a buffer copy or clear, a copy into a
buffer), and the end of that submission raises synced_gen to the newest generation for every pool synced in it
(memory_end_submission), after those stamps; a written binding of a mirrored pool syncs it first (gpu_buffer_bind), so the
pool is among them. The owner's test18 session synced 2.6 to 3.5 MB a frame into the mirrors in play. This test holds
that in place:

- the source: mirror_sync's question is the page's generation against synced_gen and it pushes the pool onto
  mirror_synced without raising synced_gen itself; memory_end_submission raises it for those pools after its write-backs
  and stamps nothing; gpu_buffer_bind syncs a pool before it queues a write-back of it; the GPU's writes are stamped where
  they are recorded; the lazy write-back's payments and the GPU-write ranges the textures keep stamp nothing;
- vk_memory.cpp's stamp_page, memory_stamp_gpu and memory_stamp, cut out and run on write_gen_table.h with mirror_sync's
  two tests and memory_end_submission's raise: a page the GPU wrote (one page, a whole 2 MB region) is not copied up at
  the next sync, a page the CPU wrote is (by a collection, or by the replay itself), the CPU's write after the GPU's is;
- the teeth: the same with the raise made at the sync instead (before the recording's GPU writes) copies the written-back
  pages up again, and has to fail.

    python -I -B tests/test_wb_no_reup.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"

RAISE = "for (Pool* p : mirror_synced) p->synced_gen = gens.now;"
REGION_SKIP = "if (page % kRegionPages == 0 && region_gen(page / kRegionPages) <= p.synced_gen) {"
PAGE_GEN = "const WriteGen pg = page_gen(page);"
DIRTY = "const bool dirty = pg > p.synced_gen;"


def cut(text, start, end_marker="\n}\n"):
    at = text.index(start)
    return text[at:text.index(end_marker, at) + len(end_marker)]


def fail(what):
    raise SystemExit("FAIL: " + what)


FIXTURE = r'''#include "write_gen_table.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>
namespace mkwvk {
using write_gen::kPageShift, write_gen::kRegionPages;
write_gen::Table gens;
inline WriteGen next_gen() { return gens.next(); }
inline WriteGen page_gen(uint64_t p) { return gens.page(p); }
inline WriteGen region_gen(uint64_t r) { return gens.region_gen[r]; }
thread_local uintptr_t tl_stamp_who;
void stamp_src_note(WriteGen, uintptr_t) {}
void memory_drop_shadow(uint64_t, uint64_t) {}
void cap_view_bump() {}
void logf(const char*, ...) {}
bool gpu_pages_on() { return false; }
std::mutex gpu_pages_mu;
std::unordered_map<uint32_t, WriteGen> gpu_pages;
void memory_stamp(uint64_t guest, uint64_t size);
@CPU_GENS@
@STAMP_GPU@
@STAMP@
struct Pool { uint64_t lo, hi; WriteGen synced_gen; };
std::vector<Pool*> mirror_synced;
/// The pages a mirror sync of p copies up from guest memory: mirror_sync's two tests, as they are there (the Python holds
/// them to the source), and its note of the pool
std::vector<uint64_t> sync(Pool& p) {
    mirror_synced.push_back(&p);
    std::vector<uint64_t> out;
    for (uint64_t page = p.lo >> kPageShift; page < (p.hi + 4095) >> kPageShift;) {
        @REGION_SKIP@
            page += kRegionPages;
            continue;
        }
        @PAGE_GEN@
        @DIRTY@
        if (dirty) out.push_back(page);
        page++;
    }
    @AT_SYNC@
    return out;
}
/// The submission's end, after its write-backs
void end_submission() {
    @RAISE@
    mirror_synced.clear();
}
}  // namespace mkwvk
using namespace mkwvk;
int failures = 0;
void check(const std::vector<uint64_t>& got, const std::vector<uint64_t>& want, const char* what) {
    if (got == want) return;
    std::printf("FAIL: %s (%zu pages copied up:", what, got.size());
    for (size_t i = 0; i < got.size() && i < 6; i++) std::printf(" %#llx", (unsigned long long)got[i]);
    std::printf("%s)\n", got.size() > 6 ? " ..." : "");
    failures++;
}
int main() {
    constexpr uint64_t kLo = 0x40000000, kHi = kLo + (8ull << 20);   // (a pool of four 2 MB regions)
    constexpr uint64_t A = kLo / 4096 + 3, B = A + 10, C = A + 20, R = (kLo + (4ull << 20)) / 4096;
    Pool p{kLo, kHi, WriteGen{}};
    stamp_page(A, next_gen());                    // (the CPU wrote A: a collection's stamp)
    check(sync(p), {A}, "the first sync copies up the page the CPU wrote");
    memory_stamp_gpu(B << 12, 4096);               // (the recording's shaders write B and the whole region at R, stamped as
    memory_stamp_gpu(R << 12, 2u << 20);           // they are recorded; the write-back copies them down at its end)
    end_submission();
    stamp_page(C, next_gen());                    // (the CPU wrote C since)
    check(sync(p), {C}, "the next sync copies up the CPU's page, and not the pages the write-back brought down (B, the region)");
    memory_stamp_gpu(B << 12, 4096);               // (the GPU writes B again; after that recording the CPU writes it too)
    end_submission();
    stamp_page(B, next_gen());
    check(sync(p), {B}, "a page the CPU wrote after the GPU did is copied up: the mirror takes the CPU's write");
    end_submission();
    memory_stamp(C << 12, 4096);                   // (the replay itself writes C on the CPU: the same as the game's write)
    check(sync(p), {C}, "a page the replay wrote on the CPU is copied up");
    end_submission();
    check(sync(p), {}, "nothing written since the last sync: nothing copied up");
    end_submission();
    if (failures) return std::printf("%d check(s) failed\n", failures), 1;
    std::printf("PASS: a mirror's write-back is not copied up again; the CPU's writes are\n");
    return 0;
}
'''


def build(cxx, tmp, memory, name, at_end=True):
    src = FIXTURE
    src = src.replace("@CPU_GENS@", cut(memory, "write_gen::Table* cpu_gens = nullptr;"))
    src = src.replace("@STAMP_GPU@", cut(memory, "void memory_stamp_gpu(uint64_t guest, uint64_t size) {"))
    src = src.replace("@STAMP@", cut(memory, "void memory_stamp(uint64_t guest, uint64_t size) {"))
    src = src.replace("@REGION_SKIP@", REGION_SKIP).replace("@PAGE_GEN@", PAGE_GEN).replace("@DIRTY@", DIRTY)
    # (the raise where the renderer has it, at the submission's end; or, for the teeth, at the sync)
    src = src.replace("@RAISE@", RAISE if at_end else "").replace("@AT_SYNC@", "" if at_end else "p.synced_gen = gens.now;")
    cpp = Path(tmp) / (name + ".cpp")
    cpp.write_text(src, encoding="utf-8")
    exe = Path(tmp) / (name + (".exe" if os.name == "nt" else ""))
    subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function",
                    "-Wno-unused-variable", "-D_CRT_SECURE_NO_WARNINGS", "-I", str(VK), str(cpp), "-o", str(exe)], check=True)
    return exe


def check_source(memory):
    sync = cut(memory, "void mirror_sync(Pool& p) {")
    for needle in ("mirror_synced.push_back(&p);", REGION_SKIP, PAGE_GEN, DIRTY):
        if needle not in sync:
            fail("mirror_sync no longer has: " + needle)
    if "synced_gen =" in sync:
        fail("mirror_sync raises synced_gen itself: the GPU's writes recorded after it would be copied up again")
    end = cut(memory, "void memory_end_submission(VkCommandBuffer cmd) {")
    if end.count(RAISE) != 1 or end.index(RAISE) < end.rindex("writebacks.clear();"):
        fail("memory_end_submission does not raise synced_gen for the pools synced in it, after its write-backs")
    for name, body in (("memory_end_submission", end), ("lazy_pay", cut(memory, "void lazy_pay(")),
                       ("lazy_copy", cut(memory, "bool lazy_copy(")), ("memory_lazy_submitted", cut(memory, "void memory_lazy_submitted() {"))):
        if "memory_stamp(" in body or "memory_stamp_gpu(" in body or "stamp_page(" in body:
            fail(name + " stamps what the GPU wrote back: the next sync would copy it up again")
    bind = cut(memory, "BufferRef gpu_buffer_bind(uint64_t gpu, uint64_t size, bool written) {")
    sync_at, wb_at = bind.find("mirror_sync(*p);"), bind.find("writebacks.push_back(")
    if sync_at < 0 or wb_at < 0 or sync_at > wb_at:
        fail("gpu_buffer_bind does not sync a pool before it queues a write-back of it")
    texture = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
    if "memory_stamp" in cut(texture, "void texture_gpu_wrote(uint64_t gpu, uint64_t size) {"):
        fail("texture_gpu_wrote stamps the page tables: a GPU write noted there would be copied up again")
    for path, needle in (("vk_draw.cpp", "if (guest) { ParLock lk; memory_stamp_gpu(guest, n); }"),
                         ("vk_replay.cpp", "if (guest) memory_stamp_gpu(guest, c.size);"),
                         ("vk_xfer.cpp", "if (guest) memory_stamp_gpu(guest, size);")):
        if needle not in (VK / path).read_text(encoding="utf-8"):
            fail("gpu/vk/%s no longer stamps the GPU's write where it is recorded: %s" % (path, needle))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    memory = (VK / "vk_memory.cpp").read_text(encoding="utf-8")
    check_source(memory)
    with tempfile.TemporaryDirectory(prefix="wb-no-reup-") as tmp:
        subprocess.run([str(build(args.cxx, tmp, memory, "fixture"))], check=True, timeout=60)
        r = subprocess.run([str(build(args.cxx, tmp, memory, "at-sync", at_end=False))], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=60)
        if r.returncode == 0 or "not the pages the write-back brought down" not in r.stdout:
            fail("with synced_gen raised at the sync, before the recording's GPU writes, this test still passes:\n" + r.stdout)
        print("with synced_gen raised at the sync instead, the written-back pages are copied up again, as they must be")


if __name__ == "__main__":
    main()
