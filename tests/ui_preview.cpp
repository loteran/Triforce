// (Triforce) The menu, the FPS counter and the notice as vk_menu.cpp draws them, into picture files, without the game and
// without a GPU: gpu/vk/vk_menu.cpp itself, compiled as it is, with a stand-in for vk_imgui.cpp that rasterizes Dear ImGui's
// draw lists in software (textured triangles, source-over, as the window's UNORM image blends them). The fonts and the
// textures are the game's own, read from a romfs folder at run time as the game's build reads them (vk_game_font.h,
// vk_game_tex.h): none is in this file or in what it writes besides the pictures themselves.
//   ui_preview <romfs folder> <width> <height> <background.rgba or -> <out.rgba> <menu keys> [fps <shown> <game> [full]]
// fps: the counter, at these rates; with full after them the FPS counter row's Full, the performance overlay, its panel fed with frames of this
// program's own making for two seconds (their times are the sleeps', so the graph is not the same twice), and this PC's own
// machine numbers (gpu/vk/perf_machine.cpp, linked in; UI_PREVIEW_GPU=<this PC's GPU's name> for its temperature and power).
// background: width x height RGBA bytes (a screenshot of the game), or - for black. menu keys: RX_VK_MENU_SCRIPT's names with
// commas ("INS,TABR,DOWN"), each a frame's tick; "-" none. The options are triforce_options.ini beside this program.
// Built by tests/ui_preview_build.ps1 (the lab's tools folder has the script that makes the comparison pictures).
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "vk_game_font.h"
#include "vk_game_tex.h"
#include "vk_imgui.h"
#include "vk_menu.h"
#include "vk_dlss.h"
#include "vk_fsr4.h"

// ---- what vk_menu.cpp takes from the rest of the back end: stand-ins ----
namespace mkwvk {
Device dev;
std::vector<std::string> gpu_device_names = {"NVIDIA GeForce RTX 4070 SUPER", "Intel(R) UHD Graphics 770"};
static std::string romfs_path;
static const char* romfs_dir_fn() { return romfs_path.c_str(); }
static MkwVkHost host_stub{};
// (the controls, for the Controls page: the Switch's own layout on a pad, a usual set of keys)
static int binding_fn(int set, int button, int slot) {
    static const int pad[HOS_SW_N] = {HOS_PAD_EAST, HOS_PAD_SOUTH, HOS_PAD_NORTH, HOS_PAD_WEST, HOS_PAD_LSTICK, HOS_PAD_RSTICK, HOS_PAD_LSHOULDER,
                                      HOS_PAD_RSHOULDER, HOS_PAD_LTRIGGER, HOS_PAD_RTRIGGER, HOS_PAD_START, HOS_PAD_BACK, HOS_PAD_LEFT, HOS_PAD_UP,
                                      HOS_PAD_RIGHT, HOS_PAD_DOWN};
    static const int keys[HOS_KEY_N] = {'E', VK_SPACE, 'Q', 'F', 'C', 'V', '1', '2', VK_SHIFT, VK_LBUTTON, VK_RETURN, VK_TAB, VK_LEFT, VK_UP,
                                        VK_RIGHT, VK_DOWN, 'W', 'S', 'A', 'D', 'I', 'K', 'J', 'L'};
    if (set == HOS_BIND_PAD) return button >= 0 && button < HOS_SW_N ? pad[button] : -1;
    return slot == 0 && button >= 0 && button < HOS_KEY_N ? keys[button] : 0;
}
static void bind_fn(int, int, int, int) {}
static uint32_t pad_raw_fn() { return 0; }
const MkwVkHost* host = &host_stub;
// (a window that is not one: the restart question is asked only with a window to close; nothing sent to it arrives)
HWND app_window() { return (HWND)(intptr_t)1; }
void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
bool antilag_available() { return false; }
bool reflex_available() { return true; }
bool dlss_built() { return false; }
void dlss_probe() {}
DlssWhy dlss_why() { return (DlssWhy)1; }
bool fsr4_built() { return false; }
void fsr4_probe() {}
FsrWhy fsr4_why() { return (FsrWhy)1; }
namespace fsr4fg {
framegen_rows::Why why() { return framegen_rows::FG_NOT_BUILT; }   // (the Frame generation row ends at 8x, as in a build without FSR)
framegen_rows::Why why_known() { return framegen_rows::FG_NOT_BUILT; }
const char* version() { return ""; }
}
bool fsr3_wanted() { return false; }
int temporal_mode() { return 0; }
void pipeline_cache_save(bool) {}
// (a bug capture's picture, vk_bugshot.cpp: none here)
void bugshot_record(VkCommandBuffer, uint32_t, uint32_t, uint32_t) {}
void bugshot_request(bool) {}
int bugshot_state() { return 0; }
namespace fsr3 {
bool supported() { return true; }
}
namespace fsr3fi {
unsigned frames_wanted() { return 0; }
}
int64_t prof_ns[PROF_COUNT];
// (Triforce: what vk_menu.cpp has come to ask of the back end since this file was written: the display, the swapchain, the
// texture detail, NVIDIA DLSS frame generation, and for the performance overlay's Full the numbers it reads. A 144 Hz
// screen, an 8 GB card with 5.2 GB in use, no upscaler's own render size, two pipelines still being compiled)
int display_refresh_mhz() { return 144000; }
int swapchain_present_mode() { return 1; }
int texture_detail_auto() { return 1; }
bool renodx_built() { return false; }
bool fsr3_native() { return false; }
void memory_trial_orderly_end() {}
uint32_t temporal_jitter_size(int) { return 0; }
bool vram_usage(uint64_t* used, uint64_t* budget, uint64_t* block_free) {
    *used = 5325ull << 20, *budget = 7782ull << 20;
    if (block_free) *block_free = 0;
    return true;
}
void draw_stats(int64_t* draws, int64_t* skipped, int64_t* dispatches, int64_t* pipelines) { *draws = 2400000, *skipped = 31, *dispatches = 9000, *pipelines = 18403; }
void draw_stats3(int64_t* pending_skips, int64_t* waits, double* wait_ms) { *pending_skips = 31, *waits = 12, *wait_ms = 384.0; }
// ((Triforce) the Full overlay's per-frame figures and the GPU it starts its machine sampler for (gpu/vk/perf_machine.cpp,
// linked in: the PC's own numbers): waits for pipelines and translations, the GPU's time and the uploads as of now)
void draw_wait_split(int64_t* pipe_us, int64_t* shader_us) { *pipe_us = prof_now() / 2000, *shader_us = prof_now() / 4000; }
int64_t gpu_time_busy_ns() { return prof_now() / 4; }
void texture_stats(int64_t* uploads, int64_t* astc_gpu, int64_t* bytes_uploaded) { *uploads = prof_now() / 50000000, *astc_gpu = 0, *bytes_uploaded = 0; }
bool gpu_device_luid(uint8_t[8]) { return false; }   // (as declared: an array, which the MSVC ABI names apart from a pointer)
bool draw_stats4(int64_t* links, double* link_ms, int64_t* lib_hits, int64_t* lib_cold, int64_t* lib_built, double* lib_build_ms) {
    *links = 900, *link_ms = 40, *lib_hits = 880, *lib_cold = 20, *lib_built = 18, *lib_build_ms = 300;
    return true;
}
// ((Triforce) frame generation for the Full overlay's FG row and its Frame generation line: the renderer's own while the row
// is on; UI_PREVIEW_DLSSG=<2 to 4>: NVIDIA DLSS frame generation runs at that multiplier instead, the rate shown that many
// times the game's: main)
static int preview_dlssg() {
    const char* e = getenv("UI_PREVIEW_DLSSG");
    return e ? atoi(e) : 0;
}
bool framegen_on() { return opts.framegen >= 1 && preview_dlssg() < 2; }
namespace dlssg {
int asked() { return preview_dlssg() >= 2; }
bool active() { return preview_dlssg() >= 2; }
int multiplier() { return preview_dlssg() >= 2 ? preview_dlssg() : 1; }
const char* status() { return "Not asked for."; }
bool reflex_on() { return false; }
}
namespace fsr3_inputs {
uint32_t jitter_size(int) { return 0; }
}
namespace pace {
int owner() { return 0; }
void limiter_text(char* out, size_t n) { snprintf(out, n, "the game's own wait"); }
}
// (the Anti-aliasing row, vk_aa.cpp: no switch over it, nothing drawing)
int aa_env_choice() { return -1; }
int aa_state() { return 0; }
} // namespace mkwvk

// ---- (Triforce) the Debug tab's name pages: stand-ins for the game's files and for the game's side ----
// The pages read the flags' and the events' names from the game's files (vk_imgui.cpp's ui::game_files). Here the names are
// the tester's own: UI_PREVIEW_FLAGS and UI_PREVIEW_EVENTS name text files with a name a line, made up by whoever runs
// this (as long and as many as the lists to be looked at); a GameDataList and event flows holding them are made in memory
// and handed to the page's own scan, so the real reading, sorting and grouping run. Neither set: "the game's files were
// not found". The Scan amiibo page lists the folder "amiibo" beside this program, as the game's does beside the game.
// UI_PREVIEW_FLAG_STATES=<characters of 1, 0, - and ?>: each tick, the flags a page asks for are answered as the game's
// side would, the characters taken in turn. UI_PREVIEW_STATUS=<text>: what the game's side says of each flag or event asked
// for (the answer comes after the ask, as the game's).
// UI_PREVIEW_AMIIBO=<0..5>: what became of the figure (HOS_AMIIBO_*). UI_PREVIEW_SAVE=1: a save is loaded (the Inventory
// page's counts, Link's age). The menu keys take WAIT too: half a second with no key, for the scan's thread.
#include "oot_cheat_items.h"
#include "oot_debug_link.h"
namespace mkwvk::ui {
static std::vector<std::string> preview_lines(const char* env) {
    std::vector<std::string> lines;
    const char* path = getenv(env);
    FILE* f = path && *path ? fopen(path, "rb") : nullptr;
    if (!f) return lines;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n) lines.push_back(line);
    }
    fclose(f);
    return lines;
}
std::vector<std::string> folder_files(const std::string& dir, const char* ending) {   // (as vk_imgui.cpp's)
    std::vector<std::string> names;
    WIN32_FIND_DATAA fd;
    const HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    const size_t len = strlen(ending);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        const std::string name = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name.size() > len && !_stricmp(name.c_str() + name.size() - len, ending)) names.push_back(name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end());
    return names;
}
bool game_files(const char* folder, const char* ending, const std::function<void(const std::string& name, const std::vector<unsigned char>& bytes)>& each) {
    (void)ending;
    const std::vector<std::string> flags = preview_lines("UI_PREVIEW_FLAGS"), events = preview_lines("UI_PREVIEW_EVENTS");
    if (flags.empty() && events.empty()) return false;
    std::vector<unsigned char> b;
    auto u32 = [&](uint32_t v) { for (int i = 3; i >= 0; i--) b.push_back((unsigned char)(v >> (8 * i))); };
    auto u24 = [&](uint32_t v) { for (int i = 2; i >= 0; i--) b.push_back((unsigned char)(v >> (8 * i))); };
    auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) b[at + i] = (unsigned char)(v >> (24 - 8 * i)); };
    if (!strcmp(folder, "GameData")) {   // (a big-endian BYML, as the game's: {Data: {Bool: [{Hash}, ...]}})
        b = {'B', 'Y', 0, 7};
        u32(16), u32(0), u32(0);
        b.push_back(0xc2), u24(3);   // (the keys: Bool, Data, Hash)
        u32(20), u32(25), u32(30), u32(35);   // (from the table's start: after its four bytes and these four offsets)
        for (const char* key : {"Bool", "Data", "Hash"}) b.insert(b.end(), key, key + 5);
        while (b.size() & 3) b.push_back(0);
        std::vector<uint32_t> at;
        for (const std::string& name : flags) {
            at.push_back((uint32_t)b.size());
            b.push_back(0xc1), u24(1), u24(2), b.push_back(0xd3), u32(oot_name_hash(name.data(), name.size()));
        }
        const uint32_t array = (uint32_t)b.size();
        b.push_back(0xc0), u24((uint32_t)flags.size());
        b.insert(b.end(), flags.size(), 0xc1);
        while (b.size() & 3) b.push_back(0);
        for (uint32_t a : at) u32(a);
        const uint32_t data = (uint32_t)b.size();
        b.push_back(0xc1), u24(1), u24(0), b.push_back(0xc0), u32(array);
        put32(12, (uint32_t)b.size());
        b.push_back(0xc1), u24(1), u24(1), b.push_back(0xc1), u32(data);
        each("GameDataList.Product.100.byml.zs", b);
        return true;
    }
    // (an event flow each: the header with one flowchart; the first one's texts are the flags' names)
    for (size_t i = 0; i < std::max<size_t>(events.size(), 1); i++) {
        b.assign(0x48, 0);
        memcpy(b.data(), "BFEVFL\0\0", 8);
        b[0xc] = 0xff, b[0xd] = 0xfe, b[0x20] = events.empty() ? 0 : 1;
        for (size_t k = 0; i == 0 && k < flags.size(); k++) {
            b.push_back((unsigned char)flags[k].size()), b.push_back(0);
            b.insert(b.end(), flags[k].begin(), flags[k].end());
            b.push_back(0);
        }
        each((events.empty() ? std::string("None") : events[i]) + ".bfevfl.zs", b);
    }
    return true;
}
} // namespace mkwvk::ui
/// The game's side for a tick: the pages' lines taken, the flags asked for answered, and what the other switches say
static void preview_host_tick() {
    using namespace mkwvk;
    static std::string line, watch;
    static bool told = false;
    auto say = [](int kind, const std::string& text) {
        for (char ch : text) menu_option_set(HOS_OPT_DEBUG_TEXT, (unsigned char)ch);
        menu_option_set(HOS_OPT_DEBUG_SAID, kind);
    };
    for (int ch; (ch = menu_take_debug_ask()) != 0;) {
        if (ch != '\n') {
            line += (char)ch;
            continue;
        }
        if (!line.empty() && line[0] == 'W') watch = line;
        // ((Triforce) a flag or an event asked for: answered as host/hos_main.c answers one, after it was asked)
        else if (const char* status = getenv("UI_PREVIEW_STATUS"); status && !line.empty()) say(OOT_SAID_STATUS, status);
        line.clear();
    }
    while (menu_take_amiibo_ask() != 0) {}
    const char* states = getenv("UI_PREVIEW_FLAG_STATES");
    OotAsk ask;
    if (states && *states && oot_ask_parse(watch.c_str(), &ask) && ask.count) {
        char text[16 + OOT_WATCH_MAX];
        int at = snprintf(text, sizeof text, "%08x ", (unsigned)oot_watch_sum(ask.watch, ask.count));
        for (int i = 0; i < ask.count; i++) text[at++] = states[(size_t)i % strlen(states)];
        text[at] = 0;
        say(OOT_SAID_FLAGS, text);
    }
    if (told) return;
    told = true;
    if (const char* figure = getenv("UI_PREVIEW_AMIIBO")) menu_option_set(HOS_OPT_AMIIBO_SAID, atoi(figure));
    if (getenv("UI_PREVIEW_SAVE")) {
        for (int i = 0; i < 400; i++) menu_option_set(OOT_CHEAT_ITEM_BASE + i, i % 3 == 0 ? 0 : i % 7 + 1);
        menu_option_set(OOT_CHEAT_AGE_ID, 0);
    }
}

// ---- the stand-in for vk_imgui.cpp ----
namespace mkwvk::ui {
namespace {
Fonts fonts_;
std::vector<std::pair<std::string, Art>> arts_;
std::vector<unsigned char> target;   // (RGBA, the picture drawn over)
uint32_t tw = 0, th = 0;
bool made = false;
bool laying_out = false;   // (a frame built and not drawn: what the menu learns from its own drawing, between two keys)

ImFont* add_font(game_font::Reader& r, const char* name, bool latin) {
    std::vector<unsigned char> bytes = r.read(name, latin);
    if (bytes.empty()) return nullptr;
    void* font = IM_ALLOC(bytes.size());
    memcpy(font, bytes.data(), bytes.size());
    ImFontConfig cfg;
    snprintf(cfg.Name, sizeof cfg.Name, "%s", name);
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(font, (int)bytes.size(), 18.0f, &cfg);
}
void init() {
    made = true;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->TexDesiredFormat = ImTextureFormat_RGBA32;
    game_font::Reader reader;
    if (!reader.open(romfs_path)) {
        fprintf(stderr, "no game files at %s\n", romfs_path.c_str());
        exit(2);
    }
    fonts_.body = add_font(reader, "ClarimoUDPE-Regular", true);   // (as vk_imgui.cpp load_fonts)
    fonts_.strong = add_font(reader, "ClarimoUDPE-DemiBold", true);
    fonts_.heading = add_font(reader, "CetraDisplayPro-Medium", true);
    fonts_.menu = add_font(reader, "CetraDisplayPro-Regular", true);
    fonts_.number = add_font(reader, "NinDev-Number", false);
    if (!getenv("UI_PREVIEW_NO_GLYPHS")) fonts_.glyph = add_font(reader, "External", false);
    if (!fonts_.body || !fonts_.strong || !fonts_.heading || !fonts_.menu) {
        fprintf(stderr, "the game's fonts were not read\n");
        exit(2);
    }
    if (!getenv("UI_PREVIEW_NO_ART")) {
        std::vector<unsigned char> bytes;
        game_tex::Skin skin;
        if (!reader.read_packed("UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs", bytes) || !game_tex::make_skin(std::move(bytes), skin)) {
            fprintf(stderr, "the game's menu textures were not read\n");
            exit(2);
        }
        for (game_tex::Skin::Entry& e : skin.images) {
            ImTextureData* t = IM_NEW(ImTextureData)();
            t->Create(ImTextureFormat_RGBA32, (int)e.img.w, (int)e.img.h);
            memcpy(t->GetPixels(), e.img.rgba.data(), e.img.rgba.size());
            t->UseColors = true;
            ImGui::RegisterUserTexture(t);
            arts_.push_back({e.name, Art{t, (float)e.img.w, (float)e.img.h}});
        }
    }
}

// ---- the rasterizer ----
struct Sampler {
    const unsigned char* px = nullptr;
    int w = 0, h = 0, bpp = 4;
    void at(float u, float v, float out[4]) const {
        if (!px) {
            out[0] = out[1] = out[2] = out[3] = 1;
            return;
        }
        const float x = u * w - 0.5f, y = v * h - 0.5f;
        const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
        const float fx = x - x0, fy = y - y0;
        float acc[4] = {0, 0, 0, 0};
        for (int j = 0; j < 2; j++)
            for (int i = 0; i < 2; i++) {
                const int xi = std::clamp(x0 + i, 0, w - 1), yi = std::clamp(y0 + j, 0, h - 1);
                const float k = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                const unsigned char* p = px + ((size_t)yi * w + xi) * bpp;
                if (bpp == 4) for (int c = 0; c < 4; c++) acc[c] += p[c] * k;
                else acc[0] += 255 * k, acc[1] += 255 * k, acc[2] += 255 * k, acc[3] += p[0] * k;
            }
        for (int c = 0; c < 4; c++) out[c] = acc[c] / 255.0f;
    }
};
void triangle(const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c, const Sampler& tex, const ImVec4& clip) {
    const float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
    if (area == 0) return;
    const int x0 = std::max((int)std::floor(std::min({a.pos.x, b.pos.x, c.pos.x})), (int)std::max(clip.x, 0.0f));
    const int x1 = std::min((int)std::ceil(std::max({a.pos.x, b.pos.x, c.pos.x})), (int)std::min(clip.z, (float)tw));
    const int y0 = std::max((int)std::floor(std::min({a.pos.y, b.pos.y, c.pos.y})), (int)std::max(clip.y, 0.0f));
    const int y1 = std::min((int)std::ceil(std::max({a.pos.y, b.pos.y, c.pos.y})), (int)std::min(clip.w, (float)th));
    const ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a.col), cb = ImGui::ColorConvertU32ToFloat4(b.col), cc = ImGui::ColorConvertU32ToFloat4(c.col);
    const bool flat = a.col == b.col && b.col == c.col;
    const bool one_texel = a.uv.x == b.uv.x && a.uv.x == c.uv.x && a.uv.y == b.uv.y && a.uv.y == c.uv.y;
    float texel[4] = {1, 1, 1, 1};
    if (one_texel) tex.at(a.uv.x, a.uv.y, texel);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((b.pos.x - px) * (c.pos.y - py) - (b.pos.y - py) * (c.pos.x - px)) / area;
            float w1 = ((c.pos.x - px) * (a.pos.y - py) - (c.pos.y - py) * (a.pos.x - px)) / area;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float t[4];
            if (one_texel) memcpy(t, texel, sizeof t);
            else tex.at(a.uv.x * w0 + b.uv.x * w1 + c.uv.x * w2, a.uv.y * w0 + b.uv.y * w1 + c.uv.y * w2, t);
            const float col[4] = {flat ? ca.x : ca.x * w0 + cb.x * w1 + cc.x * w2, flat ? ca.y : ca.y * w0 + cb.y * w1 + cc.y * w2,
                                  flat ? ca.z : ca.z * w0 + cb.z * w1 + cc.z * w2, flat ? ca.w : ca.w * w0 + cb.w * w1 + cc.w * w2};
            const float al = t[3] * col[3];
            if (al <= 0) continue;
            unsigned char* d = target.data() + ((size_t)y * tw + x) * 4;
            for (int k = 0; k < 3; k++) d[k] = (unsigned char)std::clamp(d[k] * (1 - al) + t[k] * col[k] * al * 255.0f + 0.5f, 0.0f, 255.0f);
            d[3] = 255;
        }
}
void raster(ImDrawData* dd) {
    if (dd->Textures)
        for (ImTextureData* tex : *dd->Textures)
            if (tex->Status != ImTextureStatus_OK && tex->Status != ImTextureStatus_Destroyed) {
                if (tex->Status == ImTextureStatus_WantDestroy) tex->SetTexID(ImTextureID_Invalid), tex->SetStatus(ImTextureStatus_Destroyed);
                else tex->SetTexID((ImTextureID)(intptr_t)1), tex->SetStatus(ImTextureStatus_OK);
            }
    for (ImDrawList* list : dd->CmdLists)
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) continue;
            Sampler s;
            if (ImTextureData* t = cmd.TexRef._TexData) s.px = (const unsigned char*)t->GetPixels(), s.w = t->Width, s.h = t->Height, s.bpp = t->BytesPerPixel;
            const ImDrawIdx* idx = list->IdxBuffer.Data + cmd.IdxOffset;
            const ImDrawVert* vtx = list->VtxBuffer.Data + cmd.VtxOffset;
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) triangle(vtx[idx[i]], vtx[idx[i + 1]], vtx[idx[i + 2]], s, cmd.ClipRect);
        }
}
} // namespace

bool begin(uint32_t w, uint32_t h) {
    if (!made) init();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    return true;
}
const Fonts& fonts() { return fonts_; }
ImTextureData* logo(float* aspect) {
    *aspect = 1;
    return nullptr;
}
const Art* art(const char* name) {
    for (const auto& a : arts_)
        if (a.first == name) return &a.second;
    return nullptr;
}
void want_glyphs() {}
void prebake(const float*, int) {}
void idle() {}
void record_to(VkCommandBuffer, VkImage, VkImageView, uint32_t, uint32_t) {
    ImGui::Render();
    if (!laying_out) raster(ImGui::GetDrawData());
}
void record(VkCommandBuffer cmd, uint32_t, uint32_t w, uint32_t h) { record_to(cmd, VK_NULL_HANDLE, VK_NULL_HANDLE, w, h); }
} // namespace mkwvk::ui

int main(int argc, char** argv) {
    if (argc < 7) {
        fprintf(stderr, "ui_preview <romfs> <width> <height> <background.rgba or -> <out.rgba> <keys or -> [fps <shown> <game> [full]]\n");
        return 1;
    }
    using namespace mkwvk;
    romfs_path = argv[1];
    host_stub.romfs_dir = romfs_dir_fn;
    host_stub.binding = binding_fn, host_stub.bind = bind_fn, host_stub.pad_raw = pad_raw_fn;
    ui::tw = (uint32_t)atoi(argv[2]), ui::th = (uint32_t)atoi(argv[3]);
    dev.swap_extent = {ui::tw, ui::th};
    ui::target.assign((size_t)ui::tw * ui::th * 4, 0);
    for (size_t i = 3; i < ui::target.size(); i += 4) ui::target[i] = 255;
    if (strcmp(argv[4], "-")) {
        FILE* f = fopen(argv[4], "rb");
        if (!f || fread(ui::target.data(), 1, ui::target.size(), f) != ui::target.size()) {
            fprintf(stderr, "background %s: not %ux%u RGBA\n", argv[4], ui::tw, ui::th);
            return 2;
        }
        fclose(f);
    }
    // (the keys as RX_VK_MENU_SCRIPT has them, all due at once: menu_tick takes one a call)
    // ((Triforce) WAIT: no key, the keys after it half a second later, while the ticks go on: a thread of the menu's has that
    // long, the name pages' read of the game's files)
    std::string script;
    int ticks = 2;
    double wait = 0;
    if (strcmp(argv[6], "-")) {
        for (const char* p = argv[6]; *p;) {
            const char* e = strchr(p, ',');
            const std::string key(p, e ? (size_t)(e - p) : strlen(p));
            char at[32];
            snprintf(at, sizeof at, "%.1f:", wait);
            if (key == "WAIT") wait += 0.5;
            else script += (script.empty() ? "" : ",") + std::string(at) + key, ticks++;
            p = e ? e + 1 : p + key.size();
        }
        SetEnvironmentVariableA("RX_VK_MENU_SCRIPT", script.c_str());
        _putenv_s("RX_VK_MENU_SCRIPT", script.c_str());
    }
    // (UI_PREVIEW_SCRIPT: RX_VK_MENU_SCRIPT as given, its keys at their own seconds, for pages that wait on something: the Mods
    // page's list from GameBanana. With UI_PREVIEW_FRAMES and UI_PREVIEW_SLEEP_MS the picture is the last of that many frames)
    if (const char* timed = getenv("UI_PREVIEW_SCRIPT")) {
        SetEnvironmentVariableA("RX_VK_MENU_SCRIPT", timed);
        _putenv_s("RX_VK_MENU_SCRIPT", timed);
    }
    const int frames = getenv("UI_PREVIEW_FRAMES") ? std::max(3, atoi(getenv("UI_PREVIEW_FRAMES"))) : 3;
    const int sleep_ms = getenv("UI_PREVIEW_SLEEP_MS") ? atoi(getenv("UI_PREVIEW_SLEEP_MS")) : 20;
    _putenv_s("RX_VK_UI_STILL", "1");
    _putenv_s("HOS_FULLSCREEN", "0");   // (the Fullscreen row left alone: there is no window to ask)
    menu_init();
    menu_set_gpu_count(2);
    if (argc >= 10 && !strcmp(argv[7], "fps")) {   // (the counter: the frames of a second counted, at these rates)
        opts.show_fps = 1;
        // ((Triforce) UI_PREVIEW_DLSSG=n: DLSS frame generation's n frames shown to each of the game's, as the present counts
        // them (vk_replay.cpp menu_note_frame(true, dlssg::multiplier())): the rate shown is n times the game's, whatever <shown> says)
        const double game = atof(argv[9]);
        const int dlssg_n = dlssg::multiplier();
        const double shown = dlssg_n > 1 && game > 0 ? game * dlssg_n : atof(argv[8]);
        const int per_frame = dlssg_n > 1 && game > 0 ? dlssg_n : game > 0 && game != shown ? 2 : 1;   // (each game frame's shown ones, on Full)
        if (game > 0 && game != shown) opts.framegen = 2;
        menu_tick();
        const int64_t t0 = prof_now();
        Sleep(1000);
        const double secs = (prof_now() - t0) / 1e9;
        menu_note_frame(false, (int)std::lround(shown * secs));
        for (int i = 0; i < (int)std::lround((game > 0 ? game : shown) * secs); i++) menu_note_frame(true, 0);
        if (argc >= 11 && !strcmp(argv[10], "full")) {   // (Triforce: the FPS counter row's Full, the performance overlay)
            opts.show_fps = 2;
            // (the machine sampler's GPU: UI_PREVIEW_GPU names this PC's as Vulkan does, so that its vendor's library has it)
            const char* gpu = getenv("UI_PREVIEW_GPU");
            snprintf(dev.props.deviceName, sizeof dev.props.deviceName, "%s", gpu ? gpu : gpu_device_names[0].c_str());
            menu_note_present_sizes(2560, 1440, ui::tw, ui::th);   // (the game's picture and the window, for the settings' lines)
            menu_tick();   // (the first sample on Full empties the ring: the frames after it are the panel's)
            for (int i = 0; i < 140; i++) {
                Sleep(i == 60 ? 55 : 5 + (unsigned)(i * 7) % 14);   // (frames of 5 to 19 ms, as the system's timer gives them, and one long one)
                menu_note_frame(true, per_frame);
            }
        }
    }
    // (UI_PREVIEW_GAME_OPTIONS=1: as inside the game's own Options screen: the host's word each tick, as host/oot_menu.c gives it)
    const bool game_options = getenv("UI_PREVIEW_GAME_OPTIONS") != nullptr;
    if (game_options) menu_option_set(HOS_OPT_GAME_OPTIONS, 1), menu_tick();
    const int64_t ticks_from = prof_now();
    // (a tick for every key once the last WAIT is over, and the ticks of the waits before that)
    for (int i = 0; i < ticks; i += (prof_now() - ticks_from) / 1e9 >= wait) {
        if (game_options) menu_option_set(HOS_OPT_GAME_OPTIONS, 1);
        menu_tick();
        preview_host_tick();
        // ((Triforce) a frame between two keys, as in the game, where each tick is followed by a drawing: a list page takes its
        // columns and its cards to a page from its own drawing, and the next key moves by them)
        ui::laying_out = true;
        menu_record_image(nullptr, (VkImage)1, (VkImageView)1, ui::tw, ui::th);
        ui::laying_out = false;
        if (wait > 0) Sleep(4);
    }
    menu_tick();   // (what the game's side said in the last tick is taken)
    // (two frames: the first lays out and bakes the glyphs, the second is the picture, the scroll settled)
    std::vector<unsigned char> clean = ui::target;
    for (int frame = 0; frame < frames; frame++) {
        ui::target = clean;
        if (game_options) menu_option_set(HOS_OPT_GAME_OPTIONS, 1);
        if (frame >= 3) menu_tick();   // (the timed keys and what waits: one tick a frame)
        menu_record_image(nullptr, (VkImage)1, (VkImageView)1, ui::tw, ui::th);
        if (!ui::made) {   // (nothing shows: the picture as it came)
            break;
        }
        Sleep(sleep_ms);
    }
    FILE* f = fopen(argv[5], "wb");
    if (!f) return 3;
    fwrite(ui::target.data(), 1, ui::target.size(), f);
    fclose(f);
    return 0;
}
