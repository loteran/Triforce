// (Triforce) The parallel replay with a temporal upscaler or a frame generation on: what their hooks in the draws decide
// "first" or "n-th", decided in stream order, whichever thread records which segment. tests/test_stream_order.py
// (tests/stream_order_test.cpp)
//
// NVIDIA DLSS, AMD FSR 4 and 3.1 and the frame generations that take the scene's camera (FSR's, DLSS's) are told things
// by the draws, and decide by the order they are told in (vk_temporal.cpp, vk_fsr3_inputs.cpp):
// - the scene's camera: the largest target with a camera this frame, the first of equal ones (temporal_note_scene_draw);
// - the engine's point before the game's bloom: the first draw after the scene's last (temporal_pre_point, below);
// - the skinned draws and the draws that store their positions, each named by its model and which draw of that model in
//   the frame it is (the n-th of a model this frame is matched with the n-th the frame before: dlss_actor::key);
// - and the upscale's dispatch, the engine's point and the draws after it take what those noted.
// Recorded in segments on several threads (vk_replay.cpp replay_handle) the draws came in another order, so the replay
// recorded such a frame on the replay thread alone (par_stream_order_only). Now:
// - the dry pass, which goes through the command buffer in stream order on the replay thread before any segment is
//   recorded, tells the camera and the point's choice (vk_draw.cpp dry_temporal_draw), up to the first place that takes
//   what they noted: the engine's point, or a dispatch that may be the upscale (a network, or a program that did not
//   translate: dry_temporal_dispatch). The segments' workers do not tell them again (replay_hooks_told);
// - from that place to the command buffer's end is the tail: one segment, recorded once every segment before it is
//   recorded (InOrder below), with the hooks told as the draws come, as on the replay thread alone;
// - the skinned and stored draws are noted with their place in the stream (replay_stream_pos) and put back in stream
//   order, and named again, where they are taken (renumber).
// A command buffer that starts with the engine's work pending from the one before (the engine drew, the upscale's dispatch
// still to come) is recorded on the replay thread alone, as all of them were.
//
// What is not the same as on the replay thread alone: the dry pass asks a draw's program only when the point's choice
// needs it (the first draw after a draw into the scene), so a draw that has no program at all (the log's "a draw without
// a program", which is not drawn) still counts as a draw there; and RX_VK_SKIP_FILE and RX_VK_NO_MESH, which leave draws
// out before the hooks, are not asked by the dry pass. When the stored positions' buffer runs out in a frame, which draws
// were given a slot before it did follows the threads' order (it is made larger for the next frame either way); the same
// for the skinned draws kept past 512 without an upscale (they are dropped then either way).
// RX_VK_PARALLEL_TEMPORAL=0: such a frame on the replay thread alone, as before.
#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace stream_order {

/// temporal_pre_point's choice, the scene's camera already known: a draw into the scene starts a run of it again; the first
/// draw after one is the candidate, and it is the point when it is the one learnt (a frame before had it there). Point is
/// {program, format, width, height} with ==; a Point of program 0 is none. here_of() gives the draw's own Point; it is asked
/// only when the answer depends on it (once a run's candidate is another than the learnt one, no later draw of the run can be
/// the point), so a caller that has to look the program up (the dry pass) does it for the first draw after each run
template <class Point, class HereOf>
bool pre_point_step(Point& candidate, const Point& learnt, bool into_scene, HereOf&& here_of) {
    if (into_scene) {
        candidate = Point{};
        return false;
    }
    if (candidate.program && !(candidate == learnt)) return false;
    const Point here = here_of();
    if (!candidate.program) candidate = here;
    return here == learnt && candidate == here;
}

/// Entries the scene's draws noted (skinned draws, stored positions), each with its place in the stream: in stream order
/// again, and each one's key named again by which of its model's entries it is (the key's low 8 bits, dlss_actor::key; the
/// model is the rest). Entries already in stream order (the replay thread alone) are left as they are: their names are what
/// they were given. pos_of(e): its place; key_of(e): a reference to its key
template <class T, class Pos, class Key>
void renumber(std::vector<T>& v, Pos&& pos_of, Key&& key_of) {
    const auto before = [&](const T& a, const T& b) { return pos_of(a) < pos_of(b); };
    if (std::is_sorted(v.begin(), v.end(), before)) return;
    std::stable_sort(v.begin(), v.end(), before);
    std::unordered_map<uint64_t, int> seen;
    for (T& e : v) {
        uint64_t& k = key_of(e);
        const int nth = seen[k >> 8]++;
        k = (k & ~0xffull) | (uint64_t)std::min(nth, 255);
    }
}

/// Where a command buffer's segments start: indices into points (the dry pass's points, every 64 draws and dispatches, each
/// with its work count, in order), for about `per` draws and dispatches a segment over the work before `until` (the tail's
/// start, or the command buffer's end), at most `most` segments. The first segment starts at the command buffer's start and
/// is not listed. work_of(points[i]): the work count after point i
template <class P, class Work>
std::vector<size_t> cuts(const std::vector<P>& points, int64_t until, int64_t per, int most, Work&& work_of) {
    std::vector<size_t> at;
    const int n = (int)std::clamp<int64_t>(per > 0 ? until / per : 1, 1, std::max(most, 1));
    size_t from = 0;
    for (int k = 1; k < n; k++) {
        const int64_t want = until * k / n;
        size_t i = from;
        while (i < points.size() && work_of(points[i]) < want) i++;
        if (i >= points.size() || work_of(points[i]) >= until) break;
        if (!at.empty() && at.back() == i) continue;
        at.push_back(i);
        from = i + 1;
    }
    return at;
}

/// The segments' order where it has to be kept: segment k notes that it is recorded (done), and the tail waits until every
/// segment before it is (wait_before). One a command buffer
class InOrder {
public:
    explicit InOrder(size_t n) : done_(n, false) {}
    void done(size_t k) {
        {
            std::lock_guard lk(mu_);
            if (k < done_.size()) done_[k] = true;
        }
        cv_.notify_all();
    }
    void wait_before(size_t k) {
        std::unique_lock lk(mu_);
        cv_.wait(lk, [&] {
            for (size_t i = 0; i < k && i < done_.size(); i++)
                if (!done_[i]) return false;
            return true;
        });
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<bool> done_;
};

} // namespace stream_order
