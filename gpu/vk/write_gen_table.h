// (Triforce) The page write generations' tables and their questions (vk_memory.cpp "page write generations"), in a header
// of their own so that a test runs the same code the renderer does (tests/test_write_gen.py: a generation past 2^32 still
// orders after one before it, which a 32-bit counter does not do).
//
// Two levels over the guest's 512 GB (2^27 pages of 4 KB): chunks of 64K page generations made when a page of the
// chunk is first stamped, and the newest generation of each 2 MB region (512 pages), which lets a check skip a big
// texture's clean regions without looking at their pages. 0: never written.
//
// What a generation of 8 bytes costs against one of 4: a chunk is 512 KB where it was 256 KB (one per 256 MB of guest
// address space with a stamped page), the two region tables 2 MB each where they were 1 MB, and a chunk of the "same
// bytes" stamps (texture storage a capture took unchanged) 1 MB where it was 512 KB.
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "write_gen.h"

namespace mkwvk::write_gen {

constexpr uint64_t kPageShift = 12, kChunkPages = 1u << 16, kRegionPages = 512;
constexpr uint64_t kPages = 1ull << 27;

struct Table {
    std::vector<std::unique_ptr<WriteGen[]>> gen_chunks{kPages / kChunkPages};
    std::vector<WriteGen> region_gen = std::vector<WriteGen>(kPages / kRegionPages);
    // (and the generation every page of a region was written at, at least: a large stamp (a storage buffer a dispatch may
    // write, hundreds of MB) marks whole regions, not 512 pages each; it took dispatches 8-150 ms)
    std::vector<WriteGen> region_full = std::vector<WriteGen>(kPages / kRegionPages);
    // (Triforce) A capture's stamp of a texture storage page that held the bytes of the page's last copy (take_capture): two words a
    // page, the stamp and the generation the page had before the run of such stamps. While the page's generation is still that stamp,
    // a texture asking what was written since its upload is told the generation before (page_tex); any other stamp ends it
    std::vector<std::unique_ptr<WriteGen[]>> same_chunks{kPages / kChunkPages};
    WriteGen now{1};   // (the last generation handed out)
    uint32_t chunks_made = 0, same_made = 0;   // (statistics: chunks of each kind, 8 and 16 bytes a page)

    /// The next write generation
    WriteGen next() { return WriteGen{++now.v}; }

    WriteGen page(uint64_t p) const {
        const auto& c = gen_chunks[p / kChunkPages];
        return std::max(c ? gen_load(c[p % kChunkPages]) : WriteGen{}, gen_load(region_full[p / kRegionPages]));
    }
    void stamp_page(uint64_t p, WriteGen g) {
        if (p >= kPages) return;
        auto& c = gen_chunks[p / kChunkPages];
        if (!c) {
            c.reset(new WriteGen[kChunkPages]);   // (zeroed: WriteGen's own start)
            chunks_made++;
        }
        gen_store(c[p % kChunkPages], g);
        region_gen[p / kRegionPages] = g;
    }
    WriteGen page_tex(uint64_t p) const {
        const WriteGen g = page(p);
        const auto& c = same_chunks[p / kChunkPages];
        if (!c || !g) return g;
        const WriteGen s = gen_load(c[(p % kChunkPages) * 2]);
        return s == g ? gen_load(c[(p % kChunkPages) * 2 + 1]) : g;
    }
    void same_set(uint64_t p, WriteGen stamp, WriteGen before) {
        if (p >= kPages || !stamp) return;
        auto& c = same_chunks[p / kChunkPages];
        if (!c) {
            c.reset(new WriteGen[kChunkPages * 2]);
            same_made++;
        }
        gen_store(c[(p % kChunkPages) * 2 + 1], before);
        gen_store(c[(p % kChunkPages) * 2], stamp);
    }
    /// [guest, guest + size) stamped with g: whole regions as one mark each, the pages at the ends by themselves
    void stamp_range(uint64_t guest, uint64_t size, WriteGen g) {
        if (!size) return;
        const uint64_t p0 = guest >> kPageShift, p1 = std::min((guest + size - 1) >> kPageShift, kPages - 1);
        for (uint64_t p = p0; p <= p1;) {
            if (p % kRegionPages == 0 && p + kRegionPages - 1 <= p1) {   // (a whole region)
                gen_store(region_full[p / kRegionPages], g);
                region_gen[p / kRegionPages] = g;
                p += kRegionPages;
                continue;
            }
            stamp_page(p++, g);
        }
    }
    /// True when a page of [guest, guest + size) was written after generation gen
    bool written_since(uint64_t guest, uint64_t size, WriteGen gen) const {
        if (!size) return false;
        const uint64_t p0 = guest >> kPageShift, p1 = std::min((guest + size - 1) >> kPageShift, kPages - 1);
        for (uint64_t r = p0 / kRegionPages; r <= p1 / kRegionPages; r++) {
            if (region_gen[r] <= gen) continue;
            if (gen_load(region_full[r]) > gen) return true;
            const uint64_t a = std::max(p0, r * kRegionPages), b = std::min(p1, r * kRegionPages + kRegionPages - 1);
            for (uint64_t p = a; p <= b; p++)
                if (page(p) > gen) return true;
        }
        return false;
    }
    /// The page runs of [guest, guest + size) written after gen, as byte ranges from guest (appended to out)
    void written_runs(uint64_t guest, uint64_t size, WriteGen gen, std::vector<std::pair<uint64_t, uint64_t>>& out, bool tex) const {
        if (!size) return;
        const uint64_t p0 = guest >> kPageShift, p1 = std::min((guest + size - 1) >> kPageShift, kPages - 1);
        for (uint64_t r = p0 / kRegionPages; r <= p1 / kRegionPages; r++) {
            if (region_gen[r] <= gen) continue;
            const uint64_t a = std::max(p0, r * kRegionPages), b = std::min(p1, r * kRegionPages + kRegionPages - 1);
            for (uint64_t p = a; p <= b; p++) {
                // (tex, Triforce: a texture's question: a capture's stamp of the same bytes as before is not a write, page_tex)
                if ((tex ? page_tex(p) : page(p)) <= gen) continue;
                const uint64_t lo = std::max(p << kPageShift, guest) - guest;
                const uint64_t hi = std::min((p + 1) << kPageShift, guest + size) - guest;
                if (!out.empty() && out.back().second == lo) out.back().second = hi;
                else out.push_back({lo, hi});
            }
        }
    }
};

} // namespace mkwvk::write_gen
