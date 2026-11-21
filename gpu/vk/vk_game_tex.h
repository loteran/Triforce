// (Triforce) The game's own UI textures, read from the player's game files, in memory only (vk_imgui.cpp; never written
// anywhere, never in the repository or a binary). romfs/UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs is zstd with
// the dictionary zs.zsdic (vk_game_font.h Reader); in it a BNTX: a table of BRTI records, each a texture by name. The menus'
// textures are one level, 2D, in two formats: BC4 (0x1d01: one channel, a mask the layout's material colours) and ASTC 4x4
// (0x2d06 sRGB, 0x2d01), stored in the console's block-linear order (the Switch 2's: third_party/suyu decoders.cpp). No
// Vulkan or ImGui here: tests/ui_preview.cpp draws the menu with the same textures offline.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "video_core/textures/astc.h"
#include "video_core/textures/decoders.h"

namespace mkwvk::game_tex {

struct Image {
    uint32_t w = 0, h = 0;
    bool mask = false;   // (BC4: one channel, in every byte of rgba's pixels' alpha; its colour white)
    bool srgb = false;
    std::vector<unsigned char> rgba;
};

/// A BC4 block's 16 values (4 x 4, row by row)
inline void bc4_block(const unsigned char* b, unsigned char out[16]) {
    unsigned char v[8];
    v[0] = b[0], v[1] = b[1];
    if (v[0] > v[1]) {
        for (int i = 1; i < 7; i++) v[i + 1] = (unsigned char)(((7 - i) * v[0] + i * v[1] + 3) / 7);
    } else {
        for (int i = 1; i < 5; i++) v[i + 1] = (unsigned char)(((5 - i) * v[0] + i * v[1] + 2) / 5);
        v[6] = 0, v[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = v[bits >> (3 * i) & 7];
}

class Bntx {
public:
    struct Entry {
        std::string name;
        uint32_t w = 0, h = 0, format = 0, size = 0, block_height = 0;
        uint64_t data = 0;
    };
    /// The archive's table (the bytes are kept: decode reads them)
    bool parse(std::vector<unsigned char>&& bytes) {
        d = std::move(bytes);
        entries.clear();
        if (d.size() < 0x60 || memcmp(d.data(), "BNTX", 4)) return false;
        const uint32_t count = u32(0x24);
        const uint64_t table = u64(0x28);
        if (count > 65536 || table > d.size() || d.size() - table < (uint64_t)count * 8) return false;
        for (uint32_t i = 0; i < count; i++) {
            const uint64_t p = u64(table + 8ull * i);
            if (p > d.size() || d.size() - p < 0xA0 || memcmp(d.data() + p, "BRTI", 4)) return false;
            Entry e;
            e.format = u32(p + 0x1C), e.w = u32(p + 0x24), e.h = u32(p + 0x28);
            e.block_height = u32(p + 0x34) & 7;
            e.size = u32(p + 0x50);
            const uint64_t name = u64(p + 0x60), mips = u64(p + 0x70);
            if (name + 2 > d.size() || mips + 8 > d.size()) return false;
            const size_t len = d[name] | d[name + 1] << 8;
            if (name + 2 + len > d.size()) return false;
            e.name.assign((const char*)d.data() + name + 2, len);
            e.data = u64(mips);
            const bool one_level_2d = u32(p + 0x2C) <= 1 && u32(p + 0x30) <= 1;
            if (!one_level_2d || e.data > d.size() || d.size() - e.data < e.size) e.size = 0;   // (not one this reads)
            entries.push_back(std::move(e));
        }
        return true;
    }
    const Entry* find(const char* name) const {
        for (const Entry& e : entries)
            if (e.name == name) return &e;
        return nullptr;
    }
    /// A texture's pixels (RGBA, 8 bits each): false for a format or a shape this does not read
    bool decode(const Entry& e, Image& out) const {
        const uint32_t kind = e.format >> 8, bw = (e.w + 3) / 4, bh = (e.h + 3) / 4;
        const uint32_t bytes = kind == 0x1d ? 8 : kind == 0x2d ? 16 : 0;
        if (!bytes || !e.size || !e.w || !e.h || e.w > 8192 || e.h > 8192) return false;
        std::vector<unsigned char> blocks((size_t)bw * bh * bytes);
        // (the console's block-linear order to rows of blocks)
        if (Tegra::Texture::CalculateSize(true, bytes, bw, bh, 1, e.block_height, 0) > e.size) return false;
        Tegra::Texture::UnswizzleTexture(std::span<unsigned char>(blocks), std::span<const unsigned char>(d.data() + e.data, e.size), bytes,
                                         bw, bh, 1, e.block_height, 0, 0);
        out.w = e.w, out.h = e.h, out.mask = kind == 0x1d, out.srgb = (e.format & 0xff) == 6;
        out.rgba.assign((size_t)e.w * e.h * 4, 255);
        if (kind == 0x1d) {
            for (uint32_t by = 0; by < bh; by++)
                for (uint32_t bx = 0; bx < bw; bx++) {
                    unsigned char v[16];
                    bc4_block(blocks.data() + ((size_t)by * bw + bx) * 8, v);
                    for (uint32_t y = 0; y < 4 && by * 4 + y < e.h; y++)
                        for (uint32_t x = 0; x < 4 && bx * 4 + x < e.w; x++)
                            out.rgba[((size_t)(by * 4 + y) * e.w + bx * 4 + x) * 4 + 3] = v[y * 4 + x];
                }
            return true;
        }
        Tegra::Texture::ASTC::DecompressSerial(std::span<const unsigned char>(blocks), e.w, e.h, 1, 4, 4, std::span<unsigned char>(out.rgba));
        return true;
    }
    std::vector<Entry> entries;

private:
    uint32_t u32(uint64_t o) const {
        uint32_t v;
        memcpy(&v, d.data() + o, 4);
        return v;
    }
    uint64_t u64(uint64_t o) const {
        uint64_t v;
        memcpy(&v, d.data() + o, 8);
        return v;
    }
    std::vector<unsigned char> d;
};

// ---- the menus' skin: the textures the Options screen is drawn with (vk_ui.h), by the game's own names, and a few made
// from them here as the game's layout shader makes them at each draw: a plate is its shape (BtnThinBase_00's alpha), its
// colour between two browns by the grain texture (BtnThinBase_05), its fine frame line (BtnThinBase_04, or _02 with the
// curls at its corners) over it. The colours are the layouts' own (kPlateWhite and the others below).
struct Skin {
    struct Entry {
        std::string name;
        Image img;
    };
    std::vector<Entry> images;
    const Image* get(const char* name) const {
        for (const Entry& e : images)
            if (e.name == name) return &e.img;
        return nullptr;
    }
};
// ---- (Triforce) the layouts' own material colours. Each pane of the game's layouts (UI/Layout2Archive/Common.*.blarc, BFFH)
// has a material with a white colour and a black colour, linear RGBA floats in the layout's colour table, and its texture
// picks between them: what shows is black + (white - black) x the texture's colour (linear), its alpha the texture's (the
// black colours' alpha is 0, the whites' 1). Read from the player's own files once (PaOptionBtnL_00, PaMenuCategory_00) and
// written here as numbers; the layout itself is never read at run time. They agree with the screen: the rows' plate measured
// 98,74,37 at its rim and 111,84,44 at its middle, its value's orange 211,144,37 where the plate's texture is white.
// (Before, the plates' browns ran the other way, dark in the middle; the value plates were the texture times one measured
// colour, which made the orange's middle 168,115,29 where the game's is about 190,126,30.)
struct Lin {
    float r, g, b;
};
// PaOptionBtnL_00 (an option's row): W_Base_03 the row's plate (BtnThinBase_05, the grain), W_BaseFlame_02 its fine line
// (BtnThinBase_04), W_Base_01 the value's dark panel and W_CheckL_00 / W_CheckR_00 the chosen value's orange plate (both
// OptionBtnThinElement_00, a grey colour texture, not a mask)
constexpr Lin kPlateWhite{0.118689f, 0.0658853f, 0.017683f}, kPlateBlack{0.161467f, 0.0883403f, 0.0258325f},
              kPlateLine{0.200063f, 0.139549f, 0.0484867f};
constexpr Lin kValueWhite{0.00489631f, 0.00456596f, 0.00340996f}, kValueBlack{0.0331048f, 0.0257721f, 0.0108705f};
constexpr Lin kValueOnWhite{0.655931f, 0.284227f, 0.0186829f}, kValueOnBlack{0.318547f, 0.0998504f, 0.00665673f};
// PaMenuCategory_00 (the pause menu's chosen category, MenuHeaderSelect_00): P_PlusMenuBg_00's, the + menu's, where the
// System tab is (its blue to red, 0.19, is the measured tint's, 62 / 190 = 0.33 against P_MinusMenuBg_00's 0.18)
constexpr Lin kTabWhite{0.286275f, 0.164706f, 0.054902f}, kTabBlack{0.0313726f, 0.00392157f, 0.0f};

inline float srgb_to_linear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
inline unsigned char linear_to_srgb8(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
    return (unsigned char)(s * 255 + 0.5f);
}
/// A texel's colour channel, linear (a mask's is white, as the game samples it)
struct ToLinear {
    float lut[256];
    explicit ToLinear(bool srgb) {
        for (int i = 0; i < 256; i++) lut[i] = srgb ? srgb_to_linear(i / 255.0f) : i / 255.0f;
    }
};
/// A texture as a pane of the game's layout shows it with its material's colours (black, white): the same size, sRGB
inline Image make_tinted(const Image& tex, Lin black, Lin white) {
    const ToLinear lin(tex.srgb);
    Image out;
    out.w = tex.w, out.h = tex.h, out.srgb = true;
    out.rgba.resize(tex.rgba.size());
    const float b[3] = {black.r, black.g, black.b}, w[3] = {white.r, white.g, white.b};
    for (size_t i = 0; i + 3 < tex.rgba.size(); i += 4) {
        for (int k = 0; k < 3; k++) out.rgba[i + k] = linear_to_srgb8(b[k] + (w[k] - b[k]) * lin.lut[tex.rgba[i + k]]);
        out.rgba[i + 3] = tex.rgba[i + 3];
    }
    return out;
}

/// A plate from the game's three layers (shape, grain, line): the same size as the shape. The grain is W_Base_03's texture
/// with its colours (black where the grain is dark: the plate's lighter middle; white where it is white: its rim), the line
/// W_BaseFlame_02's over it, both in linear light as the game blends its panes
inline Image make_plate(const Image& shape, const Image& grain, const Image* line, Lin black, Lin white, Lin line_col) {
    const ToLinear lin(grain.srgb);
    Image out;
    out.w = shape.w, out.h = shape.h, out.srgb = true;
    out.rgba.resize((size_t)out.w * out.h * 4);
    for (uint32_t y = 0; y < out.h; y++)
        for (uint32_t x = 0; x < out.w; x++) {
            const size_t i = ((size_t)y * out.w + x) * 4;
            const uint32_t gx = (std::min)(x, grain.w - 1), gy = (std::min)(y, grain.h - 1);
            const float g = lin.lut[grain.rgba[((size_t)gy * grain.w + gx) * 4]];
            float c[3] = {black.r + (white.r - black.r) * g, black.g + (white.g - black.g) * g, black.b + (white.b - black.b) * g};
            if (line && x < line->w && y < line->h) {
                const float a = line->rgba[((size_t)y * line->w + x) * 4 + 3] / 255.0f;
                c[0] += (line_col.r - c[0]) * a, c[1] += (line_col.g - c[1]) * a, c[2] += (line_col.b - c[2]) * a;
            }
            out.rgba[i] = linear_to_srgb8(c[0]), out.rgba[i + 1] = linear_to_srgb8(c[1]), out.rgba[i + 2] = linear_to_srgb8(c[2]);
            out.rgba[i + 3] = shape.rgba[i + 3];
        }
    return out;
}
/// A mask made smaller by a whole factor (each texel the mean of its block): for a soft one that needs no more
inline Image shrink_mask(const Image& m, uint32_t f) {
    if (f <= 1 || m.w < f || m.h < f) return m;
    Image out;
    out.w = m.w / f, out.h = m.h / f, out.mask = true, out.srgb = m.srgb;
    out.rgba.assign((size_t)out.w * out.h * 4, 255);
    for (uint32_t y = 0; y < out.h; y++)
        for (uint32_t x = 0; x < out.w; x++) {
            uint32_t sum = 0;
            for (uint32_t v = 0; v < f; v++)
                for (uint32_t u = 0; u < f; u++) sum += m.rgba[((size_t)(y * f + v) * m.w + x * f + u) * 4 + 3];
            out.rgba[((size_t)y * out.w + x) * 4 + 3] = (unsigned char)((sum + f * f / 2) / (f * f));
        }
    return out;
}

/// The cursor from the game's two masks, as its layout shader draws it: OptionBtnThinCursorLine is a ramp from the frame's
/// outer edge inwards (0 to 1 over 28 texels: a distance, not a picture of a line), the line a band of it; the glow
/// (OptionBtnThinCursorGlow, a wider ramp) shows outside the line. The band is measured from the game's own cursor on its
/// Options screen (1920 x 1080: the line 13 layout units thick, its inner edge a box of 809 x 114 units in the 960 x 260
/// pane: the ramp from 0.34 to 0.65; about 20 units of blue-white glow outside it, nothing inside).
/// *line and *glow: white, their strength in alpha (the glow is tinted and breathes at the draw)
inline void make_cursor(const Image& ramp, const Image& wide, Image* line, Image* glow) {
    auto step = [](float v, float a, float b) {
        const float t = std::clamp((v - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3 - 2 * t);
    };
    for (Image* o : {line, glow}) o->w = ramp.w, o->h = ramp.h, o->mask = true, o->rgba.assign((size_t)ramp.w * ramp.h * 4, 255);
    for (size_t i = 0; i < (size_t)ramp.w * ramp.h; i++) {
        const float l = ramp.rgba[i * 4 + 3] / 255.0f;
        const float g = i < (size_t)wide.w * wide.h ? wide.rgba[i * 4 + 3] / 255.0f : 0;
        const float la = step(l, 0.31f, 0.37f) * (1 - step(l, 0.62f, 0.69f));
        const float outside = std::pow(std::clamp(g / 0.36f, 0.0f, 1.0f), 1.6f) * (1 - step(l, 0.31f, 0.37f));
        line->rgba[i * 4 + 3] = (unsigned char)(la * 255 + 0.5f);
        glow->rgba[i * 4 + 3] = (unsigned char)(outside * 255 + 0.5f);
    }
}

/// The skin from the archive's bytes (a BNTX): false when it is not the game's (a texture of the list missing). (Triforce)
/// The optional ones are the standalone menu's background (SystemBG_00's shade and noise): one missing is left out, and the
/// menu draws its plain dim there instead (vk_ui.h system_bg); the skin stays
inline bool make_skin(std::vector<unsigned char>&& bntx_bytes, Skin& skin) {
    static const char* const names[] = {
        "SystemHeader_00", "SystemBg_05", "SystemCaptionBg_00", "SystemGuideBase_00", "BtnThinBase_00", "BtnThinBase_01",
        "BtnThinBase_02", "BtnThinBase_04", "BtnThinBase_05", "OptionBtnThinElement_00", "OptionBtnThinCursorLine_00",
        "OptionBtnThinCursorGlow_00", "ScrollBarBase_00", "ScrollBar_00", "OptionSeparateMark_00",
        "SystemLine_03", "MenuHeaderSelect_00", "OptionCheckMark_00"};
    static const char* const optional[] = {"SystemBgSh_00", "DecoNoise_00"};
    Bntx bntx;
    if (!bntx.parse(std::move(bntx_bytes))) return false;
    skin.images.clear();
    for (const char* n : names) {
        const Bntx::Entry* e = bntx.find(n);
        Skin::Entry out;
        out.name = n;
        if (!e || !bntx.decode(*e, out.img)) return false;
        skin.images.push_back(std::move(out));
    }
    for (const char* n : optional) {
        const Bntx::Entry* e = bntx.find(n);
        Skin::Entry out;
        out.name = n;
        if (!e || !bntx.decode(*e, out.img) || !out.img.mask) continue;
        // (the shade, 2560 x 1440, is a soft vignette: a quarter of its size each way draws the same, 1 MB in place of 15)
        if (out.name == "SystemBgSh_00") out.img = shrink_mask(out.img, 4);
        skin.images.push_back(std::move(out));
    }
    const Image *shape = skin.get("BtnThinBase_00"), *grain = skin.get("BtnThinBase_05");
    Skin::Entry row{"plate_row", make_plate(*shape, *grain, skin.get("BtnThinBase_04"), kPlateBlack, kPlateWhite, kPlateLine)};
    Skin::Entry row_sel{"plate_row_sel", row.img};   // (the selected row's plate is the same: the game marks it with the cursor alone)
    Skin::Entry btn{"plate_btn", make_plate(*shape, *grain, skin.get("BtnThinBase_02"), kPlateBlack, kPlateWhite, kPlateLine)};
    skin.images.push_back(std::move(row)), skin.images.push_back(std::move(row_sel)), skin.images.push_back(std::move(btn));
    // (the value's panel, the chosen value's orange plate and the chosen tab's plate, as their panes show them: drawn untinted)
    const Image *element = skin.get("OptionBtnThinElement_00"), *tab = skin.get("MenuHeaderSelect_00");
    Skin::Entry value{"value_plate", make_tinted(*element, kValueBlack, kValueWhite)};
    Skin::Entry value_on{"value_plate_on", make_tinted(*element, kValueOnBlack, kValueOnWhite)};
    Skin::Entry tab_sel{"tab_select", make_tinted(*tab, kTabBlack, kTabWhite)};
    skin.images.push_back(std::move(value)), skin.images.push_back(std::move(value_on)), skin.images.push_back(std::move(tab_sel));
    Skin::Entry line{"cursor_line", {}}, glow{"cursor_glow", {}};
    make_cursor(*skin.get("OptionBtnThinCursorLine_00"), *skin.get("OptionBtnThinCursorGlow_00"), &line.img, &glow.img);
    skin.images.push_back(std::move(line)), skin.images.push_back(std::move(glow));
    return true;
}

// ---- (Triforce, 11-19) the title's list. Each of its entries (Continue, New Game, Options, ...) is the part
// PaCommonBtnTitle_00 of UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc, its textures in the Title archive's BNTX,
// all BC4 masks the layout's materials colour. The Mods entry under the list is drawn with them as the game draws its own
// (vk_menu.cpp draw_title_mods): W_Bg_00 the chosen entry's dark band (CtrlGuideBG_00 its shape, fading out at its ends;
// CtrlGuideBGGrd_00 thinner toward its top and bottom), W_Bg_01 the decide's sparkle over it (DecideNoise_01), P_Deco_*_00
// and P_DecoGlow_*_00 the filigree either side (TitleBtnDeco_00, TitleBtnDecoGlow_00, drawn as masks). The band's colour
// and strength are measured from the lab's shot of the game's own chosen entry (runs/pt-menu, 3440 x 1440): (11,7,7) where
// it is solid, about 60% at its top and bottom, no line along its edges (so CtrlGuideBGEdge_00 is not used)
constexpr unsigned char kTitleBandRgb[3] = {7, 4, 4};
constexpr float kTitleBandAlpha = 0.96f, kTitleBandThin = 0.4f;
constexpr float kTitleNoiseAcross = 3.0f;   // (the sparkle's texels square on the band, about 3 times as wide as its texture)
/// The band (W_Bg_00: its colour, its strength from the shape and the gradient) and the decide's sparkle (W_Bg_01: the noise
/// where the band is, a mask) from the three masks, the size of the shape's
inline void make_title_band(const Image& shape, const Image& grd, const Image& noise, Image* band, Image* decide) {
    for (Image* o : {band, decide}) o->w = shape.w, o->h = shape.h, o->srgb = true, o->rgba.assign((size_t)shape.w * shape.h * 4, 255);
    band->mask = false, decide->mask = true;
    for (uint32_t y = 0; y < shape.h; y++)
        for (uint32_t x = 0; x < shape.w; x++) {
            const size_t i = ((size_t)y * shape.w + x) * 4;
            const float a = shape.rgba[i + 3] / 255.0f;
            const float g = x < grd.w && y < grd.h ? grd.rgba[((size_t)y * grd.w + x) * 4 + 3] / 255.0f : 0;
            band->rgba[i] = kTitleBandRgb[0], band->rgba[i + 1] = kTitleBandRgb[1], band->rgba[i + 2] = kTitleBandRgb[2];
            band->rgba[i + 3] = (unsigned char)(a * kTitleBandAlpha * (1 - kTitleBandThin * g) * 255 + 0.5f);
            const uint32_t nx = (uint32_t)(x * kTitleNoiseAcross) % noise.w, ny = y % noise.h;
            decide->rgba[i + 3] = (unsigned char)(a * noise.rgba[((size_t)ny * noise.w + nx) * 4 + 3] + 0.5f);
        }
}
/// The title's textures from the archive's bytes (a BNTX): added to `skin` as "title_band", "title_decide",
/// "TitleBtnDeco_00" and "TitleBtnDecoGlow_00"; false (the skin left as it was) when one is missing
inline bool make_title_skin(std::vector<unsigned char>&& bntx_bytes, Skin& skin) {
    static const char* const names[] = {"CtrlGuideBG_00", "CtrlGuideBGGrd_00", "DecideNoise_01", "TitleBtnDeco_00", "TitleBtnDecoGlow_00"};
    constexpr int kN = (int)(sizeof names / sizeof *names);
    Bntx bntx;
    if (!bntx.parse(std::move(bntx_bytes))) return false;
    Image img[kN];
    for (int i = 0; i < kN; i++) {
        const Bntx::Entry* e = bntx.find(names[i]);
        if (!e || !bntx.decode(*e, img[i]) || !img[i].mask) return false;
    }
    Skin::Entry band{"title_band", {}}, decide{"title_decide", {}};
    make_title_band(img[0], img[1], img[2], &band.img, &decide.img);
    skin.images.push_back(std::move(band)), skin.images.push_back(std::move(decide));
    skin.images.push_back({names[3], std::move(img[3])}), skin.images.push_back({names[4], std::move(img[4])});
    return true;
}

} // namespace mkwvk::game_tex
