# Lazy write-back: built, on by default

**Since options_version 5 (2026-11-16): on by default, at the owner's word, with the row and `RX_VK_LAZY_WB` to
turn it off; still never on AMD's driver by the row alone. The status below is from before.**

**Status (2026-11-15): built behind the hidden Debug tab's Lazy write-back row, off unless set, and `RX_VK_LAZY_WB`
(`=1` on, `=0` off, `=2` the self-check) over it. Nobody has run it in the game.** The design below is what was built;
"As built" at the end says how, and what only the game can show. The note was first written when it was studied for the
port of NX2navi's features and left out, because in this code base it cannot be proven correct without running the
game; the owner then asked for it to be built all the same, off by default, so that it can be turned on and compared.
The reasons in "Why it is not built" still stand as the things to watch for.

## What it is

What a shader writes into a mirrored pool lives in the pool's VRAM mirror. At the end of every submission the renderer
copies it back into guest memory, in case the CPU reads it (`gpu/vk/vk_memory.cpp`, the end of
`memory_end_submission`). The game's CPU reads very little of it. NX2navi stopped copying a range once it had gone
back one time, and copies it only when something touches it: "about a third more frames" in open areas, with 66 MiB a
frame no longer crossing PCIe (7 to 9 ms of a frame's 13 to 15 ms of GPU time there).

How NX2navi knows about a touch: it takes the access away from the range's pages (no read, no write). The first access
faults, the fault handler has the renderer copy the range down, gives the pages back and runs the access again
(NX2navi's `runtime/memory.c` "Lazy write-back", `runtime/vk_memory.c` `lazy_fetch`). Around that sit a thaw before every
kernel call that is handed guest memory, a list of ranges in flight, a window between the replay's last check and its
hand-over, a hold after a touch, and a choice by driver (`wb_lazy_choice.h`: off on AMD's driver after two reports of
the game ending in play, off outside Windows). It took several releases of play tests to get there, and one of its
refinements still loses the device on some loads.

## What is different here

- **CPU writes are known late, CPU reads never.** Lifted stores mark their page in a table (`A64_MARK`, the soft
  write watch) and the renderer collects the marks at the next submission (`memory_collect_written`). A load is a
  plain pointer access (`A64_PTR`) with no hook. So "copy it when the CPU touches it" has only two possible sources
  here: page protection, as in NX2navi, or the calls the game makes before it reads.
- **Most of the gain is already taken another way.** Since 2026-11-09 a write-back is a compute pass that compares
  the mirror with a VRAM shadow of guest memory and writes only the words that differ (`wb_diff`, on by default).
  Measured where NX2navi measured (Hyrule Field, about 69 MB a frame of written ranges): the whole copies took 4.4 ms
  of the GPU's 13, the pass takes about 0.7 ms. What a lazy write-back could still save here is that pass, the pages
  it reads into the shadows, and the waits of captures for write-backs still on the GPU (`wb_pages`, the "late"
  pages of `take_capture`). Nobody has measured how much that is.
- **The order in which the GPU's words reach guest memory is tuned by play.** The exposure meter is a sum the GPU
  adds to and the CPU reads and clears; the readback waits in `vk_replay.cpp` `invalidate`, the late pages, the
  deferred syncs and the report words cut out of write-backs (`memory_cpu_owned`) each fixed a flicker that only
  showed in the game (white flashes, blinking cells, a dark frame every 15 to 30). A lazy write-back moves every one
  of those moments.

## Who reads what is written back

Everything below would see old bytes while a copy is owed, unless it is made to pay first.

| Reader | Where | Can it be caught? |
|---|---|---|
| The game's own code | lifted loads, any thread | Only by a fault. `nvnBufferInvalidateMappedRange` and `nvnMemoryPoolInvalidateMappedRange` reach the renderer (`host/nvn_native.c`, `rx_nvn_invalidate`), and it already waits there for the range's write-back. Whether the game calls one before every read of GPU results is not known; `HOS_NVN_LOG_FLUSH` logs the calls |
| NVN event words | `nvnEventGetValue`, `hos_nvn_event_value` (the present poll, `host/hos_thread.c`) read guest memory with no call into the renderer | A hook in the host |
| Host code given game buffers | `memcpy` and the like through `GP()`: audio voices on the mixer thread, the movie's packets, the codec, `nn_libc.c` | By a fault, on that thread |
| The kernel | `ReadFile` and `WriteFile` straight on guest pointers (`host/nn_fs.c` `read_at` and the write beside it: every file read, every save); the same as `pread`/`pwrite` on Linux | **No.** A call into a page without access fails with an error and no fault (`host/mkw_glue.c` says so of reserved pages). Each needs a thaw before it, as NX2navi has |
| The renderer's CPU reads | captures (`capture_make` on the game's thread and its helpers, `guarded_copy`, which drops a page that faults on Windows and is a plain `memcpy` on Linux), `memory_read_as_submitted`, texture uploads (`vk_texture.cpp` `guest_bytes`), shader code, the bank's descriptors | By a fault, but on threads that hold `pools_mu` or `replay_mu`, which a copy must not need |
| The GPU through the import | every `vkCmdCopyBuffer` from `Pool::buffer` (`mirror_make`, `mirror_pieces_make`, `mirror_sync`'s live pages, `memory_rb_refresh`, `late_deferred_bound`, the shadows' fill) and every binding `gpu_buffer_bind` or `gpu_buffer` gives the import for | Never by a fault: each site has to pay first or bind the mirror |
| Code that takes guest memory for complete | a mirror given back or remade (`mirror_free`, `mirrors_give_back`, `demote_trials`, `pieces_set_direct`, `retire_pool` with its inheritance), `texture_flush_to_guest` | Each has to pay first |

And three things undo a page's protection behind the renderer's back on Windows: `import_pool` commits its whole
range again (`VirtualAlloc(MEM_COMMIT, PAGE_READWRITE)`, which its own comment says resets protection), `hos_commit`
does the same for the game's mappings, and `hos_mem.c` discards and decommits pages.

## A design for Triforce

The states of a guest page of a mirrored pool, as a write-back sees them:

| State | Guest memory holds | How it is left |
|---|---|---|
| current | everything the GPU wrote | a written binding of a later submission: **written** |
| written | all but this submission's writes | the submission's end: copied (the diff pass), then current; or, when the page went back once before and nothing forbids it, no copy: **owed** |
| owed | an older version; the mirror alone has the GPU's | a reader: **paying**; a later written binding: stays owed, the newer write joins the same debt (one copy pays for all of them, mirror to guest, so they land in order by construction) |
| paying | being copied by one thread | the copy done and waited for: current. Every other reader of the page waits for that copy, none starts a second |

Rules that keep it correct:

1. A page is owed only whole, only in a pool with a mirror in memory there (not a piece left in guest memory), only
   when the mirror holds every CPU write to it (`page_gen(page) <= synced_gen`: a copy from the mirror would
   otherwise put older bytes over the CPU's), and never where the replay writes words itself (the report words).
2. Every reader in the table above pays before it reads. For CPU reads that means the page has no access while it is
   owed and the fault handler pays; for the kernel a thaw before the call; for the GPU's reads and for code that gives
   a mirror up, a call that pays the range first.
3. A CPU write to an owed page pays first, like a read (the write faults). It does not drop the debt: the write
   covers part of the page and the rest is the GPU's. Only a write of the whole page, which nothing here can
   recognise, could drop it. The page then goes the usual way (marked, collected, synced into the mirror), and is not
   owed again for a while (NX2navi holds it 1 s, doubling to 32 s while it keeps being touched).
4. A payment copies after everything submitted so far, in a command buffer of its own that the paying thread waits
   for; the part being recorded checks at its end whether a range it left owed was paid meanwhile and copies it then.
5. The shadows of the write-back diff stay true without change: an owed page was not written by the pass, so the
   shadow still equals guest memory, and a payment is the same pass run later.
6. A capture's wait (`wb_pages`) has nothing to wait for on an owed page: no submission writes it back. The wait
   moves into the payment.

The page-state logic belongs in a header of its own with tests for the rules above (each reader pays first; a CPU
write pays and does not drop; debts merge; one payer at a time), as the write generations have theirs
(`gpu/vk/write_gen_table.h`).

## Why it is not built

None of these can be settled by a test without the game:

- **The set of kernel accesses.** One missed `ReadFile` into an owed page is a short read handed to the game, with no
  fault and no log. The list above is what a search of `host/` found, not a proof; libraries and drivers that are
  handed guest pointers (the movie decoder, audio) were read, not run.
- **The driver.** The pages are imported into Vulkan as host memory (`VK_EXT_external_memory_host`; a `udmabuf` on
  Linux). Taking their access away while the import lives is what NX2navi's AMD reports point at. Importing a pool
  whose pages have no access is likely to fail outright.
- **Locks.** The fault can come on any thread, including the replay thread and the capture helpers while they hold
  the renderer's locks, and on Linux inside a signal handler. NX2navi's rules for this (ranges in flight, the
  window, fetches outside its freeze lock) were found by profiles and stalls in play.
- **The game's timing.** See "tuned by play" above. Paying at the first touch instead of at the submission's end
  changes when the exposure meter, the event words and the culling's results appear to the CPU.
- **Whether it is worth it.** With the diff pass in place the saving is unmeasured and may be small.

What was considered and rejected as a shortcut: paying only at `nvn*InvalidateMappedRange` and the host's own reads,
with no page protection. It needs no fault handler and no thaw, but it is right only if the game announces every read,
which nobody knows; a read that is not announced gets old data silently. `RX_VK_WB_MAX_KB` is the same bet made by
size, and is off by default for that reason.

## What to do first, in the game

1. Measure what is left to gain: `RX_VK_PASS_TIMING` gives the GPU time of "wb diff" and "wb copies" a frame, and the
   log's "write-back diff (600 submits)" and "a capture taken" lines give the bytes compared and the takes that waited
   for late pages. `RX_VK_NO_WRITEBACK=1` is the upper bound on frame rate (its picture is wrong wherever the CPU reads
   results: do not judge the picture with it).
2. If that is worth the work: log which written-back ranges the game announces before reading
   (`HOS_NVN_LOG_FLUSH`, `RX_VK_LOG_INVALIDATE`, `RX_VK_LOG_WRITEBACK`), and build the page probe NX2navi used before
   its lazy write-back (protect a written-back range, log the first access with its thread and address, give the page
   back): that turns "the CPU does not read most of it" from NX2navi's game build into a fact about this one, and
   finds the kernel accesses on the way.
3. Then the host side (the fault hook, a thaw before `read_at` and the save writes, the commits that reset
   protection), the renderer side (rules 1 to 6), a Debug tab row that is off by default, and the same choice by
   driver NX2navi ended with.

## As built (2026-11-15)

Steps 1 and 2 above were not done first: the owner asked for it built behind a row, so that the first run with it on
is the measurement. What is where:

- **The page states**, `gpu/vk/wb_lazy_state.h`, with the rules and a threaded stress test in
  `tests/test_wb_lazy_state.py`. A page is not owed at once: it is first WATCHED (read-only, still copied at every
  submission) until a collection of the write watch and a capture, both numbered after the watch began, have come
  and named no CPU write to it. That is rule 1 for the writes the renderer learns of late; the renderer adds the rest
  of it: the CPU's newest write generation for the page no newer than its pool's `synced_gen` (`lazy_page_takes`).
  That asks `cpu_gens`, the CPU's writes alone, and not the one table of generations: the first build asked that one,
  where every storage buffer a draw or a dispatch writes is stamped while it is recorded (`memory_stamp_gpu`) and
  `synced_gen` is raised only at the submission's end, so every page being written back looked newer than its sync
  and the owner's first session with the row on watched and owed nothing (`tests/test_lazy_wb_cpu_gens.py`). A CPU write to a watched page faults and ends the watch with no copy; to an owed
  page it pays first. A payment between a recording's end and its submit leaves the rest PENDING until that submit
  (`memory_lazy_submitted`, called from `submit_recording` under the queue's lock). A touch holds its 64 KB block
  for a second, doubling to 32 s, as NX2navi does. Whether it runs: `wb_lazy::choose`.
- **The renderer**, `gpu/vk/vk_memory.cpp` "lazy write-back": `lazy_runs` at each write-back (after the CPU's words
  are cut and the runs clipped to a mirror's resident pieces; never a page with event words), the payment (`lazy_pay`:
  its own command buffer after everything submitted, the held submits flushed first, waited for; the shadows told of
  the pages, rule 5), `lazy_settle` before every copy out of an import and every binding given the import (rule 2 for
  the GPU), and before a mirror, a piece, a retired pool or an import is given up; `import_pool` holds the pages
  while it commits and imports. The fault handler is the process's first vectored handler, added when the renderer
  starts, ahead of host/mkw_glue.c's. Locks: see the section's comment (a payment needs neither `pools_mu` nor
  `replay_mu`, and logs nothing). `tests/test_lazy_wb_hooks.py` holds these hooks in place.
- **Several payments at once** (2026-11-16, NX2navi's `wb_lazy_unlocked`): each payment holds one of four slots, a
  command buffer and a fence of its own (`gpu/vk/wb_lazy_payers.h`); it was one payer at a time, so a thread that
  touched another owed page, or the replay giving a mirror up, waited for the first one's copy before its own went onto
  the queue. Two payments never have the same pages (the table's PAYING has one payer), and each copy still goes onto
  the one queue after everything before it. `RX_VK_LAZY_PAYERS=1`: one at a time again. The threaded stress test runs
  with four slots and with one. What it does not change: a payment for a mirror given up still runs on the replay
  thread under `pools_mu` (NX2navi moved its own out of its pool lock as well); moving it would mean reworking every
  give-up path, for payments of which the owner's sessions have a few a minute.
- **The host**: `hos_guest_io_begin` / `hos_guest_io_end` (`host/rx_backend.c`) around every kernel call on guest
  memory and every change of guest pages' state: `read_at` and the file writes (the saves too) in `host/nn_fs.c`, the
  image's read, `hos_commit`, the discards and decommits of `host/hos_mem.c` and `host/hos_svc.c`, the host zstd
  decoder's one-shot, and the debug dumps. `tests/test_lazy_wb_host.py` reads `host/*.c` for kernel calls that do
  neither, and every exception is in its list with a reason. Audio, the movie and the codec reach guest memory with
  the CPU only (`memcpy` and the like): their touches fault and pay.
- **Linux**: off (`wb_lazy::choose`), the row not shown. **AMD's driver**: the row alone leaves it off;
  `RX_VK_LAZY_WB=1` turns it on there.
- **The self-check**, `RX_VK_LAZY_WB=2`: guest memory is the eager copy (every copy still made); a payment reads the
  mirror's pages, what it would have copied, and compares them with guest memory. A difference is logged with its
  address at the next 600-submit line (`MISMATCH at guest`), not from inside the fault.

What only the game can show: whether the driver keeps an import whose pages have no access (NX2navi's AMD reports),
whether the list of kernel calls is whole in practice (a missed one is a short read with no fault), how often the game
touches what is owed (a touch costs a GPU round trip on the touching thread), and the timing of the exposure meter and
the event words. The 600-submit line `vk: lazy write-back (600 submits)` says what it saved and who paid; with
`RX_VK_LAZY_WB=2` the `check` line says whether it would have been right.
