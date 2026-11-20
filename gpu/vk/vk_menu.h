// The Insert menu (vk_menu.cpp): options that apply while the game runs, kept in triforce_options.ini beside the executable.
#pragma once

#include "vk_internal.h"
#include "replay_threads.h"   // (the Replay helper threads row: its Automatic and what it is unless set)

namespace mkwvk {

// (out of the box every feature is off: what the game does on its own. The ini beside the executable keeps a player's choices)
struct Options {
    int framegen = 0;     // made frames: 0 off, 1 Auto (to the display's refresh), or 2 to 8 times the game's frames
    int fg_engine = 0; // who makes the made frames: 0 Auto, 1 NVIDIA DLSS-G, 2 AMD (library), 3 Triforce (framegen_engine.h; RX_VK_FRAMEGEN_VENDOR over it; saved as frame_generation_engine)
    int pace = 1;         // frames shown on an even line (vk_framegen.cpp); (Triforce) on by default since options_version 7
    int tail_early = 1;   // (Triforce) Start next frame early (Debug tab, on by default since options_version 6): 1 the game's frame wait ends before its frame's upscaler and interface run on
                          // the GPU (gpu/vk/vk_replay.cpp the early tail, tail_early.h; RX_VK_TAIL_EARLY over it); read at the start
                          // (tail_early=, NX2navi's key)
    int hdr = 0;          // (hookshot) HDR10 out (vk_framegen.cpp framegen_hdr): 0 off, 1 the SDR picture expanded, 2 the game's own (native) HDR; when the display has it (the swapchain made again at once)
    int hdr_white = 200;  // HDR paper white, nits (the picture's white)
    int hdr_peak = 1000;  // HDR peak brightness, nits (its brightest highlights): 400 to 10,000, HDR10's ceiling
    // (Triforce, 11-13: both additions of the 11-13 patches are off until their row turns them on, read once at the start)
    int renodx = 0;       // RenoDX HDR: 0 off, 1 RenoDX's shaders with HDR Native (host/nn_vi.c reads renodx_hdr= at the start)
    int upscale_pre = 0;  // Upscale before bloom: 0 the Upscaler's engine on the finished picture, as before; 1 on the scene
                          // before the game's bloom and tone mapping (vk_temporal.cpp pre_wanted)
    // (local, RenoDX port: vk_renodx.cpp) with HDR Native, RenoDX's shaders in place of the game's tone mapping and HDR output
    int rdx_tonemap = 1;      // 0 Vanilla (the game's curve), 1 PsychoV35, 2 RenoDRT (Triforce: renodx_glue/triforce_scene.slangi)
    // (Triforce: NX2navi's HDR rows, each at the value that leaves the picture as it was before the row. vk_renodx.cpp
    // fill_settings maps them; the rows of one tone mapper show while it is the one chosen, vk_menu.cpp rdx_applies)
    int rdx_gamma = 0;        // the finished picture shown as a display of that gamma would: 0 off (the sRGB curve), 1 2.2, 2 BT.1886 (2.4)
    int rdx_match_slope = 1;  // PsychoV35: its mid tones as steep as the game's own curve; 0: the response's own slope
    int rdx_tone_slope = 50;  // PsychoV35: the tone curve's steepness through middle grey, 0 to 100, 50 unchanged
    int rdx_hue_clip = 50;    // PsychoV35: percent of the hue bend the game's clamp gives bright colours
    int rdx_highlight_sat = 50;   // RenoDRT: the brightest parts' saturation, 0 to 100, 50 unchanged
    int rdx_blowout = 0;      // RenoDRT: the brightest parts towards white, 0 to 100, 0 none
    int rdx_flare = 0;        // RenoDRT: the darkest parts lifted, 0 to 100, 0 none
    int rdx_hue = 100;        // RenoDRT: percent of their hue bright colours keep through the tone mapper
    int rdx_grade = 100;      // RenoDRT: percent of the game's own colour grading the picture keeps
    int rdx_midgray = 1;      // what PsychoV35 holds middle grey to: 0 the render's, 1 vanilla's, 2 the eye adaptation's
    int rdx_ui_white = 200;   // the menus' and HUD's white, nits
    int rdx_space = 1;        // the output's colour space: 0 Vanilla (the game's matrix), 1 BT.709, 2 DCI-P3
    int rdx_exposure = 100;   // percent, 0 to 200
    int rdx_highlights = 50, rdx_shadows = 50, rdx_contrast = 50, rdx_saturation = 50;   // 0 to 100, 50 neutral
    int rdx_bloom = 50;       // 0 to 100, 50 the game's
    int fps_cap = 0;      // (hookshot) Frame rate cap: 0 60 (the console's), 90, 120, 144, 165, 240; -1 45, -2 40, -3 30; 6 the monitor's
                          // refresh rate (fps_cap.h) (host/nn_vi.c reads frame_rate_cap= at the start)
    int steady_fps = 0;   // (Triforce) Steady frame rate: 0 Off, 1 any rate, 2 steps of the display's refresh (host/pace_steady.h; host/nn_vi.c
                          // reads steady_frame_rate= at the start)
    int reflex_limit = 0; // (Triforce) Reflex frame limit: 1 with DLSS frame generation NVIDIA Reflex's sleep holds the frame rate cap where the
                          // game's frame starts and the game's own wait steps aside (vk_dlssg.cpp pace, dlssg_pace.h); 0 the game's own wait, as before
    int resolution = 0;   // the 3D's (Render resolution): 0 Auto (the game's dynamic resolution), else a share of the output,
                          // 1..6 = 100%, 75%, 67%, 50%, 58%, 33% (upscaler_rows.h; nx2/oot_patches.py)
    int output_res = 0;   // the game's whole frame, docked: 0 its 2048x1152, 1 2560x1440, 2 3840x2160, 3 1920x1080, 4 5120x1440, 5 5120x2880, 6 5120x2160; read once, at the start (host/nn_applet.c)
    int upscale = 0;      // to the window (Window scaling filter, saved as upscaling): 0 off (the plain copy), 1 Catmull-Rom, 2 Lanczos, 3 AMD FSR 1 (EASU + RCAS)
    int reflex = 2;       // NVIDIA Reflex: 0 off, 1 on, 2 on + boost (the default since options_version 5) (vk_reflex.cpp; NVIDIA GPUs with VK_NV_low_latency2)
    int antilag = 0;      // AMD Anti-Lag 2: 0 off, 1 on (vk_antilag.cpp; AMD GPUs with VK_AMD_anti_lag)
    int dlss = 0;         // the Upscaler (its name from before FSR: options files and the tests' stand-ins keep it): 0 the game's
                          // own network, 1 NVIDIA DLSS, 2 NVIDIA DLAA (a build with NVIDIA's SDK only), 3 AMD FSR 3.1 (vk_fsr3.cpp),
                          // 4 AMD FSR 4, 5 its native AA (a build with FSR 4 only), 6 FSR 3.1's native AA. upscaler_rows.h;
                          // vk_temporal.cpp. Saved as dlss
    int dlss_model = 0;   // DLSS's network, by its preset: 0 K (transformer), 1 M, 2 L (second-generation transformer), 3 NVIDIA's
                          // default (upscaler_rows.h model_preset; vk_dlss.cpp). RX_VK_DLSS_PRESET over it
    int sharpness = 0;    // 0 to 100
    int fsr_sharpness = 0; // FSR sharpening, 0 (off) to 100: AMD's own RCAS in the library's FSR 3.1 and FSR 4 (fsr_sharpen.h), apart from sharpness
    int aa = 0;           // (Triforce) Anti-aliasing: 0 the game's own SMAA, as before the row; in its place 1 nothing, 2 FXAA, 3 SMAA at
                          // its Ultra preset (aa_rows.h, vk_aa.cpp; RX_VK_AA over it). Not applied while DLSS, DLAA or FSR
                          // upscales. Taken each frame. Saved as anti-aliasing
    int aniso = 0;        // texture filtering: 0 the game's, 1 2x, 2 4x, 3 8x, 4 16x anisotropic at least (vk_draw.cpp sampler_for)
    int tex_detail = 0;   // (Triforce) Texture detail: 0 Automatic (by the card's video memory), 1 Full, 2 Reduced, 3 Low; read at the
                          // start (tex_detail.h, vk_texture.cpp texture_detail_level; RX_VK_TEX_DETAIL over it)
    int tex_bc1 = 1;      // (Triforce) Opaque textures as BC1: 0 Off, 1 On (the default since options_version 5): ASTC textures the renderer keeps as BC3 are kept as BC1,
                          // half the video memory, when every block of them is opaque; read at the start (tex_bc1.h, vk_texture.cpp
                          // bc1_on; RX_VK_TEX_BC1 over it)
    int realtime = 1;     // game time follows the clock (host/nn_vi.c); 0: per frame (slower frames, slower game)
    int handheld = 0;     // the game's handheld profile: ~4 fps, softer; read once, at the start (host/nn_applet.c)
    int ultrawide = 0;    // the camera's shape: 0 the game's 16:9, 1 the monitor's when it is wider or narrower, 2 a fixed 21:9, 3 a fixed
                          // 32:9; read once, at the start (host/hos_main.c ultrawide_ratio)
    int wide_menus = 1;   // ultrawide: the pause, item and map menus 0 at full width (stretched), 1 in the middle 16:9 with the sides
                          // blurred, 2 with black bars (nx2/oot_wide.py, vk_framegen.cpp)
    int show_fps = 0;     // the FPS counter row: 0 off, 1 the counter, 2 (Triforce) Full, NX2navi's full performance overlay in the counter's place:
                          // the machine, the frames, the waits per frame, the settings (vk_menu.cpp draw_perf, perf_stats.h,
                          // perf_machine.h), measured only while shown
    int menu_shortcut = 0;   // 0 Insert or F1, 1 F1 only (F1 always opens the menu)
    int menu_size = 0;   // (hookshot) the overlays' size: 0 Auto, 1 Small, 2 Medium, 3 Large (gpu/vk/vk_ui.h unit)
    int fullscreen = 0;   // borderless, at the monitor's size: F10 and Alt+Enter are the same setting
    int vsync = 1;        // (Triforce) VSync: 1 the swapchain's present mode as before the row (mailbox where the driver has it), 0 immediate
                          // where the surface lists it (present_mode.h, vk_main.cpp create_swapchain; the swapchain made again at once)
    int gpu_device = 0;   // graphics card index: 0 Auto, 1..N device 0..N-1 (gpu/vk/vk_main.cpp; RX_VK_DEVICE)
    int pad_background = 0;   // the controllers count while another window has the focus (host/nn_hid.c; HOS_INPUT_BACKGROUND)
    int mouse_native = 0; // the mouse turns the camera's own angles (host/mouse_native.c); 0: as the right stick
    int mouse_sens = 100; // the mouse's sensitivity in percent: 0.1 degrees a count at 100 with native look (host/nn_hid.c)
    int mouse_invert = 0; // the mouse's up and down swapped
    int text_speed = 0;   // how fast dialogue prints: 0 the game's, 1 2x, 2 4x, 3 instant (host/nn_vi.c, nx2/oot_patches.py; HOS_TEXT_SPEED)
    int invert_cam_x = 0; // right-stick camera horizontal inversion: 0 normal, 1 inverted
    int camera_speed = 100;   // (Triforce) the right stick's camera speed, percent of the game's own (host/prompts.c build_camera)
    int invert_cam_y = 0; // right-stick camera vertical inversion: 0 normal, 1 inverted
    int rumble = 100;     // the controllers' rumble in percent of the game's: 0 Off, 25 to 300 (host/pad_sdl.c)
    int rumble_curve = 100;   // (Triforce) the power on the motors' rumble, in hundredths: 20 to 150, 100 none (host/pad_sdl.c rumble_curve)
    int rumble_triggers = 0;  // (Triforce) the rumble in an Xbox pad's trigger motors too: 0 off (host/pad_sdl.c trigger_poll)
    int stick_dead_zone = 0;  // a stick axis below it is 0: 0 XInput's 7849 (~24%), 1..5 5% to 30% (host/nn_hid.c)
    int lstick_dead_zone = -1;   // (Triforce) the left stick's own dead zone, percent: -1 As Stick dead zone, 0 to 50 (host/nn_hid.c hos_stick_axis_own)
    int rstick_dead_zone = -1;   // (Triforce) the right stick's own
    int walk_key = 0;     // (Triforce) the key that makes the movement keys walk while it is held: 0 Off, 1 Caps Lock, 2 Shift, 3 Ctrl, 4 Tab (host/nn_hid.c walk_key_code)
    int walk_scale = 51;  // (Triforce) how far they push the left stick then, in hundredths of all the way: 20 to 90 (host/nn_hid.c walk_keys)
    int pad_layout = 0;   // 0 Nintendo (default), 1 Xbox / PC, 2 Swap Face Buttons, 3 Custom
    int gyro = 1;         // motion controls from a pad's gyroscope (host/pad_sdl.c, host/hos_motion.h); on by default
    int gyro_sens_x = 100;   // the motion aim's horizontal sensitivity in percent
    int gyro_sens_y = 100;   // the motion aim's vertical sensitivity in percent
    int gyro_inv_x = 0;   // the motion aim's horizontal turned around
    int gyro_inv_y = 0;   // the motion aim's vertical turned around
    int gyro_deadzone = 1;   // the motion's deadzone: 0 tight, 1 standard (the console's default), 2 loose
    int prompts = 0;      // the button icons: 0 Auto (the controller), 1 Switch, 2 Xbox, 3 PS4, 4 PS5, 5 Steam Deck (host/prompts.c),
                          // (Triforce) 6 Keyboard & mouse
    int robust = 1;       // bounds-checked buffer access: 0 Auto (off), 1 Off (the default since 11-17, the owner's), 2 On (gpu/vk/vk_main.cpp; RX_VK_ROBUST)
    int mesh_mode = 0;    // (Triforce, 11-18) the game's mesh-shader draws: 0 Auto (vk_main.cpp's rule), 1 Compute fallback (RX_VK_TASK_MESH_FALLBACK over it)
    int descriptor_reuse = 1;  // (on by default since options_version 4) a draw keeps its descriptors through other slots' registrations (gpu/vk/vk_draw.cpp; RX_VK_DESC_SLOTGEN)
    int sampler_cache = 1;     // (on by default since options_version 5) sampler ids answer their parameters without the registrations' lock (gpu/vk/vk_draw.cpp; RX_VK_SAMPLER_CACHE)
    int fast_pipeline_link = 1;   // (Triforce, the Debug tab; on by default since options_version 5) Fast pipeline link: a new pipeline is linked from its stage libraries first
                               // (gpu/vk/vk_draw.cpp gpl_wanted, gpl_rules.h; RX_VK_GPL over it); read at the start, from the file
    int lazy_write_back = 1;   // (Triforce, the Debug tab; on by default since options_version 5) Lazy write-back: what shaders write into mirrored pools is copied back
                               // to guest memory when it is read (gpu/vk/vk_memory.cpp, wb_lazy_state.h; RX_VK_LAZY_WB over it,
                               // off on AMD's driver unless that says 1, Windows only); read at the start, from the file
                               // (lazy_write-back=, the row's name)
    int replay_threads = 8;    // (Triforce, the Debug tab) Replay helper threads: 0 off, 1 to 7 the parallel replay tried on 2 to 8
                               // threads, what RX_VK_PARALLEL_AUTO=<n> does, 8 Automatic (the default, replay_threads.h
                               // kDefaultRow: that header says why), the count replay_threads.h gives this PC
                               // (replay_threads_of; gpu/vk/vk_replay.cpp par_threads_max); read at the start
    int preset = 0;       // quality preset: 0 Custom, 1 Minimum (6GB), 2 Low (8GB), 3 Balanced (12GB), 4 High (16GB), 5 Ultra (20GB+)
    int vram_mb = 0;      // dynamic texture cache budget limit in MB
    int mirror_mb = 0;    // staging mirror limit in MB
    int audio_output = 0; // (hookshot) the speakers the game mixes for: 0 Auto (the default output's), 1 Stereo, 2 5.1; read once, at the start (host/nn_audio.c; HOS_AUDIO_CHANNELS)
    int volume = 100;  // (Triforce) Master volume, percent of the game's own (host/nn_audio.c render_frame; master_volume= in the ini)
    int menu_sounds = 80;   // (Triforce) Menu sounds: how loud the options' own sounds are, the game's interface sounds, percent,
                            // 0 silent; 80 as NX2navi's menu_sound_volume (host/menu_sound.c; menu_sounds= in the ini)
    int dlssg = 0;           // (Triforce) NVIDIA DLSS frame generation: 0 off, 1 2x, 2 3x, 3 4x; read at the start (vk_dlssg.cpp, dlss_frame_generation=)
    int wide_hud = 0;        // (Triforce) with Ultrawide: 0 the HUD in the middle 16:9, 1 its corner pieces at the screen's edges (host/oot_wide_hud.c)
    int sprint_button = 0;   // (Triforce) the sprint button: 0 off, 1 the left stick's click, 2 B (host/oot_sprint.h)
    int sprint_mode = 0;     // 0 toggle (a press starts it, the next ends it), 1 hold
    int discord = 1;   // (hookshot) the Discord status (host/discord_rpc.c): 1 on, shown while a Discord client runs on this PC
    int language = 0;  // (Triforce) Game language: the row's entry among the languages language.txt names (game_language.h). Not kept in
                       // the options file: the choice is that file's line, which the host reads at the start (host/nn_applet.c)
    int debug_tools = 0;   // (hookshot) the Debug tab's tools (#785: flight, free camera, teleport, inventory): 0 off; HOS_OOT_CHEATS=1 also turns them on
    int mic = 1;          // (hookshot) the ocarina's microphone: 0 Off, 1 the system's default, 2.. a device as host->mic_name lists it (host/nn_audio_in.c)
};
extern Options opts;
/// (local, RenoDX port) opts.hdr as triforce_options.ini had it at the start (menu_init), before any change in play: what
/// host/nn_vi.c hos_scene_rgba16f decided the scene's format from (vk_temporal.cpp temporal_scene_format)
extern int hdr_at_start;
/// (Triforce) opts.renodx and opts.upscale_pre as the file had them at the start: what this run does (RX_VK_RENODX and
/// RX_VK_DLSS_PRE, 0 or 1, over them)
extern int renodx_at_start, upscale_pre_at_start;
/// Whether RenoDX's shaders run in this session: the RenoDX HDR row as it was at the start, or RX_VK_RENODX
bool renodx_chosen();
/// (Triforce) opts.replay_threads as the file had it at the start, and the threads a value of that row stands for: 0 none,
/// else one more than the value (the row has no "1 thread": that is the replay thread alone, off). RX_VK_PARALLEL_AUTO and
/// RX_VK_PARALLEL go over the row (vk_replay.cpp par_threads_max). The row's 8, Automatic, is not one of these: its count
/// is the PC's (replay_threads.h automatic, vk_replay.cpp replay_threads_automatic)
extern int replay_threads_at_start;
constexpr int replay_threads_of(int row) { return row >= 1 && row <= 7 ? row + 1 : 0; }
/// (Triforce) opts.tex_detail as the file had it at the start: what this run's textures are made with (vk_texture.cpp
/// texture_detail_level; RX_VK_TEX_DETAIL over it)
extern int tex_detail_at_start;
/// (Triforce) opts.tex_bc1 as the file had it at the start: whether this run keeps opaque ASTC textures as BC1
/// (vk_texture.cpp bc1_on; RX_VK_TEX_BC1 over it)
extern int tex_bc1_at_start;
/// (vk_memory.cpp) true when the quality preset's memory budgets (vram_mb, mirror_mb) are to be used: the card has no more
/// video memory than the preset is for. A 4 GB preset (Minimum before 11-15) on an 8 GB card kept 256 MB of mirrors and 2.2 GB of textures: the
/// game's memory was read over PCIe and the textures made again every frame, 5 fps with the CPU idle (Discord, 11-08)
bool preset_caps_memory();

void menu_init();             // (the options from the file, once)
void menu_set_gpu_count(int count);
void menu_tick();             // (each frame shown: the keys, while the game's window has the focus)
bool menu_is_open();
bool resolution_auto();       // (the game's dynamic resolution: Resolution Auto)
int resolution_scale();       // (the fixed path's scale in 1/10000: the Resolution row's, as a twelfth of the output)
void menu_option_set(int id, int value);   // (from the game's side: 7 the mouse's sensitivity, the - and = keys)
int menu_heart_protection();
int menu_player_flight();
int menu_free_camera();
int menu_take_cheat_action();
/// (Triforce) What a row of the System tab asked the host to do since the last call: HOS_SHARE_* (vk_api.h), 0 nothing
int menu_take_share_ask();
/// (Triforce) The next byte of a line the Debug tab's name pages wait to send (gpu/vk/oot_debug_link.h), a line feed at a
/// line's end, 0 while none waits: the Game flags' and the Cutscenes' (HOS_OPT_DEBUG_ASK), and the Scan amiibo page's file
/// name (HOS_OPT_AMIIBO_ASK)
int menu_take_debug_ask();
int menu_take_amiibo_ask();
/// (Triforce) A cheat of a mod's cheat file the player switched on the System tab since the last call: (its row + 1) * 2, plus 1
/// for on; 0 nothing (vk_api.h HOS_OPT_MODCHEAT_ASK, mod_cheat_rows.h)
int menu_take_mod_cheat();
/// (Triforce) The next of the options' own sounds asked for since the last call, HOS_MENU_SOUND_* (vk_api.h), 0 none: for the
/// host's sound thread alone (HOS_OPT_MENU_SOUND; menu_sound_events.h's queue, host/menu_sound.c)
int menu_take_sound();
/// (Triforce) 1 while the Mods entry under a game list has the cursor, or its screen is open (HOS_OPT_MODS_SELECTED: the
/// host lets the game's own cursor go then, host/oot_menu.c)
int menu_mods_selected();
/// (Triforce) 1 while the Mods screen's search takes typed text (HOS_OPT_TEXT_WANTED: host/display.c sends it then)
int menu_text_wanted();
/// (Triforce) What became of the picture a bug capture asked for (vk_bugshot.h bugshot_state): 0 none asked, 1 not there yet,
/// 2 written, 3 no picture
int menu_bug_shot();
void menu_note_frame(bool game_frame, int shown = 1);   // (the frame rates the FPS counter shows)
/// The game's picture's size (the Output resolution, its crop) and the window's, at each present: the Window scaling filter
/// row says when it is not needed
void menu_note_present_sizes(uint32_t picture_w, uint32_t picture_h, uint32_t window_w, uint32_t window_h);
/// Over the window's image `index` (TRANSFER_DST_OPTIMAL before and after), w x h: the start-up screen, the menu and the FPS
/// counter, drawn with Dear ImGui (vk_imgui.h, vk_ui.h). Nothing recorded while none of them shows
void menu_record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h);
/// (headless shots, RX_VK_SHOT_MENU) The menu and the counter over another image: TRANSFER_DST_OPTIMAL before and after,
/// R8G8B8A8_UNORM with colour-attachment use, view a view of it
void menu_record_image(VkCommandBuffer cmd, VkImage image, VkImageView view, uint32_t w, uint32_t h);
/// (hookshot) The start-up screen while the shaders are prepared (vk_main.cpp, vk_progress.h), drawn by menu_record under the
/// counter and the menu. Null takes it away.
struct StartupView;
void startup_screen(const StartupView* view);
/// The start-up screen's fade to black as it ends: 0 none, 1 black (vk_main.cpp prepare_with_screen)
void startup_fade_to(float f);

} // namespace mkwvk
