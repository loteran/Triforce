// gpu/vk/texture_gone.h: which textures were destroyed, by their creation number. Run by tests/test_texture_gone.py.
#include <cstdio>
#include <cstdlib>

#include "../gpu/vk/texture_gone.h"

static void require(bool ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        exit(1);
    }
}

int main() {
    using texture_gone::Record;
    {
        Record r;
        require(r.count() == 0, "nothing went yet");
        // (the upscale's output, texture 7, noted while it is there)
        const uint64_t noted = r.count();
        require(!r.gone_since(7, noted), "a texture noted is there while nothing went");
        // (the cache lets other textures go, as in most frames while the video memory is full: the generation changed for
        // each, and each cost the frame its picture at the output's size)
        for (uint32_t id = 100; id < 140; id++) r.went(id);
        require(r.count() == 40, "the count is the textures that went");
        require(!r.gone_since(7, noted), "other textures' ends do not make a noted texture gone");
        require(r.gone_since(120, noted), "a texture that went since is gone");
        // (noted again later: what went before does not count, a pointer's address reused by a new texture's number)
        const uint64_t later = r.count();
        require(!r.gone_since(120, later), "what went before the note is not asked about");
        r.went(7);
        require(r.gone_since(7, noted) && r.gone_since(7, later), "the noted texture's own end is seen");
        require(!r.gone_since(8, later), "and it is no other texture's");
    }
    {
        // (no number: a texture that was never numbered cannot be told from one that went)
        Record r;
        require(r.gone_since(0, r.count()), "a texture without a number counts as gone");
        // (a count this record never gave)
        require(r.gone_since(7, 5), "a count from ahead of the record counts as gone");
    }
    {
        // (more went since than are kept, an area's change: it cannot be told, and it counts as gone, the old rule's answer)
        Record r;
        const uint64_t noted = r.count();
        for (uint32_t i = 0; i < Record::kKept; i++) r.went(1000 + i);
        require(!r.gone_since(7, noted), "as many as are kept: still told");
        require(r.gone_since(1000, noted), "the oldest kept is still seen");
        r.went(5000);
        require(r.gone_since(7, noted), "one more than are kept: not told, gone");
        // (and a note taken since is told again)
        const uint64_t later = r.count();
        r.went(5001);
        require(!r.gone_since(7, later) && r.gone_since(5001, later), "a later note is told from what is kept");
    }
    printf("PASS: a kept texture is gone only when it was destroyed itself, or when it cannot be told\n");
    return 0;
}
