# Frame interpolation setup failures

Frame interpolation is optional. A failed pipeline, sampler, image or image-view
creation must leave it disabled for the session, with its unused resources released
and ordinary presentation still available. An unchecked sampler result must never
be advertised as successful initialization.

`vk_framegen.cpp` constructs each pipeline and image locally, publishing the complete
object only on success. Failure releases its intermediate resources. Interpolation
initialization also releases earlier successful pipelines/samplers when a later
stage fails. The separate window-scaling path uses the same pipeline constructor
but retains its own state.

Resize keeps the existing display-job drain and queue/device-idle sequence before
replacing images. If replacement fails, all newly allocated interpolation images,
its setup objects and the published size/multiplier/history are cleared. Size and
multiplier are committed only after every required image/view exists. Interpolation
keeps the existing permanent-failure policy; this adds no per-frame retry loop,
new setting, allocation budget or change to interpolation quality.
The disabled flag is atomic: the replay thread writes it, while Reflex's
game-thread eligibility check can read it through `framegen_on()`.

`tests/test_framegen_resources.py` compiles the real constructors and lifecycle
functions with CPU Vulkan stubs and checks each creation failure, ownership order,
no duplicate destruction, stale metadata, resize draining and the direct-presentation
gate. It covers fixed2x and max8x image sets and the independent scale initialization.
It needs Vulkan headers but no GPU, SDK vendor runtime or game assets.

Injected failures in these tests do not prove actual driver OOM recovery or
device-loss handling. A successful game smoke validates integration, not a fault
injection into the player's GPU.

## AMD FSR 4's frame generation

The Frame generation row's AMD FSR 4 (`vk_fsr4.cpp`, `fsr4fg`) holds one frame generation context of AMD's library and
one image the made frame is written into. They follow the same rules:

- The image is made first, then the context; both are published together, only when both exist. A failure destroys
  what was made, turns AMD's frame generation off for the session (the row then shows what runs, 2x by the renderer's
  own interpolation) and is logged once. Nothing is retried per frame.
- A context is replaced (another picture size, a scene larger than it was made for, another kind of depth range) before
  anything of the new frame is recorded: the queue and the device idle, AMD's context destroyed, then the image.
- A context that failed while a frame was being recorded stays until the second present after it, since that frame's
  commands name it and are not on the queue yet; a context the row has left stays 120 presents. Both go at the
  present's tick (`framegen_present_tick`), in the same order.
- At the process's exit (`HOS_OPT_EXITING`) nothing of AMD's is destroyed and nothing of it is called again: there is no
  swapchain or thread of AMD's to stop, and the context goes with the process, as the upscaler's does.

The order is one function, `framegen_rows.h` `teardown`, which the real code and `tests/test_framegen_fsr4.py` both
run; the test also runs the real section against a stand-in for AMD's library and checks each of the cases above. It
does not prove what AMD's library or a driver does with these calls: nobody has run them on a card with FSR 4's frame
generation.
