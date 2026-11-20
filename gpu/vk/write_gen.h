// (Triforce) A page write generation (vk_memory.cpp "page write generations"): the number a write to guest memory gets,
// which every "written since?" compares with the number kept from the last upload or sync.
//
// They were 32 bits. A frame makes a few dozen by vk_memory.cpp's own count (the write watch's collection at each
// submission, the GPU's stamps, the captures), which is weeks of play at 60 frames a second and days at 240; NX2navi,
// whose counter was of the same kind, measured 15 to 66 hours. Past 2^32 the counter started again at 0: a stamp made
// before compared newer than every later check and one made after it older, so textures and mirrors kept old contents
// until the next start. At 64 bits nothing runs out (2^64 at a thousand a frame and 240 frames a second is two million
// years). Below 2^32 every value and every compare is what it was.
//
// A type of its own and not uint64_t, so that the compiler refuses a generation put into 32 bits (`uint32_t g =
// memory_generation()` no longer builds) and a plain number taken for one: the ways a place could have been missed when
// they were widened, and the ways a later change could narrow one again. What it does not refuse is a generation handed
// to the log: print its .v with %llu (logf's arguments are not checked against its format).
// 0: never written. The tables that hold them are in write_gen_table.h (tests/test_write_gen.py).
#pragma once

#include <atomic>
#include <compare>
#include <cstdint>

namespace mkwvk {

struct WriteGen {
    uint64_t v = 0;
    constexpr WriteGen() = default;
    constexpr explicit WriteGen(uint64_t x) : v(x) {}
    constexpr explicit operator bool() const { return v != 0; }   // (ever written)
    friend constexpr auto operator<=>(WriteGen, WriteGen) = default;
};
static_assert(sizeof(WriteGen) == 8 && alignof(WriteGen) == 8);
// (the tables' slots are read on one thread while another stamps them: a load or a store of one is one instruction)
static_assert(std::atomic<uint64_t>::is_always_lock_free);

inline WriteGen gen_load(const WriteGen& x) { return WriteGen{__atomic_load_n(&x.v, __ATOMIC_ACQUIRE)}; }
inline void gen_store(WriteGen& x, WriteGen g) { __atomic_store_n(&x.v, g.v, __ATOMIC_RELEASE); }

} // namespace mkwvk
