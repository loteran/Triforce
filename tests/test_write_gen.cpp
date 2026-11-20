// The page write generations' tables (gpu/vk/write_gen_table.h, the code vk_memory.cpp runs) on both sides of 2^32, where a
// 32-bit generation started again at 0. tests/test_write_gen.py compiles this twice: against the headers as they are (every
// check passes), and against a copy of them with the generation narrowed to 32 bits (the checks marked WRAP fail, which is
// what says they would have caught the 32-bit counter). No game, no GPU.
#include "write_gen_table.h"

#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

using mkwvk::WriteGen;
using namespace mkwvk::write_gen;

namespace {

int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), failures++;
}
using Runs = std::vector<std::pair<uint64_t, uint64_t>>;
constexpr uint64_t kPage = 4096, kRegion = kRegionPages * kPage;   // (2 MB)

Runs runs_of(const Table& t, uint64_t guest, uint64_t size, WriteGen since, bool tex = false) {
    Runs r;
    t.written_runs(guest, size, since, r, tex);
    return r;
}

/// Below 2^32: what the tables answered before, whatever the generation's width
void small() {
    Table t;
    expect(t.now == WriteGen{1} && !t.page(5) && !t.written_since(0, 1ull << 30, WriteGen{}), "small: a new table has pages never written");
    const WriteGen up = t.now;   // (an upload now)
    const WriteGen g = t.next();
    expect(g == WriteGen{2} && g > up, "small: the next generation is one more");
    t.stamp_page(0x4321, g);
    expect(t.page(0x4321) == g && !t.page(0x4320) && !t.page(0x4322), "small: a stamp is its page's alone");
    expect(t.written_since(0x4321 * kPage, 1, up) && t.written_since(0x4320 * kPage + 4095, 2, up), "small: written since the upload");
    expect(!t.written_since(0x4321 * kPage, kPage, g) && !t.written_since(0x4322 * kPage, kPage, up), "small: not since its own stamp, not the next page");
    expect(!t.written_since(0x4321 * kPage, 0, up), "small: an empty range is not written");
    // (runs: two pages in a row are one run, cut to the range asked about)
    t.stamp_page(0x4322, g);
    t.stamp_page(0x4324, g);
    expect(runs_of(t, 0x4320 * kPage + 0x800, 6 * kPage, up) == Runs{{kPage - 0x800, 3 * kPage - 0x800}, {4 * kPage - 0x800, 5 * kPage - 0x800}},
           "small: the written pages as byte runs from the range's start");
    expect(runs_of(t, 0x4321 * kPage + 16, 32, up) == Runs{{0, 32}}, "small: a run is cut to the range");
    expect(runs_of(t, 0x4320 * kPage, 6 * kPage, g).empty(), "small: no runs since the stamp itself");
    // (a large stamp marks whole 2 MB regions and the pages at its ends)
    const WriteGen big = t.next();
    const uint64_t lo = 7 * kRegion - 3 * kPage, size = 2 * kRegion + 5 * kPage;   // (3 pages, two whole regions, 2 pages)
    t.stamp_range(lo, size, big);
    expect(t.page(7 * kRegionPages - 3) == big && !(t.page(7 * kRegionPages - 4) == big), "small: a large stamp starts at its first page");
    expect(t.page(7 * kRegionPages) == big && t.page(8 * kRegionPages + 100) == big && t.page(9 * kRegionPages - 1) == big, "small: its whole regions' pages");
    expect(t.page(9 * kRegionPages + 1) == big && !t.page(9 * kRegionPages + 2), "small: and ends at its last page");
    expect(t.chunks_made == 1, "small: all of that in one chunk of page generations");
    expect(t.written_since(8 * kRegion + 12345, 1, g) && !t.written_since(8 * kRegion + 12345, 1, big), "small: a region marked whole answers for its pages");
    // (a later stamp of one page in a whole region is newer than the region's mark)
    const WriteGen later = t.next();
    t.stamp_page(8 * kRegionPages + 7, later);
    expect(t.page(8 * kRegionPages + 7) == later && t.page(8 * kRegionPages + 8) == big, "small: a page stamped after its region's mark");
    expect(runs_of(t, 8 * kRegion, kRegion, big) == Runs{{7 * kPage, 8 * kPage}}, "small: and only that page since the mark");
    // (past the window: nothing is stamped and nothing read past the tables)
    t.stamp_page(kPages, later);
    t.stamp_range(kPages * kPage - kPage, 3 * kPage, later);
    expect(t.page(kPages - 1) == later && t.written_since(kPages * kPage - kPage, 1ull << 40, big), "small: the window's last page");
}

/// WRAP: a write past 2^32 is newer than an upload or a sync before it, and a stamp from before is not newer than one after
void across() {
    Table t;
    t.now = WriteGen{0xFFFFFFF0ull};   // (what RX_VK_GEN_SEED does: 16 short of 2^32)
    const uint64_t a = 0x1234 * kPage, b = 0x9000 * kPage, c = 40 * kRegion;
    const WriteGen before = t.next();   // (a write before the end of 32 bits)
    t.stamp_page(a >> kPageShift, before);
    const WriteGen upload = t.now;      // (a texture uploaded then, a mirror synced then)
    WriteGen g = t.now, last = t.now;
    bool rising = true, never_zero = true;
    for (int i = 0; i < 40; i++) {      // (through 2^32)
        g = t.next();
        rising = rising && g > last, never_zero = never_zero && bool(g);
        last = g;
    }
    expect(rising, "WRAP: every generation is newer than the one before it");
    expect(never_zero, "WRAP: no generation is 0, which says never written");
    expect(g > upload && g > before, "WRAP: a generation past 2^32 orders after one before it");
    expect(std::max(upload, g) == g && std::min(upload, g) == upload, "WRAP: the newer of two generations (a texture's upload and the GPU's write)");
    t.stamp_page(b >> kPageShift, g);
    expect(bool(t.page(b >> kPageShift)) && t.page(b >> kPageShift) > upload, "WRAP: the page's stamp is newer than the upload");
    expect(t.written_since(b, kPage, upload), "WRAP: a page written after 2^32 is written since an upload before it");
    expect(runs_of(t, b - kPage, 3 * kPage, upload) == Runs{{kPage, 2 * kPage}}, "WRAP: and is in the runs to upload again");
    // (a mirror synced now, after the wrap: the page stamped before it is in the mirror, not dirty for good)
    const WriteGen synced = t.now;
    expect(!t.written_since(a, kPage, synced), "WRAP: a page written before 2^32 is not written since a sync after it");
    expect(runs_of(t, a, kPage, synced).empty(), "WRAP: and is not copied at every sync");
    expect(!t.written_since(b, kPage, synced), "across: the page of the sync's own generation is in it");
    // (whole regions: the region's newest and its mark)
    const WriteGen big = t.next();
    t.stamp_range(c, 3 * kRegion, big);
    expect(t.written_since(c + kRegion + 5 * kPage, 64, upload), "WRAP: a region marked whole after 2^32 is written since an upload before it");
    expect(runs_of(t, c + kRegion, 2 * kPage, upload) == Runs{{0, 2 * kPage}}, "WRAP: its pages are in the runs");
    expect(!t.written_since(c, 3 * kRegion, big), "across: not since the mark itself");
    // (the textures' question: a capture's stamp of the same bytes is not a write, while that stamp is the page's)
    const uint64_t p = 0x7777;
    Table u;
    u.now = WriteGen{0xFFFFFFFEull};
    const WriteGen first = u.next();        // (0xFFFFFFFF: the page written, the texture uploaded after)
    u.stamp_page(p, first);
    const WriteGen tex_upload = u.now;
    const WriteGen same = u.next();         // (2^32: a capture takes the page with the bytes of its last copy)
    u.same_set(p, same, u.page_tex(p));
    u.stamp_page(p, same);
    expect(u.page(p) == same && u.page_tex(p) == first, "WRAP: a same-bytes stamp past 2^32 tells a texture the generation before it");
    expect(runs_of(u, p * kPage, kPage, tex_upload, true).empty(), "WRAP: so the texture is not uploaded again");
    expect(runs_of(u, p * kPage, kPage, tex_upload, false) == Runs{{0, kPage}}, "WRAP: while a mirror's sync still takes the page");
    const WriteGen again = u.next();        // (the same bytes again: the generation before the run of such stamps stays)
    u.same_set(p, again, u.page_tex(p));
    u.stamp_page(p, again);
    expect(u.page_tex(p) == first && u.same_made == 1, "WRAP: a run of same-bytes stamps keeps the first one's answer");
    const WriteGen other = u.next();        // (any other stamp ends it)
    u.stamp_page(p, other);
    expect(u.page_tex(p) == other && !runs_of(u, p * kPage, kPage, tex_upload, true).empty(), "WRAP: a real write is told to the texture");
}

} // namespace

int main() {
    small();
    across();
    if (failures) return std::printf("%d check(s) failed\n", failures), 1;
    std::printf("PASS: page write generations order across 2^32\n");
    return 0;
}
