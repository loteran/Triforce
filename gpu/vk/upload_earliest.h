// (Triforce, 11-17) A parallel replay's upload, recorded into the earliest segment still being recorded (vk_texture.cpp
// UploadRedirect, texture_touch). tests/test_parallel_init_order.py (tests/parallel_init_order_test.cpp)
//
// A parallel replay (RX_VK_PARALLEL) records one submission's segments at once, each on a worker with its own upload command
// buffer, submitted just before its own commands, the segments in stream order. A texture is looked at once a submission
// (checked_epoch), by whichever worker reaches it first. When that is a later segment N's, its upload lands in the queue after
// an earlier segment M's draws, so M uploads it again in its own uploads: the twin (texture_touch, upload_same.h "the
// parallel replay's twin"). The lab's heavy S2 Market loops (round 48, test43) uploaded 2.1 GB of twins in their last minutes,
// the big streamed or new textures (1532x1328x3, 2240x3032, 3056x1840) each uploaded once by a later segment and again by an
// earlier one; those twins took the frames' upload budget (new_texture_over_budget), and the new textures after them were
// left for the next frame, the picture held.
//
// Now N's upload goes into the init command buffer of the earliest segment J < N still being recorded (each segment's init
// is submitted before its uploads and commands; InitRedirect's buffers, J's own lock held meanwhile, so that J cannot end
// while it is recorded and only J's end waits for it: seg_init_lock). Every segment before J has ended its
// recording: none of them looks at the texture any more, and one that sampled it had looked first, so N would not be
// uploading. Every segment from J on that samples it finds the upload at or before its place in the queue, and its look
// (upload_seg = J's order, not later than its own) asks for no twin: the texture is uploaded once. The bytes are the ones N's
// upload would have written: only an upload staged from the CPU as it is recorded (the submission's bytes) is moved, not one
// upload_inner reads on the GPU (the GPU unswizzle, from the pool) or from live memory (the all-zero clear), vk_texture.cpp
// upload_reads_live: in J's init such a read would run before a segment K between J and N writes that memory on the GPU (a
// copy out of a texture, a storage buffer), and read other bytes than N's own uploads did; those keep N's uploads, and the twin.
// Not for a texture the GPU drew (gpu_written: its contents are the GPU's, the CPU's writes over it taken by their own path)
// or a sparse one (its pages bound by its own submit path): those keep the segment's own uploads, and the twin.
// Nothing is moved when the segment is the earliest still recorded: it uploads in its own uploads, as before.
// RX_VK_UPLOAD_EARLIEST=0: every upload in the segment's own uploads, the twin as before.
#pragma once
#include <cstdint>

namespace upload_earliest {

/// Whether a look's upload may go into an earlier segment's init command buffer
constexpr bool wanted(bool on, bool gpu_written, bool sparse) {
    return on && !gpu_written && !sparse;
}

/// The segment (index, in queue order) whose init command buffer takes the upload of segment `self`: the earliest one before
/// it still being recorded (`open(i)`); -1 when there is none (its own uploads, as before)
template <class Open>
int target(Open open, int n, int self) {
    for (int i = 0; i < self && i < n; i++)
        if (open(i)) return i;
    return -1;
}

/// The order (texture_set_segment's) of segment `index`, from the recording one's: replay_handle numbers a submission's
/// segments in a run (Job::order = base + k, Job::index = k)
constexpr uint64_t order_of(uint64_t self_order, int self_index, int index) {
    return self_order - (uint64_t)(self_index - index);
}

} // namespace upload_earliest
