// (Triforce) The picture of a bug capture (vk_bugshot.cpp): the frame the window is shown, as a PNG, for the host's capture
// folder (host/bug_capture.c). The host asks through the options (HOS_OPT_BUG_SHOT), the next present copies the window's
// image out as it stands before the overlays are drawn over it, and a thread of its own writes the file.
#pragma once

#include "vk_internal.h"

namespace mkwvk {

/// The host asked for the frame shown next (option_set HOS_OPT_BUG_SHOT 1), or took the question back (0: it waited long
/// enough). Any thread. One picture at a time: a request while one is under way is that one's
void bugshot_request(bool want);
/// What became of the last request, for the host's wait (option HOS_OPT_BUG_SHOT): 0 none asked, 1 waiting for a frame,
/// for the GPU's copy or for the file, 2 written to bugshot_path(), 3 no picture (the log says why)
int bugshot_state();
/// At a present's last step, before the overlays, with the window's image `index` in TRANSFER_DST_OPTIMAL (and left so),
/// w x h: the copy of that image for a picture asked for, and three presents later the pixels of a copy to their file.
/// On the presenting thread, one at a time (present_image's lock). Costs one load when nothing was asked
void bugshot_record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h);

} // namespace mkwvk
