// (Triforce) A shader stage's SPIR-V kept packed while no pipeline is being made from it, and the shader module made from
// it again for each pipeline's creation (vk_shader.cpp StageModule; on unless RX_VK_SPV_PACK=0). Apart from Vulkan so that
// tests/test_spv_store.py runs it alone. The idea is NX2navi's (runtime/spv_store.h, "SPIR-V at rest": 777 to 229 MiB
// there); the code is written for this back end.
//
// Why: this back end keeps no SPIR-V of its own in play, the driver does. Every stage of every program variant in memory
// has its VkShaderModule from the moment it is read from triforce_programs.bin or translated until the game ends, and a
// module holds the SPIR-V it was made from (and what the driver adds). The SPIR-V here is large: measured over the kept
// variants of an install on the lab's PC (54,290 records, 6.8 GiB of SPIR-V; a sample of every 25th), 128 KiB a variant
// on average (graphics 124, compute 98, mesh 459). A module is needed only while a pipeline is created with it: after
// that the pipeline has what it needs, and Vulkan lets the module go.
//
// So a stage's SPIR-V is packed when its program becomes ready, its module destroyed, and a module made again (unpack,
// vkCreateShaderModule) for the time one pipeline's creation takes: pin, the creation, unpin. What is held between two
// pipelines of a program is the packed copy, a little over a third of the SPIR-V.
//
// The packing is this file's own: the repository carries zstd's decoder only (third_party/zstd: lib/common and
// lib/decompress), so there is no zstd encoder to link. The four bytes of the words are stored apart (all the low bytes,
// then the next ones, and so on: SPIR-V's words are small numbers, so three of the four runs are mostly zeros), then an
// LZ77 coding of that in the layout of an LZ4 block: a token (literal count, match length less four, a nibble each, 15
// meaning more bytes follow), the literals, a two-byte distance back, and literals alone to end with. Measured on 2,000
// stages of that install (136 MiB of SPIR-V, every one unpacked again and compared): 0.37 of its size, where zstd at
// level 3 gives 0.27; 0.14 ms to pack a stage of 70 KiB and 0.06 ms to unpack it, on one thread.
//
// The rules that make it safe. The test runs the packing by itself and the second and third rule; the first is a second
// guard behind the packing, and no test can make it answer no while pack and unpack are right:
//  - a store is filled only once its packed copy has been unpacked again and found the same, word for word: a stage whose
//    copy does not come back keeps its module as it is and is never packed;
//  - unpack writes no byte outside its block and reads none outside the packed copy, whatever the bytes are, and says no
//    to anything that does not give exactly as many words as the store was filled with; and a hash of the words, taken
//    when the store was filled, is asked again of every copy unpacked for a module: memory gone bad makes no module,
//    never a module of other code;
//  - the module is made and let go under the store's lock, and counted by its pins: two builders of one program's
//    pipelines get the one module, and it is destroyed when the last of them is done, never under the other.
#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace spv_store {

namespace detail {
inline constexpr size_t kMinMatch = 4, kHashBits = 15, kMaxDistance = 65535;
inline uint32_t read32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
inline void put_count(std::vector<uint8_t>& out, size_t n) {   // (what a nibble of 15 leaves over)
    for (; n >= 255; n -= 255) out.push_back(255);
    out.push_back((uint8_t)n);
}
/// src as LZ77 sequences, appended to out
inline void lz_pack(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    std::vector<uint32_t> table((size_t)1 << kHashBits, 0);   // (position + 1 of the last four bytes with this hash; 0: none)
    size_t at = 0, lit = 0;   // (lit: where the literals not written yet begin)
    const auto emit = [&](size_t match_at, size_t match_len) {
        const size_t nlit = at - lit, m = match_len ? match_len - kMinMatch : 0;
        out.push_back((uint8_t)((nlit < 15 ? nlit : 15) << 4 | (m < 15 ? m : 15)));
        if (nlit >= 15) put_count(out, nlit - 15);
        out.insert(out.end(), src + lit, src + at);
        if (!match_len) return;
        const size_t d = at - match_at;
        out.push_back((uint8_t)d), out.push_back((uint8_t)(d >> 8));
        if (m >= 15) put_count(out, m - 15);
    };
    while (at + kMinMatch <= n) {
        const uint32_t h = (read32(src + at) * 2654435761u) >> (32 - kHashBits);
        const size_t was = table[h];
        table[h] = (uint32_t)(at + 1);
        if (was && at + 1 - was <= kMaxDistance && read32(src + was - 1) == read32(src + at)) {
            const size_t from = was - 1;
            size_t len = kMinMatch;
            while (at + len < n && src[from + len] == src[at + len]) len++;
            emit(from, len);
            at += len, lit = at;
        } else {
            at++;
        }
    }
    at = n;
    emit(0, 0);   // (the literals left, alone: every packed copy ends with such a sequence, an empty one too)
}
/// The sequences of z into out, which must come out exactly n bytes long. False: z is not what lz_pack wrote for n bytes
inline bool lz_unpack(const uint8_t* z, size_t zn, uint8_t* out, size_t n) {
    size_t in = 0, at = 0;
    const auto count = [&](size_t* v) {   // (the bytes after a nibble of 15)
        for (;;) {
            if (in >= zn) return false;
            const uint8_t b = z[in++];
            *v += b;
            if (*v > n) return false;   // (no count in a copy of n bytes is past n: and no sum of them wraps)
            if (b != 255) return true;
        }
    };
    for (;;) {
        if (in >= zn) return false;
        const uint8_t token = z[in++];
        size_t nlit = token >> 4, m = token & 15;
        if (nlit == 15 && !count(&nlit)) return false;
        if (nlit > zn - in || nlit > n - at) return false;
        std::memcpy(out + at, z + in, nlit);
        in += nlit, at += nlit;
        if (in == zn) return at == n && !m;   // (the last sequence: literals alone)
        if (zn - in < 2) return false;
        const size_t d = (size_t)z[in] | (size_t)z[in + 1] << 8;
        in += 2;
        if (m == 15 && !count(&m)) return false;
        m += kMinMatch;
        if (!d || d > at || m > n - at) return false;
        for (size_t i = 0; i < m; i++) out[at + i] = out[at + i - d];   // (byte by byte: a match may run into itself)
        at += m;
    }
}
}   // namespace detail

/// The words packed into out (emptied first)
inline void pack(const uint32_t* w, size_t words, std::vector<uint8_t>& out) {
    out.clear();
    std::vector<uint8_t> planes(words * 4);
    const uint8_t* b = (const uint8_t*)w;
    for (size_t k = 0; k < 4; k++)
        for (size_t i = 0; i < words; i++) planes[k * words + i] = b[i * 4 + k];
    detail::lz_pack(planes.data(), planes.size(), out);
}

/// What pack made of that many words, back into out (resized to them). False, and out empty: z is not such a copy
inline bool unpack(const uint8_t* z, size_t zn, size_t words, std::vector<uint32_t>& out) {
    out.clear();
    if (words > SIZE_MAX / 8) return false;
    std::vector<uint8_t> planes(words * 4);
    if (!detail::lz_unpack(z, zn, planes.data(), planes.size())) return false;
    out.resize(words);
    uint8_t* b = (uint8_t*)out.data();
    for (size_t k = 0; k < 4; k++)
        for (size_t i = 0; i < words; i++) b[i * 4 + k] = planes[k * words + i];
    return true;
}

/// A hash of the words (FNV-1a's steps, a word at a time)
inline uint64_t hash(const uint32_t* w, size_t words) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < words; i++) h = (h ^ w[i]) * 1099511628211ull;
    return h;
}

/// One stage's SPIR-V, packed, and the module made from it while a pipeline is being created. Module: the back end's
/// VkShaderModule (a stand-in in the test); a value-initialised one is "none"
template <class Module>
class Code {
public:
    Code() = default;
    Code(const Code&) = delete;
    Code& operator=(const Code&) = delete;

    /// Takes the words. True: packed, and the packed copy gave them back when unpacked; false: nothing is kept (the caller
    /// goes on with the module it has)
    bool fill(const uint32_t* w, size_t words) {
        std::vector<uint8_t> z;
        std::vector<uint32_t> back;
        pack(w, words, z);
        if (!words || !unpack(z.data(), z.size(), words, back) || std::memcmp(back.data(), w, words * 4)) return false;
        z.shrink_to_fit();
        std::lock_guard lk(mu_);
        z_ = std::move(z), words_ = words, hash_ = hash(w, words);
        return true;
    }
    bool filled() const { return words_ != 0; }
    size_t words() const { return words_; }
    size_t packed_bytes() const { return z_.size(); }

    /// The module, made by make(const uint32_t* words, size_t n) if no pin holds one; kept until as many unpins as pins.
    /// "None" when the store is not filled, the copy does not unpack to the words it was filled with (it was checked then:
    /// memory gone bad) or make gives none: then nothing is pinned and no unpin is owed
    template <class Make>
    Module pin(Make&& make) {
        std::lock_guard lk(mu_);
        if (pins_) return pins_++, live_;
        std::vector<uint32_t> w;
        if (!words_ || !unpack(z_.data(), z_.size(), words_, w) || hash(w.data(), w.size()) != hash_) return Module{};
        const Module m = make((const uint32_t*)w.data(), w.size());
        if (m == Module{}) return m;
        live_ = m, pins_ = 1, made_++;
        return m;
    }
    /// The end of a pin that gave a module: the last one's has drop(module) called
    template <class Drop>
    void unpin(Drop&& drop) {
        std::lock_guard lk(mu_);
        if (!pins_ || --pins_) return;
        const Module m = live_;
        live_ = Module{};
        drop(m);
    }
    uint32_t pins() const {
        std::lock_guard lk(mu_);
        return pins_;
    }
    uint64_t made() const {   // (the modules made so far: one a pipeline, or fewer where builders overlapped)
        std::lock_guard lk(mu_);
        return made_;
    }

private:
    mutable std::mutex mu_;
    std::vector<uint8_t> z_;   // (set once, by fill, before any pin)
    size_t words_ = 0;
    uint64_t hash_ = 0;
    Module live_{};
    uint32_t pins_ = 0;
    uint64_t made_ = 0;
};

}   // namespace spv_store
