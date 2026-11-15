// AMD FSR 3.1 through AMD's own FidelityFX library, one of the temporal upscaler's engines (vk_temporal.h; vk_ffx3.cpp). The
// library (libamd_fidelityfx_vk.so on Linux, amd_fidelityfx_vk.dll on Windows) is loaded at run time and speaks the FidelityFX
// SDK 1.1.4 API (third_party/ffx_api_114, MIT); nothing of AMD's is linked or shipped here. Without it, or when it fails,
// Triforce's own FSR 3.1 (vk_fsr3.cpp: AMD's shader sources as dispatches of ours) upscales as before. RX_VK_FSR3_LIB=0: always
// Triforce's own.
//
// This file is separate from vk_fsr4.cpp on purpose: FSR 4's SDK 2.4 headers (third_party/fidelityfx) name the same types
// as these with other layouts, so the two header sets must never meet in one translation unit. Nothing here includes either
// header set; vk_ffx3.cpp alone sees the 1.1.4 one.
#pragma once

namespace mkwvk {

struct TemporalEngine;

/// The library is there, loaded with the five entry points, and its FSR 3.1 has not failed for good in this run: the Upscaler
/// row's FSR 3.1 is then drawn by the temporal chain with this engine, else by vk_fsr3.cpp. The first ask loads the library
/// (the menu and the chain ask; any thread)
bool ffx3_usable();
/// The engine for the chain
const TemporalEngine* ffx3_engine();

}  // namespace mkwvk
