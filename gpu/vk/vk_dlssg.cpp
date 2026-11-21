// (Triforce) NVIDIA DLSS Frame Generation through NVIDIA's Streamline, after NX2navi's runtime/vk_dlssg.c and
// runtime/dlssg/nx2_sl.cpp (its additions are in the public domain, over an MIT base).
//
// Asked for by the Insert menu's DLSS frame generation row (2x, 3x, 4x; dlss_frame_generation= in the options file) or
// RX_VK_DLSSG=<2|3|4>, read once at the start: Streamline has to be up before the Vulkan instance is made, and the
// instance, the device and every function of both are then fetched through its interposer (the SDK's manual hooking:
// its vkCreateInstance and vkCreateDevice add the extensions, features and queues DLSS-G needs, and its swapchain
// functions are where the generated frames go in). Nothing of Streamline is linked: sl.interposer.dll is loaded from
// the game's folder (or its dlssg folder) with its plugins sl.common, sl.dlss_g, sl.pcl, sl.reflex and nvngx_dlssg.dll;
// without them, or on another vendor's card, the game starts as without the row.
//
// Each frame of the game: where it starts (after the present of the one before) Reflex's sleep and the frame's first
// latency markers, the submission's end before the present's submit, and at the present the camera, the depth and the
// motion vectors of AMD FSR 3.1's inputs (vk_fsr3_inputs.cpp makes them at the gameplay upscale whatever the upscaler:
// the depth reversed, the motion in render pixels toward the frame before), then the present through the interposer,
// which shows one to three generated frames between two of the game's. DLSS-G needs Reflex, which is Streamline's
// here: the renderer's own Reflex (vk_reflex.cpp) and its own frame generation (vk_framegen.cpp) stand aside.
// Generation pauses while the menu is open and while a frame has no scene (a pause, a load, a movie), after the new
// state has held for 12 presents.
//
// (Triforce) One wait a frame, after NX2navi's (runtime/vk_dlssg.c vk_dlssg_pace): with the Display tab's Reflex frame
// limit row on, Reflex's sleep also holds the Game frame rate cap (its frame limit, the driver's minimum interval),
// and the game's own wait (host/nn_vi.c hos_vi_native_acquire) steps aside. The difficulty that left this out at
// first is who waits where: the host's wait is on the game's presenting thread where it takes a window texture, the
// frame's start; Reflex's sleep above is on the replay's thread after a present, and held there with a limit it would
// hold the game only at its next present, through the present queue, after the pad was read and the frame recorded.
// So while Reflex holds the limit its sleep is taken on the game's presenting thread, at that same acquire (pace, asked
// by the host through HOS_OPT_PACE_START: vk_pace.cpp), and frame_start leaves its own out. Who holds a frame is
// decided there, once a frame, by dlssg_pace.h: Reflex only while frames are being generated, a present went out
// lately, Streamline took the very limit asked for, and the frames have not shown that Reflex does not hold it; the
// game's own wait in every other case, from that same frame on. The row is Off by default: the game as before.
//
// Built only with HOOKSHOT_STREAMLINE_SDK=<the SDK's folder> (nx2/build.py; NVIDIA's headers stay outside the
// repository); without it every function answers "off". RX_VK_DLSSG_LOG=1: Streamline's own information lines too.
#include "vk_dlssg.h"

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "dlss_math.h"
#include "dlssg_pace.h"   // (Triforce: who holds the frame limit, Reflex or the game's own wait)
#include "vk_fsr3_inputs.h"
#include "vk_menu.h"
#include "vk_paths.h"

#ifdef HOOKSHOT_DLSSG
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_helpers.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#endif

namespace mkwvk {
bool present_hold_now();     // ((Triforce) vk_replay.cpp, the hold of the last picture: the present under way shows the held picture,
bool present_after_hold();   //  or is the first shown after a hold)
}

namespace mkwvk::dlssg {

namespace {

/// dlss_frame_generation= in triforce_options.ini (the menu's own load comes after the instance is made)
int ini_value() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string dir(exe, n);
    const std::string path = dir.substr(0, dir.find_last_of("\\/") + 1) + "triforce_options.ini";
    int v = 0;
    if (FILE* f = fopen(path.c_str(), "r")) {
        char line[160];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "dlss_frame_generation=", 22)) v = atoi(line + 22);
        fclose(f);
    }
    return v;
}

const char* state_line = "Not asked for.";

} // namespace

int asked() {
    static const int v = [] {
        const char* e = getenv("RX_VK_DLSSG");
        const int row = e && *e ? atoi(e) : ini_value();   // (the row: 0 off, 1 2x, 2 3x, 3 4x; the switch: the multiplier itself)
        const int mult = e && *e ? row : row > 0 ? row + 1 : 0;
        return mult >= 2 ? (mult > 4 ? 4 : mult) : 0;
    }();
    return v;
}

bool built() {
#ifdef HOOKSHOT_DLSSG
    return true;
#else
    return false;
#endif
}

#ifndef HOOKSHOT_DLSSG

PFN_vkGetInstanceProcAddr early() {
    if (asked()) logf("vk: dlssg: this build has no NVIDIA DLSS frame generation (built without NVIDIA's Streamline SDK)");
    if (asked()) state_line = "This build was made without NVIDIA's Streamline SDK.";
    return nullptr;
}
void device_extensions(std::vector<const char*>&, const std::vector<VkExtensionProperties>&) {}
void gave_up(const char*) {}
void device_ready(VkPhysicalDevice, VkDevice) {}
bool active() { return false; }
bool reflex_on() { return false; }
void swapchain_created() {}
void frame_start() {}
bool pace(int64_t) { return false; }
void submit_end() {}
void frame(VkCommandBuffer, uint32_t, uint32_t) {}
void present_marker(bool) {}
int multiplier() { return 1; }
const char* status() { return state_line; }
void shutdown() {}

#else

namespace {

HMODULE mod;
PFun_slInit* p_init;
PFun_slShutdown* p_shutdown;
PFun_slIsFeatureLoaded* p_loaded;
PFun_slIsFeatureSupported* p_supported;
PFun_slSetTagForFrame* p_tag;
PFun_slSetConstants* p_constants;
PFun_slGetNewFrameToken* p_token;
PFun_slGetFeatureFunction* p_feature;
PFun_slDLSSGSetOptions* p_dlssg_options;
PFun_slDLSSGGetState* p_dlssg_state;
PFun_slPCLSetMarker* p_marker;
PFun_slReflexSetOptions* p_reflex;
PFun_slReflexSleep* p_sleep;
PFun_slReflexGetState* p_reflex_state;
PFN_vkGetInstanceProcAddr p_gipa;

int loaded;                          // 1: Streamline is up and the instance goes through it; -1: given up for this run
std::atomic<bool> running{false};    // the device was made through it and DLSS-G answers: active()
std::atomic<uint32_t> frame_no{0};   // the game's frame being made: numbered at frame_start
std::atomic<uint32_t> frames_max{0}; // numFramesToGenerateMax as last reported, 0 before
std::atomic<int> shown_mult{1};
int frames_set = -1;                 // the frames to generate as last set (0 off), -1 before the first present
int mode_now, mode_pending;          // generation as the driver has it, and the presents since the other state was wanted
uint64_t presented, shown;
constexpr int kSettle = 12;
// (Triforce) Reflex's frame limit, in microseconds (0 none). want: what pace() asks for (the game's presenting thread
// writes it); set: what Streamline last took (written where the presents are made, reflex_apply); refused: Streamline
// refused a limit, so the game's own wait keeps it for the run
std::atomic<uint32_t> limit_want{0}, limit_set{0};
std::atomic<bool> limit_refused{false};
std::atomic<int> low_latency{-1};       // Reflex's sleep runs on this card and driver: 1, 0, -1 while Streamline has not said
int mode_set = -1;                      // ((Triforce) the NVIDIA Reflex row's value Streamline was last given: frame gives a change)
std::atomic<int64_t> hosted_ns{0};      // the host's clock after the newest present, 0 before the first
std::atomic<bool> game_sleeps{false};   // pace() takes Reflex's sleep where the game's frame starts: frame_start leaves its own out
std::atomic<int> game_in_sleep{0};      // the game's presenting thread is inside that sleep now (shutdown waits for it)
DlssgPace pace_state;                   // the game's presenting thread's (pace)
int pace_said = -1;                     // who held the limit as last logged: 1 Reflex, 0 the game's own wait (that thread's too)

void sl_log(sl::LogType type, const char* msg) {
    static const bool all = getenv("RX_VK_DLSSG_LOG") != nullptr;
    // (its information lines stay out, but for the one that says why no frame is generated: a window that is not the one
    // in front, a present mode it does not take, ...: said when it changes)
    if (type == sl::LogType::eInfo && !all) {
        const char* why = msg ? strstr(msg, "DLSS-G disabled") : nullptr;
        if (!why) return;
        static std::string last;
        static std::mutex mu;
        std::lock_guard lk(mu);
        std::string line(why);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line == last) return;
        last = line;
        logf("vk: dlssg: Streamline: %s", line.c_str());
        return;
    }
    static std::atomic<int> n{0};
    if (n++ < (all ? 8000 : 400)) {
        std::string line(msg ? msg : "");
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        logf("vk: dlssg: streamline %s: %s", type == sl::LogType::eError ? "error" : type == sl::LogType::eWarn ? "warning" : "info",
             line.c_str());
    }
}

template <class T> T* proc(const char* name) { return reinterpret_cast<T*>(GetProcAddress(mod, name)); }

/// The token of frame n. Tokens come from a small ring Streamline uses again, so each call takes its own right before use
sl::FrameToken* token(uint32_t n) {
    sl::FrameToken* t = nullptr;
    return n && p_token(t, &n) == sl::Result::eOk ? t : nullptr;
}

/// A matrix of ours (row major, clip = M * position, column vectors) as Streamline takes it: for row vectors, so transposed
void matrix(sl::float4x4& m, const float* v) {
    for (int r = 0; r < 4; r++) m[r] = sl::float4(v[r], v[4 + r], v[8 + r], v[12 + r]);
}

/// The inverse of m (row major), false when it has none
bool inverse(const float* m, float out[16]) {
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    return dlss_math::reprojection(m, identity, out);   // (identity * inverse(m))
}

/// (Triforce) Whether Reflex's sleep runs on this card and driver: Streamline takes a frame limit without it and then
/// sets nothing. Asked until Streamline answers, by reflex_apply alone: slReflexGetState is not thread safe
void reflex_state() {
    static sl::ReflexState rs;
    if (low_latency.load(std::memory_order_relaxed) >= 0) return;
    void* f = nullptr;
    if (!p_reflex_state && p_feature(sl::kFeatureReflex, "slReflexGetState", f) == sl::Result::eOk) p_reflex_state = (PFun_slReflexGetState*)f;
    if (p_reflex_state && p_reflex_state(rs) == sl::Result::eOk) low_latency = rs.lowLatencyAvailable ? 1 : 0;
}

/// Reflex's mode and frame limit to Streamline: where the device is made, before any present, and after that only on
/// the thread that makes the presents (swapchain_created, frame): slReflexSetOptions is not thread safe. Streamline
/// sets both on the swapchain it was told of last and drops them while there is none, so both are set again after
/// each swapchain, and every call carries both: a call without the limit clears it.
void reflex_apply() {
    if (!p_reflex) return;
    sl::ReflexOptions o{};
    // (the Insert menu's NVIDIA Reflex row says with or without boost; off there is still on here: DLSS-G needs it)
    mode_set = opts.reflex;
    o.mode = opts.reflex == 2 ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency;
    // (Triforce: the frame limit is the game's own wait's, 0 here, unless pace() asked for one: the Reflex frame limit row)
    uint32_t us = limit_refused.load(std::memory_order_relaxed) ? 0 : limit_want.load(std::memory_order_acquire);
    if (us) reflex_state();   // (only with a limit to give: with the row off Streamline is asked nothing more than before)
    o.frameLimitUs = us;
    sl::Result r = p_reflex(o);
    // (taken: Streamline said so, and Reflex's sleep runs here: where Streamline says it does not, nothing would hold
    // the limit. Where it has not said, dlssg_pace.h's check of the frames finds a limit nothing holds)
    bool took = r == sl::Result::eOk && (!us || low_latency.load(std::memory_order_relaxed) != 0);
    if (!took && us) {
        limit_refused.store(true, std::memory_order_release);
        logf("vk: dlssg: Streamline did not take Reflex's frame limit of %.3f ms (%d): the game's own wait keeps the limit "
             "for the rest of this run", us / 1000.0, (int)r);
        o.frameLimitUs = us = 0;
        r = p_reflex(o);
        took = r == sl::Result::eOk;
    }
    limit_set.store(took ? us : 0, std::memory_order_release);
    static int n;
    static uint32_t said_us;
    if (n < 4 || r != sl::Result::eOk || (us != said_us && n < 60)) {
        n++, said_us = us;
        char limit[48] = "no frame limit";
        if (us) snprintf(limit, sizeof limit, "frame limit %.3f ms", us / 1000.0);
        logf("vk: dlssg: Reflex low latency %s, %s (%d)", opts.reflex == 2 ? "on with boost" : "on", limit, (int)r);
    }
}

/// The interposer and its plugins: beside the game, or in its dlssg folder
std::wstring files_dir() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string dir(exe, n);
    dir = dir.substr(0, dir.find_last_of("\\/"));
    for (const char* sub : {"", "\\dlssg"}) {
        const std::string d = dir + sub;
        if (GetFileAttributesA((d + "\\sl.interposer.dll").c_str()) != INVALID_FILE_ATTRIBUTES) return std::wstring(d.begin(), d.end());
    }
    return {};
}

} // namespace

PFN_vkGetInstanceProcAddr early() {
    if (!asked() || loaded) return loaded > 0 ? p_gipa : nullptr;
    loaded = -1;
    static std::wstring dir;   // (Streamline keeps the pointers)
    dir = files_dir();
    if (dir.empty()) {
        logf("vk: dlssg: sl.interposer.dll is not beside the game (nor in its dlssg folder): DLSS frame generation off");
        state_line = "NVIDIA's Streamline files are not beside the game.";
        return nullptr;
    }
    for (const wchar_t* need : {L"sl.common.dll", L"sl.dlss_g.dll", L"sl.pcl.dll", L"sl.reflex.dll", L"nvngx_dlssg.dll"})
        if (GetFileAttributesW((dir + L"\\" + need).c_str()) == INVALID_FILE_ATTRIBUTES) {
            logf("vk: dlssg: %ls is missing beside sl.interposer.dll: DLSS frame generation off", need);
            state_line = "Some of NVIDIA's Streamline files are missing beside the game.";
            return nullptr;
        }
    mod = LoadLibraryW((dir + L"\\sl.interposer.dll").c_str());
    if (!mod) {
        logf("vk: dlssg: sl.interposer.dll did not load (error %lu): DLSS frame generation off", GetLastError());
        state_line = "NVIDIA's Streamline did not load.";
        return nullptr;
    }
    p_init = proc<PFun_slInit>("slInit");
    p_shutdown = proc<PFun_slShutdown>("slShutdown");
    p_loaded = proc<PFun_slIsFeatureLoaded>("slIsFeatureLoaded");
    p_supported = proc<PFun_slIsFeatureSupported>("slIsFeatureSupported");
    p_tag = proc<PFun_slSetTagForFrame>("slSetTagForFrame");
    p_constants = proc<PFun_slSetConstants>("slSetConstants");
    p_token = proc<PFun_slGetNewFrameToken>("slGetNewFrameToken");
    p_feature = proc<PFun_slGetFeatureFunction>("slGetFeatureFunction");
    p_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(mod, "vkGetInstanceProcAddr"));
    if (!p_init || !p_tag || !p_constants || !p_token || !p_feature || !p_gipa) {
        logf("vk: dlssg: sl.interposer.dll lacks its entry points: DLSS frame generation off");
        state_line = "NVIDIA's Streamline files are of another version.";
        return nullptr;
    }
    static const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    static const wchar_t* paths[1];
    paths[0] = dir.c_str();
    sl::Preferences p{};
    p.showConsole = false;
    p.logLevel = getenv("RX_VK_DLSSG_LOG") ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    p.pathsToPlugins = paths;
    p.numPathsToPlugins = 1;
    p.pathToLogsAndData = nullptr;   // (no log file of its own: its warnings and errors come to ours)
    p.logMessageCallback = sl_log;
    // (the renderer tracks its own command buffer state; the depth and motion are tagged per frame, which Streamline
    // refuses without the frame-based flag)
    p.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
              sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    p.featuresToLoad = features;
    p.numFeaturesToLoad = 3;
    p.applicationId = 231313132;   // (the id NVIDIA's samples use for builds without an issued one, as vk_dlss.cpp's NGX)
    p.engine = sl::EngineType::eCustom;
    p.renderAPI = sl::RenderAPI::eVulkan;
    const sl::Result r = p_init(p, sl::kSDKVersion);
    if (r != sl::Result::eOk) {
        logf("vk: dlssg: slInit failed (%d): DLSS frame generation off", (int)r);
        state_line = "NVIDIA's Streamline did not start.";
        return nullptr;
    }
    logf("vk: dlssg: Streamline is up (%dx asked for): the Vulkan instance and device are made through it", asked());
    state_line = "Starting.";
    loaded = 1;
    return p_gipa;
}

void device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all) {
    if (loaded <= 0) return;
    static const char* const promoted[] = {VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
                                           VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
                                           VK_KHR_BIND_MEMORY_2_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
                                           VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME, VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
                                           VK_KHR_MAINTENANCE_2_EXTENSION_NAME, VK_KHR_MAINTENANCE_3_EXTENSION_NAME};
    for (const char* name : promoted) {
        bool listed = false, asked_already = false;
        for (const auto& e : all) listed |= !strcmp(e.extensionName, name);
        for (const char* w : want) asked_already |= !strcmp(w, name);
        if (listed && !asked_already) want.push_back(name);
    }
}

void gave_up(const char* why) {
    if (loaded <= 0) return;
    loaded = -1, running = false;
    logf("vk: dlssg: DLSS frame generation is off in this run: %s", why);
    state_line = "Not on this graphics card or driver.";
}

void device_ready(VkPhysicalDevice physical, VkDevice) {
    if (loaded <= 0) return;
    bool feature = false;
    bool ok = p_loaded && p_loaded(sl::kFeatureDLSS_G, feature) == sl::Result::eOk && feature;
    void* f = nullptr;
    if (ok && p_feature(sl::kFeatureDLSS_G, "slDLSSGSetOptions", f) == sl::Result::eOk) p_dlssg_options = (PFun_slDLSSGSetOptions*)f;
    if (ok && p_feature(sl::kFeatureDLSS_G, "slDLSSGGetState", f) == sl::Result::eOk) p_dlssg_state = (PFun_slDLSSGGetState*)f;
    if (p_feature(sl::kFeaturePCL, "slPCLSetMarker", f) == sl::Result::eOk) p_marker = (PFun_slPCLSetMarker*)f;
    if (p_feature(sl::kFeatureReflex, "slReflexSetOptions", f) == sl::Result::eOk) p_reflex = (PFun_slReflexSetOptions*)f;
    if (p_feature(sl::kFeatureReflex, "slReflexSleep", f) == sl::Result::eOk) p_sleep = (PFun_slReflexSleep*)f;
    if (!p_dlssg_options || !p_marker) ok = false;
    if (!ok) {
        sl::Result why = sl::Result::eErrorNotInitialized;
        if (p_supported) {
            sl::AdapterInfo adapter{};
            adapter.vkPhysicalDevice = physical;
            why = p_supported(sl::kFeatureDLSS_G, adapter);
        }
        logf("vk: dlssg: frame generation does not run here (the DLSS-G plugin loaded: %d, its options: %d, the latency marker: %d; "
             "Streamline's support check: %s (%d)). It needs an RTX 40 or 50 card and a current driver", (int)feature,
             p_dlssg_options != nullptr, p_marker != nullptr, sl::getResultAsStr(why), (int)why);
        // ((Triforce, 11-20, ticket "DLSS frame gen not working no matter what troubleshooting i try": an RTX 5070 Ti on a
        // current driver was told it needs an RTX 40 or 50 card. Streamline's answer was eErrorOSDisabledHWS: Windows'
        // "Hardware-accelerated GPU scheduling" was off. The reasons a player can act on are said as they are)
        if (why == sl::Result::eErrorOSDisabledHWS) {
            logf("vk: dlssg: Windows' Hardware-accelerated GPU scheduling is off: DLSS frame generation needs it (Windows "
                 "Settings > System > Display > Graphics > Advanced graphics settings, then restart the PC)");
            state_line = "Off: turn on Hardware-accelerated GPU scheduling in Windows' graphics settings, then restart the PC.";
        } else if (why == sl::Result::eErrorDriverOutOfDate) {
            state_line = "Off: the NVIDIA driver is too old for it. Update the driver.";
        } else if (why == sl::Result::eErrorOSOutOfDate) {
            state_line = "Off: this version of Windows is too old for it.";
        } else {
            state_line = "Not on this graphics card or driver: it needs an RTX 40 or 50 card.";
        }
        return;
    }
    sl::DLSSGState s{};
    if (p_dlssg_state && p_dlssg_state(sl::ViewportHandle(0), s, nullptr) == sl::Result::eOk) frames_max = s.numFramesToGenerateMax;
    if (frames_max.load()) logf("vk: dlssg: DLSS frame generation available, up to %ux here", frames_max.load() + 1);
    else logf("vk: dlssg: DLSS frame generation available (how many frames this card generates is not reported yet)");
    state_line = "Runs in the world; pauses in menus and loads.";
    running = true;
    reflex_apply();   // (no swapchain and no present yet: the mode alone)
}

bool active() { return running.load(std::memory_order_relaxed); }
bool reflex_on() { return active() && p_reflex; }

void swapchain_created() {
    if (active()) reflex_apply();
}

void frame_start() {
    if (!active()) return;
    const uint32_t n = frame_no.fetch_add(1) + 1;
    sl::FrameToken* t = token(n);
    // (no hold before the first present: the driver has no present to time the frame by)
    // (Triforce: nor while the game's presenting thread takes the sleep itself, where its frame starts: pace)
    if (t && p_sleep && presented && !game_sleeps.load(std::memory_order_acquire)) p_sleep(*t);
    if ((t = token(n))) {
        // (the game's simulation and its recording of the frame cannot be told apart from here: both markers at the start)
        p_marker(sl::PCLMarker::eSimulationStart, *t);
        p_marker(sl::PCLMarker::eSimulationEnd, *t);
        p_marker(sl::PCLMarker::eRenderSubmitStart, *t);
    }
}

/// (Triforce) Who holds the frame limit, as a log line when it changes (the first 40). why: the reason the game's own
/// wait holds it. The game's presenting thread's
void pace_say(int reflex, uint32_t us, const char* why) {
    static int lines;
    if (pace_said == reflex || lines >= 40) return;
    pace_said = reflex, lines++;
    if (reflex)
        logf("vk: dlssg: frame limit: Reflex holds it where each frame starts (%.3f ms%s); the game's own wait steps aside",
             us / 1000.0, pace_state.shown ? ", the frames on screen" : "");
    else logf("vk: dlssg: frame limit: the game's own wait holds it (%s)", why);
}

bool pace(int64_t period_ns) {
    if (!active()) return false;
    // (the Reflex frame limit off, the default ((Triforce, options_version 7) no row: RX_VK_REFLEX_LIMIT=1): nothing is decided, asked of Streamline or logged, and the frame is the
    // game's own wait's as before the row; a limit Reflex held until now is taken back at the next present, frame)
    if (!opts.reflex_limit && !limit_want.load(std::memory_order_relaxed) && pace_said < 0) return false;
    const int64_t hosted = hosted_ns.load(std::memory_order_acquire), now = host->now_ns();   // (in this order: now is not before hosted)
    const int mult = shown_mult.load(std::memory_order_relaxed);
    const bool refused = limit_refused.load(std::memory_order_acquire);
    const int sleeps = low_latency.load(std::memory_order_relaxed);
    const bool allowed = opts.reflex_limit != 0 && p_reflex && p_sleep && !refused && sleeps != 0;
    // (only while frames are being generated: a menu, a load and a movie, where generation pauses, are the game's own
    // wait's, as NX2navi leaves its movies to its pacer)
    const uint64_t period = mult > 1 && period_ns > 0 ? (uint64_t)period_ns : 0;
    uint32_t want = 0;
    int found = DLSSG_PACE_SAME;
    bool held = dlssg_pace_step(&pace_state, (uint64_t)now, period, mult, (uint64_t)hosted, allowed,
                                limit_set.load(std::memory_order_acquire), &want, &found) != 0;
    limit_want.store(want, std::memory_order_release);   // (handed to Streamline before the next present: frame)
    const double ms = (double)period / 1e6;
    if (found == DLSSG_PACE_FAST)
        logf("vk: dlssg: Reflex does not hold the frame limit (over half of %d frames came sooner than three quarters of "
             "%.2f ms): the game's own wait holds it for the rest of this run", DLSSG_PACE_WINDOW, ms);
    else if (found == DLSSG_PACE_SHOWN)
        logf("vk: dlssg: Reflex held the frames on screen to the limit, not the frames the game draws (over half of %d "
             "came about %d times %.2f ms apart): it is given %.2f ms over the multiplier from now on", DLSSG_PACE_WINDOW, mult, ms, ms);
    else if (found == DLSSG_PACE_SLOW)
        logf("vk: dlssg: the frames still came about %d times %.2f ms apart: the game's own wait holds the limit for the "
             "rest of this run", mult, ms);
    if (held) {
        // (Reflex's sleep here, where the game's frame starts, in the host's wait's place. The number is the frame after
        // the one the replay's thread is on; the sleep itself is the driver's and takes none, VK_NV_low_latency2's
        // vkLatencySleepNV. A sleep Streamline does not take leaves the frame, and the rest of the run, to the game's
        // own wait: never neither)
        // (counted while this thread is inside Streamline: shutdown waits for it before Streamline goes, and nothing is
        // called once the exit has begun)
        game_in_sleep.fetch_add(1);
        const bool up = running.load();
        sl::FrameToken* t = up ? token(frame_no.load(std::memory_order_relaxed) + 1) : nullptr;
        const sl::Result r = t ? p_sleep(*t) : sl::Result::eErrorInvalidParameter;
        game_in_sleep.fetch_sub(1);
        if (r != sl::Result::eOk) {
            held = false, pace_state.bad = 1, pace_state.last_ns = 0;
            limit_want.store(0, std::memory_order_release);
            if (up)
                logf("vk: dlssg: Reflex's sleep was not taken where the game's frame starts (%d): the game's own wait holds "
                     "the limit for the rest of this run", (int)r);
        }
    }
    game_sleeps.store(held, std::memory_order_release);
    if (held) pace_say(1, want, nullptr);
    else if (!want)   // (a limit Streamline has yet to take is no change: it is handed over at the next present)
        pace_say(0, 0, !opts.reflex_limit ? "the Reflex frame limit is off: RX_VK_REFLEX_LIMIT=1 turns it on"
                       : !p_reflex || !p_sleep ? "this Streamline has no Reflex sleep"
                       : refused ? "Streamline refused the limit"
                       : !sleeps ? "Reflex's sleep does not run on this card and driver"
                       : pace_state.bad ? "Reflex did not hold the limit"
                       : !period ? "no frame is generated now"
                       : hosted ? "no present for 200 ms"
                                : "no present yet");
    return held;
}

void submit_end() {
    sl::FrameToken* t = active() ? token(frame_no.load()) : nullptr;
    if (t) p_marker(sl::PCLMarker::eRenderSubmitEnd, *t);
}

void frame(VkCommandBuffer cmd, uint32_t out_width, uint32_t out_height) {
    if (!active()) return;
    // (Triforce) Reflex's frame limit as the game's presenting thread last asked for it (pace), before this present: on
    // the thread that makes the presents, as every call of Streamline's options. The game's own wait stands aside only
    // once Streamline took it (limit_set)
    if ((limit_want.load(std::memory_order_acquire) != limit_set.load(std::memory_order_acquire) &&
         !limit_refused.load(std::memory_order_relaxed)) ||
        (opts.reflex == 2) != (mode_set == 2))   // ((Triforce) the NVIDIA Reflex row changed between On and On + Boost)
        reflex_apply();
    const uint32_t n = frame_no.load();
    sl::FrameToken* t = token(n);
    if (!t) return;
    const sl::ViewportHandle vp(0);
    fsr3_inputs::Frame in;
    // (Triforce, the hold of the last picture: vk_replay.cpp do_present) a present of the held picture is a frame with no
    // scene: the held frame's depth and motion taken and dropped, never given with a picture they are not of; and the first
    // frame shown after a hold is a new start, nothing generated between it and the picture before the hold
    static bool after_hold;
    const bool held = present_hold_now();
    const bool have = fsr3_inputs::take(in) && in.depth_image && in.motion_image && !held;
    if (held || present_after_hold()) after_hold = true;   // (the latter too where the hold had no picture to show)
    int want = have && !menu_is_open();
    if (want == mode_now) mode_pending = 0;
    else if (++mode_pending >= kSettle) mode_now = want, mode_pending = 0;
    else want = mode_now;   // (the state it has until the new one has stayed)
    const uint32_t most = frames_max.load();
    int generate = want ? asked() - 1 : 0;
    if (generate > (int)(most ? most : 1)) generate = (int)(most ? most : 1);   // (one while Streamline has not said)
    shown_mult = generate + 1;

    static sl::Constants last;   // (the constants of the last frame that had its inputs)
    static bool have_last;
    if (generate && !have && have_last) {
        // (generation is on for this present and the frame has no scene: a menu or a fade inside the settle time. Streamline
        // asks for the common constants every frame and for null tags where depth or motion are not valid: the last
        // frame's constants again, as a new start)
        sl::Constants c = last;
        c.reset = sl::Boolean::eTrue;
        p_constants(c, *t, vp);
        sl::ResourceTag none[2] = {sl::ResourceTag(nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
                                   sl::ResourceTag(nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent)};
        if ((t = token(n))) p_tag(*t, vp, none, 2, (sl::CommandBuffer*)cmd);
    }
    if (generate && have) {
        float clip_to_view[16], prev_to_clip[16];
        if (!inverse(in.projection, clip_to_view) || !inverse(in.clip_to_prev, prev_to_clip)) {
            static int told;
            if (!told++) logf("vk: dlssg: a frame whose projection has no inverse: no generated frame after it");
            return;
        }
        sl::Constants c{};
        matrix(c.cameraViewToClip, in.projection);
        matrix(c.clipToCameraView, clip_to_view);
        matrix(c.clipToPrevClip, in.clip_to_prev);
        matrix(c.prevClipToClip, prev_to_clip);
        c.jitterOffset = sl::float2(in.jitter[0], in.jitter[1]);
        // (the motion is in render pixels: this makes it fractions of the picture)
        c.mvecScale = sl::float2(1.0f / (float)in.width, 1.0f / (float)in.height);
        c.cameraPinholeOffset = sl::float2(0, 0);
        // (the view matrix's rows are the camera's right, up and back axes, its last column the translation: the position is
        // -R^T t)
        const float* v = in.view;
        c.cameraRight = sl::float3(v[0], v[1], v[2]);
        c.cameraUp = sl::float3(v[4], v[5], v[6]);
        c.cameraFwd = sl::float3(-v[8], -v[9], -v[10]);
        c.cameraPos = sl::float3(-(v[0] * v[3] + v[4] * v[7] + v[8] * v[11]), -(v[1] * v[3] + v[5] * v[7] + v[9] * v[11]),
                                 -(v[2] * v[3] + v[6] * v[7] + v[10] * v[11]));
        c.cameraNear = in.camera.near_plane;
        c.cameraFar = std::isfinite(in.camera.far_plane) && in.camera.far_plane < 1e8f ? in.camera.far_plane : 1e8f;
        c.cameraFOV = in.camera.fov_y;
        c.cameraAspectRatio = (float)in.width / (float)in.height;
        c.depthInverted = sl::Boolean::eTrue;   // (FSR's copy of the depth: 1 at the near plane)
        c.cameraMotionIncluded = sl::Boolean::eTrue;
        c.motionVectors3D = sl::Boolean::eFalse;
        c.reset = in.reset || after_hold ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        after_hold = false;
        p_constants(c, *t, vp);
        last = c, have_last = true;
        sl::Resource rd(sl::ResourceType::eTex2d, (void*)in.depth_image, nullptr, (void*)in.depth, (uint32_t)VK_IMAGE_LAYOUT_GENERAL);
        rd.width = in.width, rd.height = in.height, rd.nativeFormat = (uint32_t)VK_FORMAT_R32_SFLOAT;
        rd.mipLevels = 1, rd.arrayLayers = 1, rd.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        sl::Resource rm(sl::ResourceType::eTex2d, (void*)in.motion_image, nullptr, (void*)in.motion, (uint32_t)VK_IMAGE_LAYOUT_GENERAL);
        rm.width = in.width, rm.height = in.height, rm.nativeFormat = (uint32_t)VK_FORMAT_R16G16_SFLOAT;
        rm.mipLevels = 1, rm.arrayLayers = 1, rm.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        sl::Extent ext{0, 0, in.width, in.height};
        sl::ResourceTag tags[2] = {sl::ResourceTag(&rd, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &ext),
                                   sl::ResourceTag(&rm, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &ext)};
        if ((t = token(n))) p_tag(*t, vp, tags, 2, (sl::CommandBuffer*)cmd);
        static bool told;
        if (!told) {
            told = true;
            logf("vk: dlssg: the first frame's inputs: depth and motion %ux%u for a %ux%u picture, near %.4f far %.6g fov %.2f deg, "
                 "jitter %+.3f %+.3f px", in.width, in.height, out_width, out_height, c.cameraNear, c.cameraFar,
                 c.cameraFOV * 57.2957795f, in.jitter[0], in.jitter[1]);
        }
    }
    if (generate != frames_set) {
        sl::DLSSGOptions o{};
        o.mode = generate ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
        o.numFramesToGenerate = generate ? (uint32_t)generate : 1;
        // (off for every menu and load, and on again after: with this flag off only suspends. Without it the generator's
        // resources go and come back inside the present with the queue held, a frame of 100 to 150 ms each time)
        o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
        const sl::Result r = p_dlssg_options(vp, o);
        if (generate) logf("vk: dlssg: frame generation on, %dx (%d made for each of the game's) (%d), picture %ux%u", generate + 1,
                           generate, (int)r, out_width, out_height);
        else logf("vk: dlssg: frame generation paused (%d)", (int)r);
        frames_set = generate;
    }
}

void present_marker(bool after) {
    // (Triforce: when the newest present went out, for pace: with the Reflex frame limit row on only)
    if (after && opts.reflex_limit && active()) hosted_ns.store(host->now_ns(), std::memory_order_release);
    sl::FrameToken* t = active() ? token(frame_no.load()) : nullptr;
    if (!t) return;
    p_marker(after ? sl::PCLMarker::ePresentEnd : sl::PCLMarker::ePresentStart, *t);
    if (!after) return;
    presented++;
    sl::DLSSGState s{};
    if (!p_dlssg_state || p_dlssg_state(sl::ViewportHandle(0), s, nullptr) != sl::Result::eOk) return;
    shown += s.numFramesActuallyPresented;
    {   // (the moment the frames a present shows change, the first 30 times: 2 with one made frame, 1 while Streamline
        // holds back, as it does for a window that is not the one in front)
        static uint32_t last_shown;
        static int told;
        if (s.numFramesActuallyPresented != last_shown && told < 30) {
            told++, last_shown = s.numFramesActuallyPresented;
            logf("vk: dlssg: %u frame(s) shown for each present from present %llu on (status %u)", last_shown,
                 (unsigned long long)presented, (unsigned)s.status);
        }
    }
    if (s.numFramesToGenerateMax && s.numFramesToGenerateMax != frames_max.load()) {   // (a 0 now and then is no news)
        logf("vk: dlssg: up to %ux on this card and driver", s.numFramesToGenerateMax + 1);
        frames_max = s.numFramesToGenerateMax;
    }
    if (presented == 600 || presented % 3600 == 0)
        logf("vk: dlssg: %llu presents, %llu frames shown, %u shown for the last one, status %u", (unsigned long long)presented,
             (unsigned long long)shown, s.numFramesActuallyPresented, (unsigned)s.status);
}

int multiplier() { return active() ? shown_mult.load(std::memory_order_relaxed) : 1; }

const char* status() { return state_line; }

void shutdown() {
    if (loaded <= 0) return;
    // (no present may be inside Streamline while it goes, and none may start after: the presents' lock is taken and kept,
    // as the process exits right after this)
    dev.present_mu.lock();
    running = false, loaded = -1;
    // (Triforce: nor the game's presenting thread inside Reflex's sleep, pace: a frame's time at most. Waited for half a
    // second, then Streamline goes as it did before that sleep was there)
    for (int i = 0; i < 100 && game_in_sleep.load(); i++) Sleep(5);
    if (p_shutdown) p_shutdown();
    logf("vk: dlssg: Streamline shut down");
}

#endif

} // namespace mkwvk::dlssg
