/* The Vulkan back end (mkw_vk.dll) and the game executable: what each gives the other.

   The executable records the game's NVN command buffers (host/nvn_native.c) and calls a renderer through the rx_*
   functions (gpu/render/rx_nvn.h). With RX_BACKEND=vulkan host/rx_backend.c loads mkw_vk.dll, hands it the host
   functions below and routes the rx_* calls to the table it returns; otherwise they go to the WebGPU renderer in
   the executable. The DLL carries Eden's shader recompiler (gpu/vk/eden, SPIR-V back end) and sirit: their C++
   namespaces are the same as those of the WebGPU renderer's suyu copy, so the two live in different binaries. */
#pragma once

#include <stdint.h>

#include "../render/rx_nvn.h"   /* (the NVN objects and command records: C) */

#ifdef __cplusplus
extern "C" {
#endif

#define MKWVK_API_VERSION 13
enum { HOS_OPT_CHEAT_HEARTS = 32, HOS_OPT_CHEAT_FLIGHT, HOS_OPT_CHEAT_ACTION, HOS_OPT_CHEAT_CAMERA };
/* (hookshot) the Motion page's rows (host/pad_sdl.c reads them, vk_replay.cpp's option answers them): Motion controls on
   or off, the horizontal's and the vertical's sensitivity in percent, each turned around, and the deadzone (0 to 2) */
enum { HOS_OPT_GYRO = 18, HOS_OPT_GYRO_SENS_X, HOS_OPT_GYRO_INV_X, HOS_OPT_GYRO_INV_Y, HOS_OPT_GYRO_SENS_Y, HOS_OPT_GYRO_DEADZONE };
/* (hookshot) NVIDIA DLSS's camera jitter (host/rx_backend.c, gpu/vk/vk_dlss.cpp): the scene's width and height while DLSS
   draws, 0 when there is to be no jitter, and the jitter sequence's length. 24 on: 18 to 23 are the motion rows' */
enum { HOS_OPT_DLSS_JITTER_W = 24, HOS_OPT_DLSS_JITTER_H, HOS_OPT_DLSS_JITTER_PHASES };
/* (hookshot) the Insert menu's HDR set to Native (host/nn_vi.c QueryDisplayHdrStatus and QueryDisplayHdrPaperWhite): whether
   the game is told its display is HDR10 (the swapchain is one), and the peak and paper white it is told, in nits */
/* (HOS_OPT_DISCORD, 43: the Insert menu's Discord status row, host/discord_rpc.c: 1 on) */
/* (HOS_OPT_SPRINT, 45, and HOS_OPT_SPRINT_HOLD, 46: the Sprint button row (0 off, 1 the left stick's click, 2 B) and
   the Sprint mode row (0 toggle, 1 hold), host/oot_sprint.c) */
/* (HOS_OPT_VOLUME, 44: the Insert menu's Master volume row, host/nn_audio.c: percent of the game's own, 0 to 100) */
enum { HOS_OPT_HDR_NATIVE = 40, HOS_OPT_HDR_PEAK, HOS_OPT_HDR_WHITE, HOS_OPT_DISCORD, HOS_OPT_VOLUME, HOS_OPT_SPRINT, HOS_OPT_SPRINT_HOLD };
/* (Triforce) option_set only, each frame the game's own Options screen runs (host/oot_menu.c): the menu then draws its tabs
   over that screen's header, the game's own list the first of them (gpu/vk/vk_menu.cpp game_options_shown) */
/* (Triforce) HOS_OPT_EXITING, option_set only: the process is about to exit (host/hos_relaunch.c): what must be shut
   down in order before the DLLs are unloaded is (NVIDIA's Streamline, gpu/vk/vk_dlssg.cpp) */
enum { HOS_OPT_GAME_OPTIONS = 48, HOS_OPT_EXITING };
/* (Triforce) HOS_OPT_DISPLAY_CAP: the frame rate cap the display the window is on gives, for the Insert menu's Game frame
   rate cap on Monitor's rate (host/nn_vi.c fps_max, gpu/vk/fps_cap.h): its refresh rate rounded, 30 to 240; 0 when the
   rate cannot be read. 60 to 69 are the display, graphics and system rows' */
enum { HOS_OPT_DISPLAY_CAP = 60 };
/* (Triforce) The System tab's save rows (gpu/vk/vk_menu.cpp), whose work the host does on a thread of its own
   (host/save_share.c): HOS_OPT_SHARE_ASK, option: what a row asked for since the last read, HOS_SHARE_*, 0 nothing (taken:
   the next read answers 0). HOS_OPT_SHARE_TEXT, option_set: the next byte of a line for the menu, UTF-8, and
   HOS_OPT_SHARE_SAID, option_set: the bytes sent so far are the line the row of that HOS_SHARE_* shows under its help
   (HOS_SHARE_NOTICE: the line at the foot of the screen for a moment, while the options are closed).
   HOS_OPT_SHARE_IMPORT, option_set: 1 while an imported save waits for the next start (the Import row then cancels it).
   The bug capture (host/bug_capture.c, F8 or its row: HOS_SHARE_CAPTURE; HOS_SHARE_CAPTURE_KEY is F8 where the window
   has no key handler of the host's, Linux) asks for its picture with HOS_OPT_BUG_SHOT, option_set: 1 the frame shown next
   is written as captures/frame-pending.png in the game folder (gpu/vk/vk_bugshot.cpp), 0 asked no longer; option: 0 none
   asked, 1 not there yet, 2 written, 3 no picture */
enum { HOS_OPT_SHARE_ASK = 70, HOS_OPT_SHARE_TEXT, HOS_OPT_SHARE_SAID, HOS_OPT_SHARE_IMPORT, HOS_OPT_BUG_SHOT };
enum { HOS_SHARE_EXPORT = 1, HOS_SHARE_EXPORT_BUG, HOS_SHARE_IMPORT, HOS_SHARE_IMPORT_CANCEL, HOS_SHARE_CAPTURE, HOS_SHARE_CAPTURE_KEY,
       HOS_SHARE_NOTICE };
/* (Triforce) The Debug tab's name pages (gpu/vk/vk_menu.cpp; gpu/vk/oot_debug_link.h has the lines). HOS_OPT_DEBUG_ASK, option:
   the next byte of a line the Game flags or the Cutscenes page asks of the game's side (host/hos_main.c reads it on the
   game's thread, at Link's update), a line feed at a line's end, 0 while there is none. HOS_OPT_DEBUG_TEXT, option_set: the
   next byte of a line for the pages, and HOS_OPT_DEBUG_SAID, option_set: the bytes sent so far are a line of that kind
   (OOT_SAID_*). HOS_OPT_AMIIBO_ASK, option: the same for the Scan amiibo page, the name of a file of the game folder's
   amiibo folder, or a line of its own for none (host/nn_nfp.c reads it at the game's pad read). HOS_OPT_AMIIBO_SAID,
   option_set: what became of the figure, HOS_AMIIBO_* (host/amiibo.h's NFP_SAID_*): none chosen, waiting for the game to
   look for a figure, on the reader, read by the game and lifted off, the file is not a figure's dump, the file is not
   there */
enum { HOS_OPT_DEBUG_ASK = 115, HOS_OPT_DEBUG_TEXT, HOS_OPT_DEBUG_SAID, HOS_OPT_AMIIBO_ASK, HOS_OPT_AMIIBO_SAID };
enum { HOS_AMIIBO_NONE, HOS_AMIIBO_WAITING, HOS_AMIIBO_ON_READER, HOS_AMIIBO_READ, HOS_AMIIBO_REFUSED, HOS_AMIIBO_NO_FILE };
/* (Triforce) The game's frame pacing beside other limiters (host/nn_vi.c hos_vi_native_acquire, gpu/vk/vk_pace.cpp), on the
   game's presenting thread where its frame starts, once a frame. HOS_OPT_PACE_PERIOD, option_set: the period the host
   paces the frame by, in nanoseconds (the frame rate cap in effect). HOS_OPT_PACE_MOVIE, option_set: 1 while a movie
   plays (the game's own wait holds a movie). HOS_OPT_PACE_START, option, right after them: 0 the host's own wait paces
   this frame, as without a renderer to ask; 1 another limiter holds it and the host's wait steps aside for this frame
   (RivaTuner Statistics Server, which holds the frame's presents, or NVIDIA Reflex with DLSS frame generation, whose
   sleep is taken inside the call). 85 to 89 are pacing's.
   (Triforce) HOS_OPT_PACE_STEADY, the Steady frame rate row (host/pace_steady.h): option_set, after the period, 1 while the
   period told is the steady frame rate's (the present then holds the game's pictures to it: gpu/vk/present_smooth.h
   present_smooth_paced_at), 0 otherwise; option, the microseconds the present held the game's presenting thread for it since
   the last ask (what the frames needed is measured without that wait) */
enum { HOS_OPT_PACE_PERIOD = 85, HOS_OPT_PACE_START, HOS_OPT_PACE_MOVIE, HOS_OPT_PACE_STEADY };

/* (hookshot) the controls' names (host/nn_hid.c, host/pad_sdl.c, the pad_ and key_ lines of triforce_options.ini):
   the Switch's buttons in nn::hid's bit order, and a gamepad's inputs by position, SDL's order (the bottom face
   button first: A on an Xbox pad, cross on a DualSense) */
enum { HOS_SW_A, HOS_SW_B, HOS_SW_X, HOS_SW_Y, HOS_SW_LS, HOS_SW_RS, HOS_SW_L, HOS_SW_R, HOS_SW_ZL, HOS_SW_ZR,
       HOS_SW_PLUS, HOS_SW_MINUS, HOS_SW_LEFT, HOS_SW_UP, HOS_SW_RIGHT, HOS_SW_DOWN, HOS_SW_N };
enum { HOS_PAD_SOUTH, HOS_PAD_EAST, HOS_PAD_WEST, HOS_PAD_NORTH, HOS_PAD_BACK, HOS_PAD_GUIDE, HOS_PAD_START,
       HOS_PAD_LSTICK, HOS_PAD_RSTICK, HOS_PAD_LSHOULDER, HOS_PAD_RSHOULDER, HOS_PAD_UP, HOS_PAD_DOWN, HOS_PAD_LEFT,
       HOS_PAD_RIGHT, HOS_PAD_LTRIGGER, HOS_PAD_RTRIGGER, HOS_PAD_N };
/* ((Triforce) 11-17) the left stick pushed past half its travel, in the menus' pad bits only (host/nn_hid.c hos_pad_raw):
   the overlay's lists move by the stick as by the D-pad. Past HOS_PAD_N, so no binding takes them */
enum { HOS_PAD_LS_UP = 20, HOS_PAD_LS_DOWN, HOS_PAD_LS_LEFT, HOS_PAD_LS_RIGHT,
       HOS_PAD_RS_UP, HOS_PAD_RS_DOWN };   /* (and the right stick up or down: the Mods screen's description scrolls by it) */
enum { HOS_BIND_PAD, HOS_BIND_KEY };
/* (hookshot) the controller faces of the button prompts (host/prompts.c): the Switch, then nx2/prompt_faces.py FACES' order;
   the Button prompts row's value is one more (0 Auto). (Triforce) HOS_PROMPT_KEYBOARD: the keyboard and mouse, no
   controller's: each button's icon is the key or the mouse button bound to it (nx2/prompt_keys.py) */
enum { HOS_PROMPT_SWITCH, HOS_PROMPT_XBOX, HOS_PROMPT_PS4, HOS_PROMPT_PS5, HOS_PROMPT_STEAMDECK, HOS_PROMPT_KEYBOARD, HOS_PROMPT_N };
/* keys only (HOS_BIND_KEY), after the Switch's buttons: the left stick's and the camera's directions */
enum { HOS_KEY_MOVE_UP = HOS_SW_N, HOS_KEY_MOVE_DOWN, HOS_KEY_MOVE_LEFT, HOS_KEY_MOVE_RIGHT, HOS_KEY_CAM_UP,
       HOS_KEY_CAM_DOWN, HOS_KEY_CAM_LEFT, HOS_KEY_CAM_RIGHT, HOS_KEY_N };
/* (Triforce) more of the Controls tab's rows, after NX2navi's (host/pad_sdl.c and host/nn_hid.c read them, vk_replay.cpp's
   option answers them; 50 to 59 are theirs). HOS_OPT_RUMBLE_CURVE: the Vibration curve, the power on the motors' rumble
   in hundredths, 20 to 150, 100 none. HOS_OPT_RUMBLE_TRIGGERS: Trigger vibration, 1 the rumble in an Xbox pad's triggers
   too. HOS_OPT_LSTICK_ZONE, HOS_OPT_RSTICK_ZONE: the left and the right stick's own dead zone, one more than the row's
   value: 0 none (As Stick dead zone), 1 to 51 for 0% to 50% (host/nn_hid.c hos_stick_own). HOS_OPT_WALK_KEY: the Walk
   key row, 0 Off, 1 Caps Lock, 2 Shift, 3 Ctrl, 4 Tab (host/nn_hid.c walk_key_code). HOS_OPT_WALK_SCALE: how far the
   movement keys push the left stick while it is held, in hundredths, 20 to 90 */
enum { HOS_OPT_RUMBLE_CURVE = 50, HOS_OPT_RUMBLE_TRIGGERS, HOS_OPT_LSTICK_ZONE, HOS_OPT_RSTICK_ZONE, HOS_OPT_WALK_KEY,
       HOS_OPT_WALK_SCALE };
/* (Triforce) The System tab's rows for the cheats of the mods' cheat files (host/mod_cheats.c runs them, gpu/vk/vk_menu.cpp
   and gpu/vk/mod_cheat_rows.h show them; 130 to 134 are theirs, 134 not used). At the first frame the host names its
   cheats, in the rows' order: HOS_OPT_MODCHEAT_TEXT, option_set: the next byte of a cheat's name, UTF-8, then a line feed
   and the line the row shows under its help; HOS_OPT_MODCHEAT_ADD, option_set: the bytes sent so far are the next row's,
   the value its HOS_MODCHEAT_* bits; HOS_OPT_MODCHEAT_COUNT, option_set: the list is whole, the rows are shown (0: none).
   HOS_OPT_MODCHEAT_ASK, option: a switch the player changed since the last read, (its row + 1) * 2 + 1 for on, + 0 for
   off; 0 nothing (taken: the next read answers the next change, or 0) */
enum { HOS_OPT_MODCHEAT_TEXT = 130, HOS_OPT_MODCHEAT_ADD, HOS_OPT_MODCHEAT_COUNT, HOS_OPT_MODCHEAT_ASK };
enum { HOS_MODCHEAT_ON = 1,       /* switched on */
       HOS_MODCHEAT_ALWAYS = 2,   /* a master code: always runs, the row cannot be changed */
       HOS_MODCHEAT_REFUSED = 4   /* cannot run here (the row's line says why): the row cannot be changed */ };
/* (Triforce) The options' own sounds, the game's interface sounds, after NX2navi's menu sounds (gpu/vk/vk_menu.cpp and
   gpu/vk/menu_sound_events.h say which, host/menu_sound.c plays them; 135 to 139 are theirs). HOS_OPT_MENU_SOUND, option:
   the next sound the options asked for, HOS_MENU_SOUND_*, 0 none (taken: the next read answers the one after it). Read
   by the host's sound thread alone, at each block it mixes, so a sound starts within one block of its press.
   HOS_OPT_MENU_SOUND_VOLUME, option: the Audio tab's Menu sounds row, percent, 0 to 100 (0: silent) */
enum { HOS_OPT_MENU_SOUND = 135, HOS_OPT_MENU_SOUND_VOLUME };
enum { HOS_MENU_SOUND_OPEN = 1, HOS_MENU_SOUND_CLOSE, HOS_MENU_SOUND_CURSOR, HOS_MENU_SOUND_DECIDE, HOS_MENU_SOUND_CANCEL,
       HOS_MENU_SOUND_TAB_LEFT, HOS_MENU_SOUND_TAB_RIGHT,
       HOS_MENU_SOUND_FANFARE,   /* ((Triforce) a mod installed from the Mods screen: the game's item-get fanfare) */
       /* ((Triforce) 11-17) the Mods entry under the game's lists sounds as those lists do: the system menu's cursor and its
          "decide" (the title's Continue, the Load list's choice: the lab's file opens, HOS_FS_TRACE) */
       HOS_MENU_SOUND_SYS_CURSOR, HOS_MENU_SOUND_SYS_DECIDE,
       HOS_MENU_SOUND_N };
/* (Triforce) The Mods entry under a game list's last button (host/oot_menu.c tells, gpu/vk/vk_menu.cpp draws it): the pause
   menu's System page and the title's list. Each frame such a list runs, option_set: HOS_OPT_MODS_X0 .. HOS_OPT_MODS_Y1 its
   last button's box in the layout's units (3840 x 2160, the origin at the middle, y up), HOS_OPT_MODS_PITCH the distance to
   the button above it, then HOS_OPT_MODS_LIST: HOS_MODS_LIST_* bits (that frame's report is whole). HOS_OPT_MODS_ENTER,
   option_set: the cursor goes onto Mods (1: the game's Down there, which the game is not given, host/nn_hid.c), or Mods was
   clicked (2: open it). HOS_OPT_MODS_SELECTED, option: 1 while the cursor is on Mods or its screen is open (the game gets
   no input then: host/nn_hid.c). HOS_OPT_MODS_PRESS, option_set: presses the game was given meanwhile that are the menu's (a
   test's scripted ones, which reach no window): HOS_MODS_PRESS_* bits */
enum { HOS_OPT_MODS_LIST = 150, HOS_OPT_MODS_X0, HOS_OPT_MODS_X1, HOS_OPT_MODS_Y0, HOS_OPT_MODS_Y1, HOS_OPT_MODS_PITCH, HOS_OPT_MODS_ENTER,
       HOS_OPT_MODS_SELECTED, HOS_OPT_MODS_PRESS };
enum { HOS_MODS_LIST_SHOWN = 1, HOS_MODS_LIST_LAST_FOCUSED = 2, HOS_MODS_LIST_TITLE = 4 };
enum { HOS_MODS_PRESS_UP = 1, HOS_MODS_PRESS_DOWN = 2, HOS_MODS_PRESS_LEFT = 4, HOS_MODS_PRESS_RIGHT = 8, HOS_MODS_PRESS_A = 16,
       HOS_MODS_PRESS_B = 32, HOS_MODS_PRESS_L = 64, HOS_MODS_PRESS_R = 128 };
/* ((Triforce) 11-17) Text typed into the menu (the Mods screen's search). HOS_OPT_TEXT_WANTED, option: 1 while the menu takes
   text. HOS_OPT_TEXT_CHAR, option_set (the window's thread, host/display.c): a code point typed, or 8 Backspace, 127
   Ctrl+Backspace (a pasted text comes as its characters) */
enum { HOS_OPT_TEXT_CHAR = 159, HOS_OPT_TEXT_WANTED };

/* (hookshot) the mouse over the window while it is free (not captured): the Insert menu's (host/display.c,
   host/posix/display_sdl.c). x and y: the cursor in the window's pixels, of a window width x height pixels (the
   swapchain's, as a rule); held: the buttons down now, pressed: those pressed since the last read (1 left, 2 right,
   4 middle); wheel: the wheel's notches since then, away from the player positive */
enum { HOS_MOUSE_LEFT = 1, HOS_MOUSE_RIGHT = 2, HOS_MOUSE_MIDDLE = 4 };
typedef struct MkwVkPointer {
    int32_t x, y, width, height;
    uint32_t held, pressed;
    int32_t wheel;
} MkwVkPointer;

/* what the executable gives the back end */
typedef struct MkwVkHost {
    uint32_t version;   /* MKWVK_API_VERSION */
    void (*log)(const char *line);
    /* guest memory: the window, and GPU addresses (pools, below 1 TB the guest address itself) */
    uint8_t *(*guest_base)(void);
    uint64_t (*guest_size)(void);
    uint8_t *(*gpu_ptr)(uint64_t gpu, uint64_t *avail);
    uint64_t (*gpu_guest)(uint64_t gpu);
    int (*gpu_read)(uint64_t gpu, void *dst, uint64_t n);
    int (*gpu_write)(uint64_t gpu, const void *src, uint64_t n);
    /* guest pages written since the last call (the soft write watch): count, page numbers in *pages (freed with
       free_pages) */
    int64_t (*collect_written)(uint32_t **pages);
    void (*free_pages)(uint32_t *pages);
    void (*mark_written)(const void *p, uint64_t n);
    /* clocks: the guest tick (31.25 MHz from the start), host ns */
    uint64_t (*ticks)(void);
    int64_t (*now_ns)(void);
    /* NVN texture layouts (nvn_tex.c) */
    void (*tex_layout)(const nvn_texdesc *d, nvn_texlayout *o);
    uint64_t (*tex_level_offset)(const nvn_texdesc *d, unsigned level);
    int (*fmt_bpb)(int format, int *bw, int *bh);
    void (*tex_level_tiles)(const nvn_texdesc *d, unsigned level, unsigned *tile_h, unsigned *tile_d);
    uint32_t (*tex_linear_stride)(const nvn_texdesc *d);
    int (*depth_minus_w)(void);
    /* the memory pools ({guest start, size} each; returns the count, at most max) and a number that changes with
       them (host/nvdrv.c) */
    int (*gpu_pools)(uint64_t (*out)[2], int max);
    uint32_t (*maps_generation)(void);
    /* a number that changes with every mapping of NVN's virtual pools' pages (MapVirtual: sparse textures) */
    uint32_t (*vmap_generation)(void);
    /* the logical processors kept from the game's threads, for the replay thread (0: none) */
    uint64_t (*reserved_cpus)(void);
    /// The soft write watch's marks within [ranges[i][0], ranges[i][1]) only, read and cleared (free with free_pages)
    int64_t (*collect_written_in)(const uint64_t (*ranges)[2], int n, uint32_t **pages);
    /// Zeroed guest memory of the renderer's own (address space above the heap, never freed; 0 when none is left):
    /// what the NVN driver keeps in GPU memory of its own, such as its constant banks (mkw_eden.dll)
    uint64_t (*guest_alloc)(uint64_t size);
    /// (debugging) CPU writes to [guest, guest + size) logged with the writer's guest call stack (a page watch)
    void (*watch_guest_writes)(uint64_t guest, uint64_t size);
    /* (hookshot) the gamepads' inputs held now (1 << HOS_PAD_*), whatever they are bound to: the Insert menu's
       (host/nn_hid.c) */
    uint32_t (*pad_raw)(void);
    /* (hookshot) the controls, for the Insert menu's Controls page. button: the Switch's, HOS_SW_*; set HOS_BIND_PAD:
       the gamepad input that presses it (a HOS_PAD_* position, -1 none; slot 0 only); set HOS_BIND_KEY: the keys that
       do (Windows key codes, 0 none; slots 0 and 1; a mouse button's too, and HOS_KEY_* past the Switch's buttons).
       bind with button -1: all back to their defaults */
    int (*binding)(int set, int button, int slot);
    void (*bind)(int set, int button, int slot, int code);
    /// Display aspect of the presented picture, after crop: the camera aspect applied at startup.
    /// Zero means no override; the renderer then fits the cropped texture's pixel aspect.
    float (*aspect_ratio)(void);
    int (*take_key_press)(int key);
    /* (hookshot) the free mouse (MkwVkPointer), read and its presses and wheel cleared: 1 when the cursor is over the
       window and the window has the focus. The Insert menu releases the mouse while it is open: WM_APP + 8 to the
       window, wParam 1 open, 0 closed (the capture it had then comes back) */
    int (*pointer)(MkwVkPointer *out);
    /* (hookshot) the microphones for the game's input streams (host/nn_audio_in.c), for the Insert menu's Microphone row:
       how many there are (listing them again), one's name, and the row's choice: mode 0 off, 1 the system's default, 2 the
       microphone of that name (index: its place in the list, -1 while it is not plugged in) */
    int (*mic_count)(void);
    const char *(*mic_name)(int index);
    void (*mic_select)(int mode, const char *name, int index);
    /* (hookshot) the controllers' motion sensors, for the Insert menu's Motion controls row (host/pad_sdl.c): 0 no
       controller, 1 controllers with no gyroscope, 2 one with a gyroscope */
    int (*gyro_status)(void);
    /* (hookshot) the log lines queued so far written out now (host/hos_thread.c log_queue): before the back end ends
       the process itself (replay_fatal), so a device lost's report is not lost with it */
    void (*log_flush)(void);
    /* (hookshot) the controller face of the button prompts the game was given at its start (host/prompts.c): HOS_PROMPT_*;
       -1 none (the game's own icons), or not chosen yet */
    int (*prompt_face)(void);
    /* (hookshot) the game's romfs folder (--romfs, or romfs beside the executable): the overlays read the game's own menu font
       from it (gpu/vk/vk_imgui.cpp), in memory only */
    const char *(*romfs_dir)(void);
    /* (hookshot) ultrawide's menus (nx2/oot_wide.py KEEP_VTABLES): keep_mask sets which of those screens keep the whole picture
       (a bit each; HOS_WIDE_KEEP=<hex> over it); returns the margin of the middle 16:9 at either side, a fraction of the
       picture's width (0: the picture is 16:9) */
    float (*wide_menus)(uint32_t keep_mask);
} MkwVkHost;

/* what the back end gives the executable: the rx_* functions (gpu/render/rx_nvn.h, rx_core) */
typedef struct MkwVkRenderer {
    uint32_t version;
    int (*start)(void *hwnd);
    void (*resize)(int width, int height);
    const char *(*describe)(void);
    void (*stats)(int64_t *draws, int64_t *skipped, int64_t *pipelines);
    void (*nvn_submit)(const uint64_t *handles, int n);
    uint64_t (*nvn_fence)(void);
    uint64_t (*nvn_pushed)(void);
    uint64_t (*nvn_replayed)(void);
    int (*nvn_wait)(uint64_t fence, uint64_t timeout_ns);
    void (*wait_writebacks_reading)(const void *p, uint64_t n);
    void (*nvn_finish)(void);
    void (*nvn_present)(const nvn_tex *t, const int crop[4]);
    void (*nvn_register_texture)(int id, const nvn_tex *t, const nvn_view *v);
    void (*nvn_register_sampler)(int id, const nvn_samp_par *p);
    void (*nvn_set_texture_pool_size)(int n);
    void (*nvn_texture_released)(const nvn_tex *t);
    void (*nvn_program_released)(const nvn_prog *p);
    void (*nvn_read_texels)(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst,
                            uint64_t row_pitch, uint64_t image_pitch);
    void (*nvn_write_texels)(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r,
                             const uint8_t *src, uint64_t row_pitch, uint64_t image_pitch);
    void (*nvn_invalidate)(uint64_t gpu, uint64_t size);
    void (*nvn_pool_created)(uint64_t gpu, uint64_t size);   /* a memory pool made (non-virtual): imported early */
    /* whether the GPU may use guest memory in [lo, hi) through an import (the host keeps such pages committed) */
    int (*guest_imported)(uint64_t lo, uint64_t hi);
    /* virtual pages [gpu, gpu + size) mapped now are about to be unmapped or remapped (the old mapping still in place):
       what the GPU wrote into textures there goes into their memory first */
    void (*nvn_vmap_changing)(uint64_t gpu, uint64_t size);
    /* a texture initialized (its storage: whether the CPU's reads of a range may need a texture's texels) */
    void (*nvn_texture_created)(const nvn_tex *t);
    /* the CPU set an event's value: in order with the submits (the GPU's waits on it) */
    void (*nvn_event_signal)(uint64_t gpu, uint32_t value);
    int (*option)(int id);   /* (hookshot, the Insert menu) 0: game time follows the clock, 1: the menu is open, 2: dynamic resolution, 3: its fixed scale in percent, 4: the controllers count behind other windows, 5: text speed (0 normal to 3 instant), 6: native mouse look (host/mouse_native.c),
                                7: the mouse's sensitivity in percent, 8: its up and down swapped (host/nn_hid.c),
                                18 to 23: the Motion page (HOS_OPT_GYRO*: motion controls from a pad's gyroscope, host/pad_sdl.c;
                                the horizontal and vertical sensitivity in percent, each turned around, the deadzone) */
    /* a submit with its queue */
    void (*nvn_submit_q)(const uint64_t *handles, int n, uint64_t queue);
    /* (hookshot) a fence of one queue: the value its own last submission signals (nvnQueueFenceSync) */
    uint64_t (*nvn_fence_q)(uint64_t queue);
    /* (hookshot, the Insert menu) a setting changed from the game's side, kept in the options file: 7 the mouse's sensitivity
       in percent (the - and = keys, host/nn_hid.c) */
    void (*option_set)(int id, int value);
} MkwVkRenderer;

/* the DLL's one export: null when Vulkan is not usable (the executable then falls back to WebGPU) */
typedef const MkwVkRenderer *(*mkwvk_init_fn)(const MkwVkHost *host);
#ifdef MKWVK_BUILDING
#ifdef _WIN32
__declspec(dllexport) const MkwVkRenderer *mkwvk_init(const MkwVkHost *host);
#else
__attribute__((visibility("default"))) const MkwVkRenderer *mkwvk_init(const MkwVkHost *host);
#endif
#endif

#ifndef _WIN32
/* (linux) what start() is given in place of a window handle (host/posix/display_sdl.c): the window makes the
   surface (SDL_Vulkan_CreateSurface: X11 or Wayland) and says its size in pixels */
#define MKWVK_WINDOW_VERSION 2
typedef struct MkwVkWindow {
    uint32_t version;   /* MKWVK_WINDOW_VERSION */
    int (*create_surface)(void *window, void *instance, void *surface_out);   /* (VkInstance, VkSurfaceKHR *): 1 if made */
    void (*drawable_size)(void *window, int *width, int *height);
    void *window;
    /* (version 2) the refresh rate of the display the window is on, in mHz; 0 when not known (frame generation's Auto) */
    int (*refresh_mhz)(void *window);
} MkwVkWindow;
#endif

#ifdef __cplusplus
}
#endif
