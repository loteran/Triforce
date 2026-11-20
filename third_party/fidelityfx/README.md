# AMD FSR API headers (FidelityFX SDK v2.4.0)

Unmodified MIT-licensed headers from AMD's FidelityFX SDK, tag `v2.4.0`
(<https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK>, `Kits/FidelityFX/api/include` and
`Kits/FidelityFX/upscalers/include`). See `LICENSE.md`.

Only the headers are compiled in (`gpu/vk/vk_fsr4.cpp`, in a build with `HOOKSHOT_FSR=1`). AMD's signed
`amd_fidelityfx_loader_vk.dll` and `amd_fidelityfx_upscaler_vk.dll` (from `Kits/FidelityFX/signedbin` of the same release)
are not vendored: they are loaded at run time, from `RX_VK_FSR_DIR` or the folder of `hookshot.exe`, when the Upscaler row
is on AMD FSR 4.

(Triforce) `framegeneration/include/ffx_framegeneration.h` and `ffx_framegeneration_api_types.h`, of the same tag
(`Kits/FidelityFX/framegeneration/include`, which `LICENSE.md` lists among AMD's MIT files), are the frame generation
context's descriptions, for the Frame generation row's AMD FSR 4 (`gpu/vk/vk_fsr4.cpp`, `fsr4fg`). They were taken as
NX2navi carries them (its `runtime/third_party/ffx_sdk`, whose other headers are byte for byte the ones here), not
downloaded again. The frame generation swapchain's header of that folder (`vk/ffx_api_framegeneration_vk.h`) is not
here: the renderer keeps its own swapchain. AMD's signed `amd_fidelityfx_framegeneration_vk.dll` is not vendored
either: `scripts/fsr_sdk.py` fetches it with the two above.
