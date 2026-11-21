/* (Triforce) The program cache carried over an update of the translator (vk_shader.cpp disk_stamp and disk_init).

   triforce_programs.bin keeps what translation made, a record a program variant, under a stamp: what translated it and for
   what. The stamp named every file of the translator whole (nx2/build.py's HOOKSHOT_SHADER_STAMP: Eden, sirit, vk_shader.cpp,
   vk_bank.cpp, vk_state.h, vk_api.h), so any edit of one threw every program away: a log line, the read-ahead, a host option's
   number in vk_api.h (14 of the 17 commits that changed that stamp from v0.4.0 to a464979 were vk_api.h's alone, and none of
   the 17 changed a translation). The owner's test7 and test8 started with "0 program keys kept" and prepared some 46,600
   programs before the title (75.2 s in test7), and the pipeline cache, the pipeline notes and the prepare's records, which go
   by the same stamp, went with them.

   The rule. A record is used by a build of the translator other than the one that made it only when the two have the same
   translation stamp (nx2/build.py translation_stamp, HOOKSHOT_TRANSLATION_STAMP): the same files, each taken without what
   cannot change a translation, and nothing else left out:
   - vk_api.h, the interface between the game and the renderer: no translation reads a constant, a macro or a type of its own
     (and it includes rx_nvn.h before anything of its own). What a translation asks of the host goes through host->, the
     guest's memory and the depth mode, and the answers are the host's whatever vk_api.h says (the depth mode is in the
     stamp's run-time part below).
   - The parts of vk_shader.cpp between an "Outside the translation stamp" mark and the next "Inside the translation stamp
     again": the shader modules made of a translation's SPIR-V (made the same for a record read back as for a program just
     translated, from SPIR-V they take const), the program cache's file and its list of records, the read-ahead and the
     precompile's loads (where the records are and when they are read, never what one holds: program_cache_read, in the stamp,
     reads each and checks its key). What a record holds, how it is written and read, the keys and the answers a record is
     picked by all stay in the stamp.
   - The parts of vk_bank.cpp so marked: code no translation can reach (sampler_bias_changed, which only vk_draw.cpp's
     sampler_for reads by). What a translation calls there, what those read and what writes that (texture_of's registrations:
     register_texture, texture_released) stay in the stamp; tests/test_program_carry.py bank_check holds it.
   - Comments, blank lines and the spaces at the ends of lines, in the C and C++ files.
   The run-time part of the stamp is as before (disk_stamp: the depth mode, the GPU's mesh shaders and warps, the per-GPU
   translation settings, the C++ library's layouts, the RX_VK_ and EDEN_ switches that change a translation). So a record is
   never used by a build that would have made it otherwise, as far as the stamp of the whole translator could ever tell (what
   it never covered, the host's side, the other gpu/vk headers, the compiler, this does not cover either), and nothing is
   guessed at run time: the two stamps are the same, or every record is translated again. tests/test_program_carry.py holds
   the rule: which edits change the stamp and which do not, that every translator file is the same C++ to clang's lexer as
   the stamp takes it, that the marked parts touch nothing of a translation and give the rest only what it may take from
   them, that vk_api.h is not read, and disk_init's verdicts on files it authors.

   Then what vk_draw.cpp keeps by program_cache_stamp() (the pipeline cache, mkw_vk_pipelines.bin, by its .for; the pipeline
   notes; the prepare's record, triforce_prepared.bin, and the driver's part of it; the predicted pipelines' list) is kept with
   the programs, as at a start of the same build: no preparation before the title. The log's "program cache" line at the start
   says whether the programs were kept, carried over the translator's update (and from which build), or translated again (and
   how many the cache had); while programs of a cache thrown away are translated again, its lines count them.
   RX_VK_PROGRAM_CARRY=0: the old rule, the stamp of the whole translator (a cache is good for one build of it). Plain
   functions: tests/test_program_carry.py compiles them. */
#pragma once

#include <cstdint>
#include <cstdlib>

namespace mkwvk::program_carry {

/// RX_VK_PROGRAM_CARRY: on unless set to 0
inline bool on(const char* env) {
    return !env || std::atoi(env) != 0;
}

/// The build of the translator, by its whole stamp (HOOKSHOT_SHADER_STAMP): the cache file's second word names the build that
/// last opened it (0 in files of before this rule)
inline uint32_t writer(uint64_t whole) {
    const uint32_t w = (uint32_t)(whole ^ (whole >> 32));
    return w ? w : 1;
}

enum class Verdict {
    Fresh,     // (no cache there, or not one of ours: a new one)
    Kept,      // (made for this stamp, and last opened by this build of the translator)
    Carried,   // (made for this stamp by another build of the translator: its translation is this one's)
    Dropped,   // (made for another stamp: every program is translated again)
};

/// A cache file by its head (read: whether its 16 bytes were there; magic, writer and stamp as they are in it), for this run's
/// stamp and build. Only a file made for this very stamp is used
inline Verdict verdict(bool read, uint32_t magic, uint32_t file_writer, uint64_t file_stamp, uint32_t want_magic, uint64_t stamp,
                       uint32_t writer) {
    if (!read || magic != want_magic) return Verdict::Fresh;
    if (file_stamp != stamp) return Verdict::Dropped;
    return file_writer == writer ? Verdict::Kept : Verdict::Carried;
}

/// What the stamp is made from for the translator, two words: with the carry the translation stamp, without it (or in a build
/// that has no translation stamp, 0) the whole translator's, and which of the two (a cache of one rule is never one of the other)
inline void translator_words(bool carry, uint64_t translation, uint64_t whole, uint64_t out[2]) {
    const bool by_translation = carry && translation != 0;
    out[0] = by_translation ? translation : whole;
    out[1] = by_translation ? 2u : 1u;
}

} // namespace mkwvk::program_carry
