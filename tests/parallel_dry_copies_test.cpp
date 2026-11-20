// tests/test_parallel_fixes.py puts vk_xfer.cpp's own dry-pass functions (b2t_on_cpu, dry_copy_b2t, dry_copy_t2t,
// dry_clear_tex) in the place of ACTUAL and runs them against a model of the texture cache and of the queue: a command
// buffer in three segments whose workers record out of order, each segment's uploads run before its commands, the
// segments in order. No game, no GPU.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace fixture {
void require(bool ok, const char* why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", why);
        std::exit(1);
    }
}
constexpr uint32_t VK_IMAGE_ASPECT_COLOR_BIT = 1, VK_IMAGE_ASPECT_DEPTH_BIT = 2, VK_IMAGE_ASPECT_STENCIL_BIT = 4;
using VkImageAspectFlags = uint32_t;
struct nvn_tex { uint64_t gpu = 0; int format = 0; };
struct nvn_view { int level = 0, layer = 0; };
struct NVNcopyRegion { int xoffset = 0, yoffset = 0, zoffset = 0, width = 0, height = 0, depth = 0; };
struct nvn_p_copy_bt { const nvn_tex* tex = nullptr; nvn_view view; int has_view = 0; NVNcopyRegion r; uint64_t buf = 0; };
struct nvn_p_copy_tt {
    const nvn_tex* src = nullptr; nvn_view sv; int has_sv = 0; NVNcopyRegion sr;
    const nvn_tex* dst = nullptr; nvn_view dv; int has_dv = 0; NVNcopyRegion dr;
};
struct nvn_p_clear_tex { const nvn_tex* tex = nullptr; nvn_view view; int has_view = 0; NVNcopyRegion r; };
struct NvnState { int copy_stride = 0; };

// ---- the model: textures, and what each segment's uploads and commands do to them in the queue ----
struct Texture {
    uint64_t gpu = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    int format = 1;
    bool gpu_written = false, released = false, only_cleared = false, bc1 = false, bc = false;
    bool uploaded = false, missing = false;
    int upload_seg = -1;
    std::string content = "nothing";   // (what the image holds once the queue has run)
};
std::map<uint64_t, Texture> textures;
int seg_now = 0;   // (0: the recording before the segments, where the dry pass's uploads go; 1 to 3: a segment's worker)
struct Op { char what; uint64_t gpu; };   // ('u' an upload from the game's memory, 'c' a copy into it, 'z' a clear, 's' a sample)
std::vector<Op> uploads[4], commands[4];
std::vector<std::string> sampled;         // (what each sample saw, in the queue's order)
int demoted = 0, cpu_writes = 0;
std::vector<std::string> marks;

/// texture_touch's rule, as far as it matters here: the GPU's image is the content; else the game's memory, uploaded
/// into this segment's uploads when it never was, or was by a segment after this one
Texture* texture_get(const nvn_tex* t, bool for_sampling) {
    if (!t || !t->gpu) return nullptr;
    Texture& x = textures[t->gpu];
    x.gpu = t->gpu;
    if (x.missing) return nullptr;
    if (!for_sampling || (x.gpu_written && !x.released)) return &x;
    if (!x.uploaded || x.upload_seg > seg_now) uploads[seg_now].push_back({'u', x.gpu}), x.uploaded = true, x.upload_seg = seg_now;
    return &x;
}
void texture_written(Texture* t, const char* why = "?") {
    marks.push_back(why);
    t->only_cleared = false, t->gpu_written = true, t->released = false, t->uploaded = true;
}
bool texture_bc1_to_bc3(Texture* t, const char*) { return t->bc1 = false, t->bc = true, demoted++, true; }
struct Sub {
    Texture* tex = nullptr;
    VkImageAspectFlags aspect = 0;
    uint32_t level = 0, layer = 0;
};
bool resolve(const nvn_tex* t, const nvn_view* v, bool has_v, const NVNcopyRegion&, Sub& s, bool = false) {
    if (!t || !t->gpu) return false;
    s.tex = texture_get(t, true);
    if (!s.tex) return false;
    s.aspect = s.tex->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : s.tex->aspect;
    s.level = has_v ? (uint32_t)v->level : 0, s.layer = has_v ? (uint32_t)v->layer : 0;
    return true;
}
// (the CPU's buffer-to-texture copy: ASTC by the format, a mapped buffer, the bytes into the texture's storage)
struct FormatInfo { bool astc; };
FormatInfo vk_format(int f) { return {f == 0x79}; }
int view_nvn_format(const nvn_tex* t, const nvn_view*) { return t->format; }
int view_level(const nvn_view* v) { return v ? v->level : 0; }
int view_layer(const nvn_view* v) { return v ? v->layer : 0; }
uint8_t buffer_bytes[64];
bool buffer_mapped = true, buffer_fits = true;
struct Host {
    const uint8_t* gpu_ptr(uint64_t, uint64_t* avail) { return *avail = sizeof buffer_bytes, buffer_mapped ? buffer_bytes : nullptr; }
} host_model, *host = &host_model;
bool texels_fit(const nvn_tex*, const NVNcopyRegion*, uint64_t, uint64_t) { return buffer_fits; }
void warn(int, const char*) {}
void texture_read_texels(const nvn_tex* t, unsigned, unsigned, const NVNcopyRegion*, uint8_t*, uint64_t, uint64_t, bool write) {
    require(write, "the CPU's copy writes");
    cpu_writes++;
    Texture& x = textures[t->gpu];
    x.gpu_written = false, x.uploaded = false;   // (its storage holds new bytes: uploaded again at its next look)
}

/* ACTUAL */

// ---- a segment's worker, as vk_xfer.cpp's copies and a draw's sample are for the model ----
void work_copy(uint64_t src, uint64_t dst) {   // (src 0: from a buffer)
    nvn_tex s{src}, d{dst};
    if (src) texture_get(&s, true);
    texture_get(&d, true);
    commands[seg_now].push_back({'c', dst});
    texture_written(&textures[dst], "a texture-to-texture copy");
}
void work_clear(uint64_t dst) {
    nvn_tex d{dst};
    Texture* t = texture_get(&d, true);
    commands[seg_now].push_back({'z', dst});
    const bool fresh = !t->gpu_written || t->released || t->only_cleared;
    texture_written(t);
    t->only_cleared = fresh;
}
void work_sample(uint64_t gpu) {
    nvn_tex t{gpu};
    texture_get(&t, true);
    commands[seg_now].push_back({'s', gpu});
}
void run_queue() {
    for (int seg = 0; seg < 4; seg++) {
        for (const Op& o : uploads[seg]) textures[o.gpu].content = "the game's memory";
        for (const Op& o : commands[seg]) {
            if (o.what == 'c') textures[o.gpu].content = "the copy";
            if (o.what == 'z') textures[o.gpu].content = "the clear";
            if (o.what == 's') sampled.push_back(textures[o.gpu].content);
        }
    }
}
constexpr uint64_t S = 0x1000, D = 0x2000;

int run(int argc, char** argv) {
    require(argc == 2, "a case");
    const std::string test = argv[1];
    nvn_tex ts{S}, td{D};
    if (test == "t2t" || test == "t2t-unmarked" || test == "clear" || test == "clear-unmarked" || test == "b2t" || test == "b2t-unmarked") {
        // the stream: segment 1 writes D (a copy from S, a clear, or a copy from a buffer), segment 3 samples D. The dry
        // pass, then segment 3's worker before segment 1's.
        const bool marked = test.find("unmarked") == std::string::npos;
        const char kind = test[0];
        seg_now = 0;
        if (marked) {
            if (kind == 't') { nvn_p_copy_tt p; p.src = &ts, p.dst = &td; dry_copy_t2t(p); }
            if (kind == 'c') { nvn_p_clear_tex p; p.tex = &td; dry_clear_tex(p); }
            if (kind == 'b') { NvnState st; nvn_p_copy_bt p; p.tex = &td; dry_copy_b2t(st, p); }
        }
        seg_now = 3, work_sample(D);
        seg_now = 1;
        if (kind == 'c') work_clear(D);
        else work_copy(kind == 't' ? S : 0, D);
        run_queue();
        require(sampled.size() == 1, "one sample");
        const char* want = kind == 'c' ? "the clear" : "the copy";
        if (marked) {
            require(sampled[0] == want, "the later segment samples what the earlier segment's copy or clear left");
            require(uploads[3].empty() && uploads[1].empty(), "no segment uploads the destination: it was uploaded before them all");
            require(uploads[0].size() >= 1, "the dry pass brought the destination's contents from the game's memory first");
        } else {
            // (the hole, shown so that the case above means something: the later segment's upload lands over the copy)
            require(sampled[0] == "the game's memory", "without the mark the later segment's upload overwrites the copy");
        }
        if (marked && kind == 'c') require(textures[D].only_cleared, "a texture all the GPU did was clear is noted as only cleared");
    } else if (test == "clear-drawn") {
        // a texture the GPU drew before, cleared: not "only cleared" (what the CPU writes into it does not count)
        textures[D].gpu = D, textures[D].gpu_written = true;
        nvn_p_clear_tex p;
        p.tex = &td;
        dry_clear_tex(p);
        require(marks.size() == 1 && !textures[D].only_cleared && textures[D].gpu_written, "cleared after a draw: the GPU's, not only cleared");
    } else if (test == "skips") {
        // what the copies leave out is not marked: a copy within one subresource, unlike depth formats, a missing texture,
        // a buffer into a depth-stencil texture
        nvn_p_copy_tt same;
        same.src = &td, same.dst = &td;
        dry_copy_t2t(same);
        require(marks.empty(), "a copy within one subresource marks nothing");
        textures[S].aspect = VK_IMAGE_ASPECT_DEPTH_BIT, textures[S].format = 2;
        textures[D].aspect = VK_IMAGE_ASPECT_DEPTH_BIT, textures[D].format = 3;
        nvn_p_copy_tt unlike;
        unlike.src = &ts, unlike.dst = &td;
        dry_copy_t2t(unlike);
        require(marks.empty(), "a copy between unlike depth formats marks nothing");
        textures[D].format = 2;
        dry_copy_t2t(unlike);
        require(marks.size() == 1, "a copy between like depth formats marks its destination");
        marks.clear();
        textures[D].missing = true;
        dry_copy_t2t(unlike);
        nvn_p_clear_tex gone;
        gone.tex = &td;
        dry_clear_tex(gone);
        require(marks.empty(), "a texture with no image marks nothing");
        textures[D].missing = false, textures[D].aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        NvnState st;
        nvn_p_copy_bt b;
        b.tex = &td;
        dry_copy_b2t(st, b);
        require(marks.empty(), "a buffer into a depth-stencil texture marks nothing");
        nvn_p_copy_bt none;
        dry_copy_b2t(st, none);
        nvn_p_copy_tt null;
        dry_copy_t2t(null);
        require(marks.empty(), "no texture: nothing");
    } else if (test == "bc1") {
        // a destination kept as BC1 becomes BC3 in the dry pass, before it is marked; a BC1 source of a copy into a BC3 one too
        textures[D].bc1 = textures[D].bc = true;
        nvn_p_copy_tt p;
        p.src = &ts, p.dst = &td;
        dry_copy_t2t(p);
        require(demoted == 1 && !textures[D].bc1 && marks.size() == 1, "a BC1 destination is BC3 before the mark");
        textures[S].bc1 = textures[S].bc = true;
        dry_copy_t2t(p);
        require(demoted == 2 && !textures[S].bc1, "a BC1 source of a copy into a BC3 texture is BC3 too");
    } else if (test == "b2t-cpu") {
        // ASTC blocks from a buffer: the CPU's copy, made in the dry pass once and not marked as the GPU's; the worker's
        // call (write false) makes nothing and still says the copy was the CPU's
        NvnState st;
        nvn_tex astc{D, 0x79};
        nvn_p_copy_bt p;
        p.tex = &astc;
        dry_copy_b2t(st, p);
        require(cpu_writes == 1 && marks.empty(), "the CPU's copy is made in the dry pass, and marks nothing");
        require(b2t_on_cpu(st, p, false) && cpu_writes == 1, "the worker does not make it again");
        buffer_fits = false;
        require(b2t_on_cpu(st, p, true) && cpu_writes == 1, "a buffer too short: left out, by the dry pass and the worker alike");
        buffer_fits = true, buffer_mapped = false;
        require(b2t_on_cpu(st, p, true) && cpu_writes == 1, "ASTC from a buffer the CPU cannot read: left out");
        nvn_tex plain{S, 1};
        nvn_p_copy_bt q;
        q.tex = &plain;
        require(!b2t_on_cpu(st, q, true) && cpu_writes == 1, "another format is the GPU's copy");
    } else {
        require(false, "a known case");
    }
    return 0;
}
} // namespace fixture
int main(int argc, char** argv) { return fixture::run(argc, argv); }
