// NVIDIA DLSS Super Resolution, one of the temporal upscaler's engines (vk_temporal.h; vk_dlss.cpp). Built in only with
// NVIDIA's NGX SDK (HOOKSHOT_NGX, scripts/ngx_sdk.py: Linux and Windows); without it these say DLSS is not there and the
// game's network runs.
#pragma once

#include <vector>

#include "vk_internal.h"

namespace mkwvk {

struct TemporalEngine;

/// Whether this build has DLSS in it (HOOKSHOT_NGX)
bool dlss_built();
/// Why DLSS cannot be chosen here (the Insert menu's Upscaler row says it), DLSS_OK when it can or is not known yet
enum DlssWhy { DLSS_OK, DLSS_NOT_BUILT, DLSS_ENV_OFF, DLSS_NEEDS_RTX, DLSS_NEEDS_DRIVER, DLSS_NO_RUNTIME,
               DLSS_RUNTIME_NOT_ASCII, DLSS_NGX_FAILED, DLSS_WHY_COUNT };
DlssWhy dlss_why();
/// Asks NGX once whether DLSS runs on this device, so dlss_why knows before the first upscale (the menu, when it opens).
/// Nothing off an NVIDIA device with NGX's extensions, or in a build without DLSS
void dlss_probe();
/// The instance's extensions DLSS needs, added to `want` when the loader has them (all: what it has)
void dlss_instance_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all);
/// The device's extensions DLSS needs, added to `want` when the device has them
void dlss_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all);
/// How many frames NVIDIA DLSS Frame Generation (vk_ngxfg.cpp) makes between two of the game's here, as NGX answered (its
/// FrameGeneration.Available and DLSSG.MultiFrameCountMax): 0 when it cannot (no NVIDIA RTX 40 or 50, no libnvidia-ngx-dlssg
/// runtime beside DLSS's, NGX not started, RX_VK_DLSS=0 or RX_VK_FRAMEGEN_VENDOR=triforce). Starts NGX the first time, so from
/// the device's threads once it exists; 0 before
int dlss_fg_max();
/// Why dlss_fg_max is 0 (a short phrase for the log), "" when it is not
const char* dlss_fg_why();
/// DLSS as the temporal upscaler's engine (DLSS and DLAA on the Upscaler row); nullptr in a build without it
const TemporalEngine* dlss_engine();

} // namespace mkwvk
