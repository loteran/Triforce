/* (Triforce, 11-19) Benchmark mode (host/hos_bench.c): HOS_BENCHMARK=<results.json> starts the game straight into play on
   the newest save, tours a few places with the camera turning, measures every frame and writes the results for the launcher.
   Nothing changes without the variable. The game's sides call in here: */
#pragma once
#include <stdint.h>

/* at the start (hos_main.c, before anything else reads it): the variables read once */
void hos_bench_init(void);
/* 1 while benchmark mode is on (HOS_BENCHMARK set), from the start to the exit */
int hos_bench_on(void);
/* host/nn_hid.c: 1 while the benchmark drives the game: the player's devices do not count, the benchmark's presses count with
   the window behind other windows */
int hos_bench_input_only(void);

/* host/oot_menu.c, a menu screen's update with what it shows now: its vtable, whether it has a focused button, the focused
   entry's place in its list (-1 none) and how many entries it has. Returns what to do: */
enum { HOS_BENCH_MENU_NONE, HOS_BENCH_MENU_PRESS_A };
/* (*entry: kept for an entry to focus first; none is asked for now: the file select opens on the newest file itself) */
int hos_bench_menu(uint64_t screen, uint64_t vt, int focus, int focused_entry, int entries, int *entry);

/* host/hos_main.c, Link's update: the map now (Sequence_CurrentBanc), whether a map loads, whether an event plays. Returns the
   destination index to teleport to now (oot_destinations), -1 none; then hos_bench_teleport_sent with what oot_teleport said */
int hos_bench_link(int64_t now, uint64_t actor, const char *map, int loading, int event);
void hos_bench_teleport_sent(int index, int ok);
/* 1 while hos_bench_link wants the map's state (it is read only then: nothing is asked of the game while a scene is measured) */
int hos_bench_link_wants_state(void);

/* host/nn_vi.c, each of the game's presents (now: hos_now_ns at that present) */
void hos_bench_present(int64_t now);

/* host/nn_fs.c, each file opened (its guest path): the save file the game loads, for the log and the results */
void hos_bench_file_opened(const char *guest);
/* host/nn_fs.c: 1 when a save commit is to be left out (benchmark mode writes no save) */
int hos_bench_blocks_save(void);

/* host/hos_relaunch.c: the window is closing (the player, the launcher or the benchmark itself): a run not finished writes
   that it was stopped */
void hos_bench_closing(void);

/* host/rx_backend.c: MkwVkHost.bench, the overlay's state for the renderer (gpu/vk/vk_api.h MkwVkBench) */
struct MkwVkBench;
int hos_bench_state(struct MkwVkBench *out);
