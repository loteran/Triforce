#include "../gpu/vk/upscaler_rows.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    namespace ur = mkwvk::upscaler_rows;
    using ur::DLAA;
    using ur::DLSS;
    using ur::GAME;
    // (the values saved before keep their shares; the new ones are DLSS's ratios, each a twelfth of the output)
    CHECK("saved values keep their shares", ur::percent(0) == 0 && ur::percent(1) == 100 && ur::percent(2) == 75 &&
                                                ur::percent(3) == 67 && ur::percent(4) == 50);
    CHECK("Balanced and Ultra Performance", ur::percent(5) == 58 && ur::percent(6) == 33);
    CHECK("a value past the row is Auto", ur::percent(7) == 0 && ur::percent(-1) == 0);
    for (int v = 1; v < ur::kValues; v++) {
        const int k = (int)(ur::percent(v) * 12 / 100.0 + 0.5);
        CHECK("each share is a twelfth", k * 100 / 12 == ur::percent(v) || (k * 100 + 11) / 12 == ur::percent(v));
    }
    CHECK("DLSS's mode names", !std::strcmp(ur::dlss_mode_name(3), "Quality") && !std::strcmp(ur::dlss_mode_name(5), "Balanced") &&
                                   !std::strcmp(ur::dlss_mode_name(4), "Performance") &&
                                   !std::strcmp(ur::dlss_mode_name(6), "Ultra Performance"));
    CHECK("no mode name for Auto, 100% or 75%", !ur::dlss_mode_name(0) && !ur::dlss_mode_name(1) && !ur::dlss_mode_name(2));

    // the game's own: Auto, 100, 75, 67, 50, as before
    CHECK("game: Right", ur::step(GAME, 0, 1) == 1 && ur::step(GAME, 1, 1) == 2 && ur::step(GAME, 3, 1) == 4 && ur::step(GAME, 4, 1) == 0);
    CHECK("game: Left", ur::step(GAME, 0, -1) == 4 && ur::step(GAME, 1, -1) == 0 && ur::step(GAME, 4, -1) == 3);
    // DLSS: Auto, Quality 67, Balanced 58, Performance 50, Ultra Performance 33
    CHECK("dlss: Right", ur::step(DLSS, 0, 1) == 3 && ur::step(DLSS, 3, 1) == 5 && ur::step(DLSS, 5, 1) == 4 &&
                             ur::step(DLSS, 4, 1) == 6 && ur::step(DLSS, 6, 1) == 0);
    CHECK("dlss: Left", ur::step(DLSS, 0, -1) == 6 && ur::step(DLSS, 3, -1) == 0 && ur::step(DLSS, 6, -1) == 4);
    // a value that is not a step (saved with the other upscaler): its neighbours by size
    CHECK("dlss from 75%", ur::step(DLSS, 2, 1) == 3 && ur::step(DLSS, 2, -1) == 0);
    CHECK("dlss from 100%", ur::step(DLSS, 1, 1) == 3 && ur::step(DLSS, 1, -1) == 0);
    CHECK("game from 58%", ur::step(GAME, 5, 1) == 4 && ur::step(GAME, 5, -1) == 3);
    CHECK("game from 33%", ur::step(GAME, 6, 1) == 0 && ur::step(GAME, 6, -1) == 4);
    CHECK("DLAA does not step", ur::step(DLAA, 3, 1) == 3 && ur::step(DLAA, 0, -1) == 0);

    // another upscaler: the nearest step (the larger on a tie), Auto stays Auto
    CHECK("snap to DLSS", ur::snap(DLSS, 0) == 0 && ur::snap(DLSS, 1) == 3 && ur::snap(DLSS, 2) == 3 && ur::snap(DLSS, 3) == 3 &&
                              ur::snap(DLSS, 4) == 4);
    CHECK("snap to the game's", ur::snap(GAME, 5) == 4 && ur::snap(GAME, 6) == 4 && ur::snap(GAME, 3) == 3 && ur::snap(GAME, 1) == 1);
    CHECK("DLAA keeps the value", ur::snap(DLAA, 5) == 5 && ur::snap(DLAA, 1) == 1);
    CHECK("a value past the row snaps to Auto", ur::snap(GAME, 9) == 0 && ur::snap(DLSS, -3) == 0);

    // the Upscaler row: all three where DLSS runs, the game's own alone elsewhere
    CHECK("upscaler with DLSS", ur::step_upscaler(0, 1, true) == 1 && ur::step_upscaler(1, 1, true) == 2 &&
                                    ur::step_upscaler(2, 1, true) == 0 && ur::step_upscaler(0, -1, true) == 2);
    CHECK("upscaler without DLSS", ur::step_upscaler(0, 1, false) == 0 && ur::step_upscaler(2, -1, false) == 0);
    CHECK("upscaler from a bad value", ur::step_upscaler(7, 1, true) == 1);

    // the Upscaler row only in a build with DLSS; the rows with nothing to choose
    CHECK("upscaler row shown", ur::upscaler_shown(true) && !ur::upscaler_shown(false));
    // AMD FSR 3.1 on the same row, after DLAA: stepping visits only what runs here, and wraps
    {
        const unsigned all = ur::bit(GAME) | ur::dlss_bits(true) | ur::bit(ur::FSR3);
        const unsigned fsr3 = ur::bit(GAME) | ur::bit(ur::FSR3);
        CHECK("FSR 3.1 after DLAA", ur::FSR3 == 3);
        CHECK("upscaler steps through all four", ur::step_upscaler(DLAA, 1, all) == ur::FSR3 &&
                                                   ur::step_upscaler(ur::FSR3, 1, all) == GAME &&
                                                   ur::step_upscaler(GAME, -1, all) == ur::FSR3 &&
                                                   ur::step_upscaler(ur::FSR3, -1, all) == DLAA);
        CHECK("upscaler without DLSS skips it", ur::step_upscaler(GAME, 1, fsr3) == ur::FSR3 &&
                                                  ur::step_upscaler(ur::FSR3, 1, fsr3) == GAME &&
                                                  ur::step_upscaler(GAME, -1, fsr3) == ur::FSR3);
        CHECK("an upscaler that cannot run here steps as the game's own", ur::step_upscaler(DLSS, 1, fsr3) == ur::FSR3 &&
                                                                            ur::step_upscaler(ur::FSR3, 1, ur::bit(GAME)) == GAME);
        CHECK("the DLSS-only form is the mask's", ur::step_upscaler(DLAA, 1, true) == GAME && ur::step_upscaler(GAME, -1, true) == DLAA);
        CHECK("upscaler fixed only with nothing but the game's own",
              ur::upscaler_fixed(ur::bit(GAME)) && !ur::upscaler_fixed(fsr3) && !ur::upscaler_fixed(all));
        CHECK("upscaler row shown with FSR 3.1 alone", ur::upscaler_shown(false, true) && !ur::upscaler_shown(false, false) &&
                                                          ur::upscaler_shown(true, false));
        CHECK("FSR 3.1's modes are DLSS's shares", ur::scales_by_mode(ur::FSR3) && ur::scales_by_mode(DLSS) &&
                                                     !ur::scales_by_mode(DLAA) && !ur::scales_by_mode(GAME));
        CHECK("FSR 3.1 steps as DLSS does", ur::step(ur::FSR3, 0, 1) == 3 && ur::step(ur::FSR3, 3, 1) == 5 &&
                                              ur::step(ur::FSR3, 6, 1) == 0 && ur::snap(ur::FSR3, 2) == 3);
        CHECK("the model row waits with FSR 3.1", ur::model_fixed(ur::FSR3, false) && !ur::model_fixed(DLSS, false) &&
                                                    !ur::model_fixed(DLAA, false));
        CHECK("FSR 3.1 has a Render resolution", !ur::resolution_fixed(ur::FSR3));
    }
    // AMD FSR 4 and its native AA after FSR 3.1 (saved values keep their meaning), on the same row and mask
    {
        using ur::FSR3;
        using ur::FSR4;
        using ur::FSR4_AA;
        const unsigned every = ur::bit(GAME) | ur::dlss_bits(true) | ur::bit(FSR3) | ur::fsr4_bits(true);
        const unsigned fsr4 = ur::bit(GAME) | ur::fsr4_bits(true);
        CHECK("FSR 4 after FSR 3.1", FSR4 == 4 && FSR4_AA == 5);
        CHECK("upscaler steps through all six", ur::step_upscaler(FSR3, 1, every) == FSR4 &&
                                                  ur::step_upscaler(FSR4, 1, every) == FSR4_AA &&
                                                  ur::step_upscaler(FSR4_AA, 1, every) == GAME &&
                                                  ur::step_upscaler(GAME, -1, every) == FSR4_AA &&
                                                  ur::step_upscaler(FSR4, -1, every) == FSR3);
        CHECK("upscaler with FSR 4 alone", ur::step_upscaler(GAME, 1, fsr4) == FSR4 && ur::step_upscaler(FSR4_AA, 1, fsr4) == GAME &&
                                               ur::step_upscaler(GAME, -1, fsr4) == FSR4_AA && !ur::upscaler_fixed(fsr4));
        CHECK("a saved FSR 4 where it cannot run steps as the game's own",
              ur::step_upscaler(FSR4, 1, ur::bit(GAME) | ur::dlss_bits(true)) == DLSS &&
              ur::step_upscaler(FSR4_AA, -1, ur::bit(GAME) | ur::bit(FSR3)) == FSR3);
        CHECK("engines", ur::is_dlss(DLSS) && ur::is_dlss(DLAA) && !ur::is_dlss(FSR3) && !ur::is_dlss(FSR4) &&
                             ur::is_fsr4(FSR4) && ur::is_fsr4(FSR4_AA) && !ur::is_fsr4(FSR3) && !ur::is_fsr4(DLAA) &&
                             !ur::is_fsr4(GAME));
        CHECK("full scale", ur::full_scale(DLAA) && ur::full_scale(FSR4_AA) && !ur::full_scale(FSR3) && !ur::full_scale(FSR4) &&
                                !ur::full_scale(DLSS) && !ur::full_scale(GAME));
        CHECK("FSR 4's modes are DLSS's shares", ur::scales_by_mode(FSR4) && !ur::scales_by_mode(FSR4_AA));
        for (int v = 0; v < ur::kValues; v++)
            for (int dir : {1, -1})
                CHECK("FSR 4 steps as DLSS", ur::step(FSR4, v, dir) == ur::step(DLSS, v, dir) && ur::snap(FSR4, v) == ur::snap(DLSS, v));
        CHECK("FSR 4's native AA does not step", ur::step(FSR4_AA, 3, 1) == 3 && ur::snap(FSR4_AA, 5) == 5);
        CHECK("the model row waits with FSR 4", ur::model_fixed(FSR4, false) && ur::model_fixed(FSR4_AA, false));
    }
    // AMD FSR 3.1's native AA: the value after the ones saved before, shown after FSR 3.1, the 3D at full size
    {
        using ur::FSR3;
        using ur::FSR3_AA;
        using ur::FSR4;
        using ur::FSR4_AA;
        const unsigned every = ur::bit(GAME) | ur::dlss_bits(true) | ur::fsr3_bits(true) | ur::fsr4_bits(true);
        const unsigned fsr3 = ur::bit(GAME) | ur::fsr3_bits(true);
        CHECK("FSR 3.1's native AA after FSR 4's", FSR3_AA == 6 && ur::kUpscalers == 7);
        CHECK("FSR 3.1's two run together", ur::fsr3_bits(true) == (ur::bit(FSR3) | ur::bit(FSR3_AA)) && ur::fsr3_bits(false) == 0u);
        // the row's order: each engine's native AA after it
        const int order[] = {GAME, DLSS, DLAA, FSR3, FSR3_AA, FSR4, FSR4_AA};
        for (int i = 0; i < 7; i++) {
            CHECK("the row's order, Right", ur::step_upscaler(order[i], 1, every) == order[(i + 1) % 7]);
            CHECK("the row's order, Left", ur::step_upscaler(order[i], -1, every) == order[(i + 6) % 7]);
        }
        unsigned seen = 0;
        for (int v : ur::kOrder) seen |= ur::bit(v);
        CHECK("every value has its place in the order", seen == (1u << ur::kUpscalers) - 1 && ur::kOrder[0] == GAME);
        CHECK("with FSR 3.1 alone", ur::step_upscaler(GAME, 1, fsr3) == FSR3 && ur::step_upscaler(FSR3, 1, fsr3) == FSR3_AA &&
                                        ur::step_upscaler(FSR3_AA, 1, fsr3) == GAME && ur::step_upscaler(GAME, -1, fsr3) == FSR3_AA &&
                                        ur::step_upscaler(FSR3_AA, -1, fsr3) == FSR3 && !ur::upscaler_fixed(fsr3));
        CHECK("a saved FSR 3.1 native AA where it cannot run steps as the game's own",
              ur::step_upscaler(FSR3_AA, 1, ur::bit(GAME) | ur::dlss_bits(true)) == DLSS &&
              ur::step_upscaler(FSR3_AA, -1, ur::bit(GAME) | ur::fsr4_bits(true)) == FSR4_AA &&
              ur::step_upscaler(FSR3_AA, 1, ur::bit(GAME)) == GAME);
        CHECK("FSR 3.1's engine", ur::is_fsr3(FSR3) && ur::is_fsr3(FSR3_AA) && !ur::is_fsr3(FSR4) && !ur::is_fsr3(FSR4_AA) &&
                                      !ur::is_fsr3(DLAA) && !ur::is_fsr3(GAME) && !ur::is_fsr4(FSR3_AA) && !ur::is_dlss(FSR3_AA));
        CHECK("FSR 3.1's native AA is at full scale", ur::full_scale(FSR3_AA) && ur::resolution_fixed(FSR3_AA) &&
                                                          !ur::scales_by_mode(FSR3_AA) && !ur::full_scale(FSR3));
        CHECK("FSR 3.1's native AA does not step", ur::step(FSR3_AA, 3, 1) == 3 && ur::step(FSR3_AA, 0, -1) == 0 &&
                                                       ur::snap(FSR3_AA, 5) == 5 && ur::snap(FSR3_AA, 0) == 0);
        CHECK("the model row waits with FSR 3.1's native AA", ur::model_fixed(FSR3_AA, false));
        // (from Native AA back to FSR 3.1: the Render resolution comes back as one of its modes)
        CHECK("back to FSR 3.1's modes", ur::snap(FSR3, 1) == 3 && ur::snap(FSR3, 4) == 4);
    }
    CHECK("upscaler fixed where DLSS cannot run", ur::upscaler_fixed(false) && !ur::upscaler_fixed(true));
    CHECK("render resolution fixed at full scale only", ur::resolution_fixed(DLAA) && ur::resolution_fixed(ur::FSR4_AA) &&
                                                             ur::resolution_fixed(ur::FSR3_AA) && !ur::resolution_fixed(ur::FSR3) &&
                                                             !ur::resolution_fixed(DLSS) && !ur::resolution_fixed(ur::FSR4) &&
                                                             !ur::resolution_fixed(GAME));
    // ((Triforce, options_version 7) the window's filter, the Window scaling filter row's choice made by the sizes: none at the
    // picture's own size (or before the sizes are known), AMD FSR 1 enlarging, the plain copy making smaller or another
    // shape, and the plain copy with HDR Native, whose 10-bit picture framegen_scale's 8-bit images would cut)
    CHECK("no filter at the picture's size", ur::auto_filter(2560, 1440, 2560, 1440) == 0 && ur::auto_filter(3413, 1440, 3413, 1440) == 0);
    CHECK("no filter before the sizes are known", ur::auto_filter(0, 0, 0, 0) == 0 && ur::auto_filter(2560, 1440, 0, 0) == 0 &&
                                                      ur::auto_filter(0, 0, 2560, 1440) == 0);
    CHECK("FSR 1 enlarging", ur::auto_filter(2048, 1152, 2560, 1440) == 3 && ur::auto_filter(1536, 864, 3840, 2160) == 3 &&
                                 ur::auto_filter(2560, 1440, 2560, 2160) == 3);
    CHECK("the plain copy making smaller", ur::auto_filter(3840, 2160, 2560, 1440) == 0 && ur::auto_filter(2560, 1440, 1920, 1440) == 0 &&
                                              ur::auto_filter(2560, 1080, 1920, 1440) == 0 && ur::auto_filter(2048, 1152, 1280, 720) == 0);
    CHECK("the plain copy with HDR Native", ur::window_filter(2560, 1440, 3360, 1440, true) == 0 &&
                                                ur::window_filter(2048, 1152, 3840, 2160, true) == 0 &&
                                                ur::window_filter(2560, 1440, 3360, 1440, false) == 3);
    // ((Triforce, options_version 7) the Upscaler row names the engine, and an engine's native AA is the Render resolution's
    // Native (100%): the saved values 2, 5 and 6 stay what they were)
    {
        using ur::FSR3;
        using ur::FSR3_AA;
        using ur::FSR4;
        using ur::FSR4_AA;
        const unsigned every = ur::bit(GAME) | ur::dlss_bits(true) | ur::fsr3_bits(true) | ur::fsr4_bits(true);
        CHECK("engines", ur::engine(DLAA) == DLSS && ur::engine(FSR4_AA) == FSR4 && ur::engine(FSR3_AA) == FSR3 &&
                             ur::engine(DLSS) == DLSS && ur::engine(GAME) == GAME && ur::engine(-1) == GAME && ur::engine(9) == GAME);
        CHECK("native AAs", ur::native_of(DLSS) == DLAA && ur::native_of(FSR4) == FSR4_AA && ur::native_of(FSR3) == FSR3_AA &&
                                ur::native_of(GAME) == GAME);
        // the Upscaler: Game, DLSS, FSR 3.1, FSR 4 and back, the resolution left to snap
        ur::Scaling s = ur::step_engine(GAME, 3, 1, every);
        CHECK("game to DLSS", s.upscaler == DLSS && s.resolution == 3);
        s = ur::step_engine(DLSS, 4, 1, every);
        CHECK("DLSS to FSR 3.1", s.upscaler == FSR3 && s.resolution == 4);
        s = ur::step_engine(FSR3, 6, 1, every);
        CHECK("FSR 3.1 to FSR 4", s.upscaler == FSR4 && s.resolution == 6);
        s = ur::step_engine(FSR4, 6, 1, every);
        CHECK("FSR 4 to the game's", s.upscaler == GAME && s.resolution == 6);
        s = ur::step_engine(GAME, 0, -1, every);
        CHECK("the game's to FSR 4, Left", s.upscaler == FSR4 && s.resolution == 0);
        // Native (100%) goes with it: DLAA to FSR 3.1's native AA, FSR 4's to the game's own at 100%, and back
        s = ur::step_engine(DLAA, 3, 1, every);
        CHECK("DLAA to FSR 3.1's native AA", s.upscaler == FSR3_AA && s.resolution == 3);
        s = ur::step_engine(FSR4_AA, 6, 1, every);
        CHECK("FSR 4's native AA to the game's at 100%", s.upscaler == GAME && s.resolution == 1);
        s = ur::step_engine(GAME, 1, 1, every);
        CHECK("the game's at 100% to DLAA", s.upscaler == DLAA && s.resolution == 1);
        // only what runs here; a saved engine that cannot run here steps as the game's own
        s = ur::step_engine(GAME, 4, 1, ur::bit(GAME) | ur::fsr4_bits(true));
        CHECK("FSR 4 alone", s.upscaler == FSR4);
        s = ur::step_engine(FSR4_AA, 4, 1, ur::bit(GAME) | ur::fsr4_bits(true));
        CHECK("FSR 4 alone, back", s.upscaler == GAME && s.resolution == 1);
        s = ur::step_engine(DLSS, 4, 1, ur::bit(GAME) | ur::fsr3_bits(true));
        CHECK("a saved DLSS where it cannot run", s.upscaler == FSR3);
        // the Render resolution: the game's own steps as before; DLSS and FSR Auto, Native (100%), Quality ... Ultra Performance
        s = ur::step_resolution(GAME, 0, 1);
        CHECK("the game's own: Auto to 100%", s.upscaler == GAME && s.resolution == 1);
        s = ur::step_resolution(FSR4, 0, 1);
        CHECK("FSR 4: Auto to Native", s.upscaler == FSR4_AA && s.resolution == 0);
        s = ur::step_resolution(FSR4_AA, 0, 1);
        CHECK("FSR 4: Native to Quality", s.upscaler == FSR4 && s.resolution == 3);
        s = ur::step_resolution(DLAA, 4, -1);
        CHECK("DLSS: Native to Auto", s.upscaler == DLSS && s.resolution == 0);
        s = ur::step_resolution(DLSS, 3, -1);
        CHECK("DLSS: Quality to Native", s.upscaler == DLAA && s.resolution == 3);
        s = ur::step_resolution(FSR3, 6, 1);
        CHECK("FSR 3.1: Ultra Performance wraps to Auto", s.upscaler == FSR3 && s.resolution == 0);
        s = ur::step_resolution(FSR3, 0, -1);
        CHECK("FSR 3.1: Auto wraps to Ultra Performance", s.upscaler == FSR3 && s.resolution == 6);
        s = ur::step_resolution(FSR4, 4, 1);
        CHECK("FSR 4: Performance to Ultra Performance", s.upscaler == FSR4 && s.resolution == 6);
        s = ur::step_resolution(DLSS, 2, 1);
        CHECK("DLSS from the game's 75%", s.upscaler == DLSS && s.resolution == 3);
        CHECK("the 3D at full size", ur::native_size(DLAA, 3) && ur::native_size(GAME, 1) && !ur::native_size(GAME, 2) &&
                                         !ur::native_size(DLSS, 1));
    }
    // the DLSS model row: its presets (NVIDIA's numbers, K = 11), RX_VK_DLSS_PRESET over it, and when it applies
    CHECK("model presets", ur::model_preset(ur::MODEL_K) == 11 && ur::model_preset(ur::MODEL_M) == 13 &&
                               ur::model_preset(ur::MODEL_L) == 12 && ur::model_preset(ur::MODEL_DEFAULT) == 0);
    CHECK("model: a value past the row is K", ur::model_preset(-1) == 11 && ur::model_preset(ur::kModels) == 11);
    CHECK("model without the switch", ur::dlss_preset(ur::MODEL_M, nullptr) == 13 && ur::dlss_preset(ur::MODEL_DEFAULT, nullptr) == 0);
    CHECK("RX_VK_DLSS_PRESET wins", ur::dlss_preset(ur::MODEL_M, "K") == 11 && ur::dlss_preset(ur::MODEL_K, "m") == 13 &&
                                        ur::dlss_preset(ur::MODEL_K, "J") == 10 && ur::dlss_preset(ur::MODEL_M, "0") == 0);
    CHECK("RX_VK_DLSS_PRESET not a preset", !ur::preset_env(nullptr) && !ur::preset_env("") && !ur::preset_env("1") &&
                                                !ur::preset_env("05") && ur::dlss_preset(ur::MODEL_M, "7") == 13 &&
                                                ur::dlss_preset(ur::MODEL_L, "") == 12);
    CHECK("preset letters", ur::preset_letter(11) == 'K' && ur::preset_letter(13) == 'M' && ur::preset_letter(0) == '0');
    CHECK("model row shown with DLSS built", ur::model_shown(true) && !ur::model_shown(false));
    CHECK("model row fixed with the game's own or the switch", ur::model_fixed(GAME, false) && !ur::model_fixed(DLSS, false) &&
                                                               !ur::model_fixed(DLAA, false) && ur::model_fixed(DLSS, true));
    // the FSR sharpening row: AMD's library's FSR 4 (both) and FSR 3.1 only where the library draws it
    CHECK("FSR sharpening used by FSR 4 and its native AA", ur::fsr_sharpen_used(ur::FSR4, false) && ur::fsr_sharpen_used(ur::FSR4_AA, false));
    CHECK("FSR sharpening used by FSR 3.1 through the library only", ur::fsr_sharpen_used(ur::FSR3, true) && !ur::fsr_sharpen_used(ur::FSR3, false));
    CHECK("FSR sharpening waits with the game's own, DLSS and DLAA", ur::fsr_sharpen_fixed(GAME, true) && ur::fsr_sharpen_fixed(DLSS, true) &&
                                                                       ur::fsr_sharpen_fixed(DLAA, true));
    CHECK("FSR sharpening row shown where AMD's library can run", ur::fsr_sharpen_shown(true, false) && ur::fsr_sharpen_shown(false, true) &&
                                                                    !ur::fsr_sharpen_shown(false, false));
    std::puts("upscaler rows: all checks passed");
}
