#include "generated/default/stf_xbla_init.h"
#include <rex/ppc.h>
#include <rex/image_info.h>
#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/function_dispatcher.h>
#include <rex/ui/keybinds.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/system/xmemory.h>
#include <Windows.h>
#include <timeapi.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "stf_xbla_app.h"

// Drivers only honor these in the main executable (the copies in the GPU plugin
// DLL are ignored), so without them hybrid laptops run on the integrated GPU.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

uint32_t StfrMapHelpOptionsSelection(uint32_t selection);

namespace {
std::atomic<int> g_pc_width{1920}, g_pc_height{1080}, g_pc_fps{60};
std::atomic<bool> g_pc_fullscreen{true}, g_pc_vsync{true};
std::atomic<int> g_pc_aa{1};
std::atomic<int> g_keyboard_player{1};
std::atomic<int> g_original_screen_size{-1};
using SetGpuPostEffect = void (*)(void*, int);
std::atomic<void*> g_graphics_system{nullptr};
std::atomic<SetGpuPostEffect> g_set_gpu_post_effect{nullptr};
std::atomic<bool> g_recomp_settings_open{false};
std::atomic<bool> g_remap_open{false};
std::atomic<int> g_remap_page{0};
std::atomic<int> g_remap_selected_row{0};
std::atomic<int> g_remap_capture_binding{-1};
std::atomic<WPARAM> g_remap_capture_activation_key{0};
std::atomic<uint32_t> g_remap_reset_generation{0};
// Resolution, Screen Type, Frame Rate, VSync, Anti-Aliasing, Keyboard Player.
constexpr int kPcSettingRows = 6;
std::array<int, kPcSettingRows> g_pc_setting_values{{1, 1, 1, 1, 1, 0}};
bool g_pc_setting_values_initialized = false;
std::atomic<int> g_pc_selected_row{0};
std::mutex g_pc_setting_values_mutex;
std::atomic<uint32_t> g_help_options_object{0};
std::atomic<uint32_t> g_help_options_selection{0xFFFFFFFFu};
std::atomic<bool> g_help_options_active{false};
std::atomic<ULONGLONG> g_help_options_draw_tick{0};
std::atomic<bool> g_reset_dialog_open{false};
std::atomic<uint8_t*> g_guest_base{nullptr};
std::atomic<rex::memory::Memory*> g_guest_memory{nullptr};
std::array<uint32_t, 45> g_recomp_string_buffers{};
std::atomic<rex::system::KernelState*> g_achievement_kernel{nullptr};
std::atomic<bool> g_achievements_open{false};
std::atomic<bool> g_pause_achievements_open{false};
std::atomic<uint32_t> g_main_menu_object{0};
std::atomic<ULONGLONG> g_main_menu_draw_tick{0};
std::atomic<int> g_achievement_page{0};
std::atomic<int> g_achievement_selected_row{0};
std::atomic<bool> g_achievement_entering{false};
std::atomic<WPARAM> g_achievement_back_key{0};
std::array<uint32_t, 8> g_achievement_row_buffers{};
std::array<std::string, 8> g_achievement_row_text{};
std::mutex g_achievement_rows_mutex;
std::array<uint32_t, 8> g_remap_string_buffers{};
std::array<std::string, 8> g_remap_row_text{{"Remap Inputs", "REMAP INPUTS"}};
std::mutex g_remap_rows_mutex;
std::string g_remap_status;
std::wstring ExecutableDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring value(path);
    return value.substr(0, value.find_last_of(L"\\/"));
}

std::filesystem::path OriginalScreenSizePath()
{
    return std::filesystem::path(ExecutableDirectory()) / L"userdata" /
           L"original_screen_size.ini";
}

void SetOriginalScreenSize(int size, bool save)
{
    if (size < 0 || size > 2 || g_original_screen_size.exchange(size) == size) return;

    // Full fills the window. Normal and Large keep the game's aspect rations
    rex::cvar::SetFlagByName("present_letterbox", size == 2 ? "false" : "true");
    if (save) {
        const auto path = OriginalScreenSizePath();
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (!error) {
            std::ofstream file(path, std::ios::trunc);
            if (file) file << size << '\n';
        }
    }
    REXLOG_INFO("Original Screen Size: {} (fill display: {})", size, size == 2);
}

void LoadOriginalScreenSize()
{
    std::ifstream file(OriginalScreenSizePath());
    int size = -1;
    if (file >> size) SetOriginalScreenSize(size, false);
}

HWND FindGameWindow()
{
    struct Search { DWORD process_id; HWND window; } search{GetCurrentProcessId(), nullptr};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto& state = *reinterpret_cast<Search*>(parameter);
        DWORD owner = 0;
        GetWindowThreadProcessId(window, &owner);
        if (owner == state.process_id && GetWindow(window, GW_OWNER) == nullptr) {
            state.window = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

void ApplyPcSettings()
{
    rex::cvar::SetFlagByName("fullscreen", g_pc_fullscreen.load() ? "true" : "false");
    // VSync and the frame rate only change how frames reach the display. The
    // game steps its simulation once per 60 Hz vblank, so its speed is fixed.
    rex::cvar::SetFlagByName("vsync", g_pc_vsync.load() ? "true" : "false");
    rex::cvar::SetFlagByName("guest_present_interval", g_pc_fps.load() == 30 ? "2" : "1");
    rex::cvar::SetFlagByName("window_width", std::to_string(g_pc_width.load()));
    rex::cvar::SetFlagByName("window_height", std::to_string(g_pc_height.load()));
    const int aa = g_pc_aa.load();
    rex::cvar::SetFlagByName("swap_post_effect",
        aa == 1 ? "fxaa" : aa == 2 ? "fxaa_extreme" : "none");
    if (auto set_effect = g_set_gpu_post_effect.load()) {
        set_effect(g_graphics_system.load(), aa);
    }
    if (!g_pc_fullscreen.load()) {
        if (HWND window = FindGameWindow()) {
            RECT rect{0, 0, g_pc_width.load(), g_pc_height.load()};
            AdjustWindowRect(&rect, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE);
            SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

void LoadPcSettings(bool apply = true)
{
    const auto settings_path = std::filesystem::path(ExecutableDirectory()) /
        L"userdata" / L"recompilation_settings.ini";
    std::ifstream file(settings_path);
    std::string line;
    while (std::getline(file, line)) {
        const auto split = line.find('=');
        if (split == std::string::npos) continue;
        const std::string key = line.substr(0, split);
        const int value = std::atoi(line.substr(split + 1).c_str());
        if (key == "width") g_pc_width = value;
        else if (key == "height") g_pc_height = value;
        else if (key == "fullscreen") g_pc_fullscreen = value != 0;
        else if (key == "vsync") g_pc_vsync = value != 0;
        // Older versions offered 120/144, which only sped the game up.
        else if (key == "fps") g_pc_fps = value == 30 ? 30 : 60;
        else if (key == "aa") g_pc_aa = value == 3 ? 1 : std::clamp(value, 0, 2);
        else if (key == "keyboard_player") g_keyboard_player = value == 2 ? 2 : 1;
    }
    if (apply) ApplyPcSettings();
}

void SavePcSettings()
{
    const auto settings_path = std::filesystem::path(ExecutableDirectory()) /
        L"userdata" / L"recompilation_settings.ini";
    std::error_code error;
    std::filesystem::create_directories(settings_path.parent_path(), error);
    std::ofstream file(settings_path, std::ios::trunc);
    file << "width=" << g_pc_width.load() << '\n'
         << "height=" << g_pc_height.load() << '\n'
         << "fullscreen=" << (g_pc_fullscreen.load() ? 1 : 0) << '\n'
         << "vsync=" << (g_pc_vsync.load() ? 1 : 0) << '\n'
         << "fps=" << g_pc_fps.load() << '\n'
         << "aa=" << g_pc_aa.load() << '\n'
         << "keyboard_player=" << g_keyboard_player.load() << '\n';
}

struct RemapBinding {
    std::string_view label;
    std::string_view flag;
    std::string_view default_key;
};

constexpr std::array<RemapBinding, 25> kRemapBindings{{
    {"Y", "keybind_y", "X"}, {"B", "keybind_b", "C"},
    {"X", "keybind_x", "Z"}, {"A", "keybind_a", "Space"},
    {"LB", "keybind_left_shoulder", "Q"},
    {"LT", "keybind_left_trigger", "Shift+Shift"},
    {"RB", "keybind_right_shoulder", "E"},
    {"RT", "keybind_right_trigger", "R"},
    {"START", "keybind_start", "Return"},
    {"BACK", "keybind_back", "Backspace"},
    {"MOVE UP", "keybind_lstick_up", "W"},
    {"MOVE DOWN", "keybind_lstick_down", "S"},
    {"MOVE LEFT", "keybind_lstick_left", "A"},
    {"MOVE RIGHT", "keybind_lstick_right", "D"},
    {"L STICK CLICK", "keybind_lstick_press", "F"},
    {"LOOK UP", "keybind_rstick_up", "Up"},
    {"LOOK DOWN", "keybind_rstick_down", "Down"},
    {"LOOK LEFT", "keybind_rstick_left", "Left"},
    {"LOOK RIGHT", "keybind_rstick_right", "Right"},
    {"R STICK CLICK", "keybind_rstick_press", "K"},
    {"D-PAD UP", "keybind_dpad_up", "Shift+Up"},
    {"D-PAD DOWN", "keybind_dpad_down", "Shift+Down"},
    {"D-PAD LEFT", "keybind_dpad_left", "Shift+Left"},
    {"D-PAD RIGHT", "keybind_dpad_right", "Shift+Right"},
    {"GUIDE", "keybind_guide", ""}
}};

bool SaveRemapBinding(int binding, std::string_view key)
{
    if (binding < 0 || binding >= static_cast<int>(kRemapBindings.size())) return false;
    if (!rex::cvar::SetFlagByName(kRemapBindings[static_cast<size_t>(binding)].flag, key)) return false;
    rex::cvar::SaveConfig(std::filesystem::path(ExecutableDirectory()) / L"stf_xbla.toml");
    return true;
}

void ResetRemapBindings()
{
    for (const auto& binding : kRemapBindings) {
        rex::cvar::SetFlagByName(binding.flag, binding.default_key);
    }
    rex::cvar::SaveConfig(std::filesystem::path(ExecutableDirectory()) / L"stf_xbla.toml");
    ++g_remap_reset_generation;
}

uint32_t AllocateGameBuffer(uint32_t size)
{
    auto* memory = g_guest_memory.load();
    return memory ? memory->SystemHeapAlloc(size) : 0;
}

void WriteGameUtf8(uint32_t address, size_t capacity, std::string_view text, uint8_t* base)
{
    if (!address || !capacity) return;
    const size_t length = (std::min)(text.size(), capacity - 1);
    for (size_t i = 0; i < capacity; ++i) {
        REX_STORE_U8(address + static_cast<uint32_t>(i),
                     i < length ? static_cast<uint8_t>(text[i]) : 0);
    }
}

uint32_t GetRecompStringPointer(uint32_t id, uint8_t* base)
{
    // Frame Rate keeps its ids (5308, 5309). 120/144 FPS are gone because the
    // game can't draw more than one frame per 60 Hz simulation step; their ids
    // (5310, 5311) now hold the Keyboard Player row.
    static constexpr std::array<std::string_view, 45> strings{{
        "Recompilation Settings", "RECOMPILATION SETTINGS",
        "Resolution", "HD", "FHD", "2K", "4K",
        "Screen Type", "Windowed", "Fullscreen",
        "Frame Rate", "30 FPS", "60 FPS", "", "",
        "VSync", "Off", "On",
        "Anti-Aliasing", "Off", "FXAA", "FXAA Extreme", "",
        "Choose the game resolution.",
        "Choose windowed or fullscreen display.",
        "Frames shown per second. Game speed stays the same.",
        "Sync to the display. Off may tear but lowers latency.",
        "Choose an anti-aliasing mode.",
        "Resolution: HD >", "Resolution: < FHD >", "Resolution: < 2K >", "Resolution: < 4K",
        "Screen Type: Windowed >", "Screen Type: < Fullscreen",
        "Frame Rate: 30 FPS >", "Frame Rate: < 60 FPS",
        "Keyboard: Player 1 >", "Keyboard: < Player 2",
        "VSync: Off >", "VSync: < On",
        "Anti-Aliasing: Off >", "Anti-Aliasing: < FXAA >",
        "Anti-Aliasing: < FXAA Extreme", "",
        ""
    }};
    if (id < 5274 || id > 5318) return 0;
    const size_t index = id - 5274;
    uint32_t& buffer = g_recomp_string_buffers[index];
    if (!buffer) {
        buffer = AllocateGameBuffer(128);
        if (!buffer) return 0;
        WriteGameUtf8(buffer, 128, strings[index], base);
    }
    return buffer;
}

void RefreshAchievementPage()
{
    const auto* kernel = g_achievement_kernel.load();
    auto achievements = kernel ? kernel->achievements().ListAchievements()
                               : std::vector<rex::system::AchievementInfo>{};
    std::sort(achievements.begin(), achievements.end(),
              [](const auto& left, const auto& right) { return left.id < right.id; });
    int unlocked_count = 0;
    for (const auto& achievement : achievements) {
        unlocked_count += kernel->achievements().IsUnlocked(achievement.id) ? 1 : 0;
    }
    const int pages = std::max(1, static_cast<int>((achievements.size() + 3) / 4));
    const int page = std::clamp(g_achievement_page.load(), 0, pages - 1);
    g_achievement_page = page;
    std::lock_guard lock(g_achievement_rows_mutex);
    g_achievement_row_text[0] = "ACHIEVEMENTS " + std::to_string(unlocked_count) +
                                "/" + std::to_string(achievements.size());
    for (int row = 0; row < 4; ++row) {
        const size_t index = static_cast<size_t>(page * 4 + row);
        if (index >= achievements.size()) {
            g_achievement_row_text[row + 1] = "";
            continue;
        }
        const auto& achievement = achievements[index];
        std::string name = achievement.label.empty()
            ? "Achievement " + std::to_string(achievement.id) : achievement.label;
        if (name.size() > 28) name = name.substr(0, 25) + "...";
        g_achievement_row_text[row + 1] = name + " - " +
            (kernel->achievements().IsUnlocked(achievement.id) ? "RECEIVED" : "NOT RECEIVED");
    }
    if (achievements.empty()) g_achievement_row_text[1] = "Achievement data unavailable";
    g_achievement_row_text[5] = std::string(page > 0 ? "< " : "  ") +
        "PAGE " + std::to_string(page + 1) + "/" + std::to_string(pages) +
        (page + 1 < pages ? " >" : "  ");
    const int selected = g_achievement_selected_row.load();
    const size_t selected_index = static_cast<size_t>(page * 4 + selected);
    if (selected == 4) {
        g_achievement_row_text[6] = "Left/Right: change page.";
    } else if (selected_index < achievements.size()) {
        const auto& achievement = achievements[selected_index];
        g_achievement_row_text[6] = achievement.description.empty()
            ? "Achievement status shown above." : achievement.description;
    } else {
        g_achievement_row_text[6].clear();
    }
    g_achievement_row_text[7].clear();
}

uint32_t GetAchievementStringPointer(uint32_t id, uint8_t* base)
{
    if (id < 5319 || id > 5326) return 0;
    const size_t index = id - 5319;
    uint32_t& buffer = g_achievement_row_buffers[index];
    if (!buffer) buffer = AllocateGameBuffer(128);
    if (!buffer) return 0;
    std::lock_guard lock(g_achievement_rows_mutex);
    WriteGameUtf8(buffer, 128, g_achievement_row_text[index], base);
    return buffer;
}

constexpr int kRemapRowsPerPage = 4;
constexpr int kRemapPageCount =
    (static_cast<int>(kRemapBindings.size()) + kRemapRowsPerPage - 1) / kRemapRowsPerPage;

int RemapBindingsOnPage(int page)
{
    return std::clamp(static_cast<int>(kRemapBindings.size()) - page * kRemapRowsPerPage,
                      0, kRemapRowsPerPage);
}

std::string RemapKeyDisplay(std::string key)
{
    if (key.empty()) return "UNBOUND";
    if (key == "Shift+Shift") return "Shift";
    if (key == "Control+Control") return "Control";
    if (key == "Alt+Alt") return "Alt";
    return key;
}

void RefreshRemapPage()
{
    const int page = std::clamp(g_remap_page.load(), 0, kRemapPageCount - 1);
    g_remap_page = page;
    const int visible = RemapBindingsOnPage(page);
    const int selected = g_remap_selected_row.load();
    const int captured = g_remap_capture_binding.load();
    std::lock_guard lock(g_remap_rows_mutex);
    g_remap_row_text[0] = "Remap Inputs";
    g_remap_row_text[1] = "REMAP INPUTS";
    for (int row = 0; row < kRemapRowsPerPage; ++row) {
        const int binding_index = page * kRemapRowsPerPage + row;
        if (row >= visible) {
            g_remap_row_text[2 + row].clear();
            continue;
        }
        const auto& binding = kRemapBindings[static_cast<size_t>(binding_index)];
        if (binding_index == captured) {
            g_remap_row_text[2 + row] = std::string(binding.label) + " : PRESS A KEY";
        } else {
            g_remap_row_text[2 + row] = std::string(binding.label) + " : " +
                RemapKeyDisplay(rex::cvar::GetFlagByName(binding.flag));
        }
    }
    g_remap_row_text[6] = std::string(page > 0 ? "< " : "  ") + "PAGE " +
        std::to_string(page + 1) + "/" + std::to_string(kRemapPageCount) +
        (page + 1 < kRemapPageCount ? " >" : "  ");
    if (captured >= 0) {
        g_remap_row_text[7] = "Press a key or mouse button. Esc cancels. F6 resets.";
    } else if (!g_remap_status.empty()) {
        g_remap_row_text[7] = g_remap_status;
    } else if (selected == 4) {
        g_remap_row_text[7] = "Left/Right: page. A: next page. B: close. F6: reset.";
    } else {
        g_remap_row_text[7] = "A: choose key. B: close. F6: reset all controls.";
    }
}

uint32_t GetRemapStringPointer(uint32_t id, uint8_t* base)
{
    if (id < 5327 || id > 5334) return 0;
    const size_t index = id - 5327;
    uint32_t& buffer = g_remap_string_buffers[index];
    if (!buffer) buffer = AllocateGameBuffer(128);
    if (!buffer) return 0;
    std::lock_guard lock(g_remap_rows_mutex);
    WriteGameUtf8(buffer, 128, g_remap_row_text[index], base);
    return buffer;
}

void ChangeRemapPage(int direction)
{
    const int next = std::clamp(g_remap_page.load() + direction, 0, kRemapPageCount - 1);
    if (next == g_remap_page.load()) return;
    g_remap_page = next;
    g_remap_selected_row = 4;
    g_remap_status.clear();
    RefreshRemapPage();
}

constexpr std::array<int, kPcSettingRows> kPcMenuMaxima{{3, 1, 1, 1, 2, 1}};

void InitializePcMenuValues()
{
    std::lock_guard lock(g_pc_setting_values_mutex);
    if (g_pc_setting_values_initialized) return;
    g_pc_setting_values = {{
        g_pc_width.load() >= 3840 ? 3 : g_pc_width.load() >= 2560 ? 2 :
            g_pc_width.load() >= 1920 ? 1 : 0,
        g_pc_fullscreen.load() ? 1 : 0,
        g_pc_fps.load() >= 60 ? 1 : 0,
        g_pc_vsync.load() ? 1 : 0,
        std::clamp(g_pc_aa.load(), 0, 2),
        g_keyboard_player.load() == 2 ? 1 : 0,
    }};
    g_pc_setting_values_initialized = true;
}

void AdjustPcMenuValue(int row, int direction)
{
    if (row < 0 || row >= static_cast<int>(kPcMenuMaxima.size())) return;
    InitializePcMenuValues();
    {
        std::lock_guard lock(g_pc_setting_values_mutex);
        int& value = g_pc_setting_values[static_cast<size_t>(row)];
        const int next = std::clamp(value + direction, 0, kPcMenuMaxima[static_cast<size_t>(row)]);
        if (next == value) return;
        value = next;
        static constexpr int widths[4] = {1280, 1920, 2560, 3840};
        static constexpr int heights[4] = {720, 1080, 1440, 2160};
        static constexpr int limits[2] = {30, 60};
        g_pc_width = widths[g_pc_setting_values[0]];
        g_pc_height = heights[g_pc_setting_values[0]];
        g_pc_fullscreen = g_pc_setting_values[1] != 0;
        g_pc_fps = limits[g_pc_setting_values[2]];
        g_pc_vsync = g_pc_setting_values[3] != 0;
        g_pc_aa = g_pc_setting_values[4];
        g_keyboard_player = g_pc_setting_values[5] ? 2 : 1;
        REXLOG_INFO("Recomp menu changed row {} to {}", row, next);
    }
    SavePcSettings();
    ApplyPcSettings();
}

void CloseRecompSettings()
{
    uint8_t* base = g_guest_base.load();
    g_recomp_settings_open = false;
    g_pc_selected_row = 0;
    if (const uint32_t object = g_help_options_object.load(); object && base)
    {
        REX_STORE_U32(object + 76, 3);
    }
}

void CloseRemapInputs()
{
    uint8_t* base = g_guest_base.load();
    g_remap_open = false;
    g_remap_capture_binding = -1;
    g_remap_capture_activation_key = 0;
    g_remap_selected_row = 0;
    g_remap_status.clear();
    if (const uint32_t object = g_help_options_object.load(); object && base) {
        REX_STORE_U32(object + 76, 4);
    }
}

void StartRemapCapture(WPARAM activation_key)
{
    const int binding = g_remap_page.load() * kRemapRowsPerPage + g_remap_selected_row.load();
    if (binding < 0 || binding >= static_cast<int>(kRemapBindings.size())) return;
    g_remap_capture_binding = binding;
    g_remap_capture_activation_key = activation_key;
    g_remap_status.clear();
    RefreshRemapPage();
}

void AssignRemapKey(std::string key)
{
    const int binding = g_remap_capture_binding.load();
    if (key == "Shift") key = "Shift+Shift";
    else if (key == "Control") key = "Control+Control";
    else if (key == "Alt") key = "Alt+Alt";
    if (SaveRemapBinding(binding, key)) {
        g_remap_status = std::string(kRemapBindings[static_cast<size_t>(binding)].label) +
            " set to " + RemapKeyDisplay(key) + ". Saved.";
    } else {
        g_remap_status = "Could not save this binding.";
    }
    g_remap_capture_binding = -1;
    g_remap_capture_activation_key = 0;
    RefreshRemapPage();
}

bool IsHelpOptionsWindowActive(HWND window)
{
    const ULONGLONG last_draw = g_help_options_draw_tick.load();
    return g_help_options_active.load() &&
        g_help_options_object.load() && g_guest_base.load() && IsWindow(window) &&
        GetForegroundWindow() == window && last_draw &&
        GetTickCount64() - last_draw < 300;
}

LRESULT CALLBACK GameWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    static WNDPROC original = reinterpret_cast<WNDPROC>(GetPropW(window, L"STFR_OriginalWndProc"));
	uint8_t* base = g_guest_base.load();
	static std::array<bool, 256> blocked_key_releases{};
	static bool blocked_mouse_release = false;
	const bool key_down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
	const bool key_up = message == WM_KEYUP || message == WM_SYSKEYUP;
	if ((key_up && g_remap_capture_activation_key.load() == wparam) ||
	    (message == WM_LBUTTONUP && g_remap_capture_activation_key.load() == VK_LBUTTON)) {
		g_remap_capture_activation_key = 0;
	}
	if (key_up && wparam < blocked_key_releases.size() && blocked_key_releases[wparam]) {
		blocked_key_releases[wparam] = false;
		return 0;
	}
	if (blocked_mouse_release &&
	    (message == WM_LBUTTONUP || message == WM_RBUTTONUP)) {
		blocked_mouse_release = false;
		return 0;
	}
	if ((g_recomp_settings_open.load() || g_remap_open.load()) &&
	    (message == WM_KILLFOCUS || message == WM_ACTIVATEAPP) &&
	    !g_reset_dialog_open.load() &&
	    (message == WM_KILLFOCUS || wparam == FALSE)) {
		if (g_recomp_settings_open.load()) CloseRecompSettings();
		if (g_remap_open.load()) CloseRemapInputs();
		g_help_options_active = false;
		g_help_options_draw_tick = 0;
	}
	if (g_achievement_entering.load() && (key_up || message == WM_LBUTTONUP)) {
		g_achievement_entering = false;
		return CallWindowProcW(original, window, key_up ? message : WM_KEYUP,
		                       key_up ? wparam : VK_SPACE, lparam);
	}
	if (key_up && g_achievement_back_key.load() == wparam) {
		g_achievement_back_key = 0;
		return CallWindowProcW(original, window, WM_KEYUP, 'C', lparam);
	}
	if ((message == WM_LBUTTONUP && g_achievement_back_key.load() == VK_LBUTTON) ||
	    (message == WM_RBUTTONUP && g_achievement_back_key.load() == VK_RBUTTON)) {
		g_achievement_back_key = 0;
		return CallWindowProcW(original, window, WM_KEYUP, 'C', lparam);
	}
	if (g_pause_achievements_open.load()) {
		if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN) {
			g_pause_achievements_open = false;
			blocked_mouse_release = true;
			return 0;
		}
		if (message == WM_LBUTTONUP || message == WM_RBUTTONUP) return 0;
		if (key_down) {
			const int row = g_achievement_selected_row.load();
			if (wparam == VK_ESCAPE || wparam == VK_BACK || wparam == 'C' ||
			    wparam == VK_RETURN || wparam == VK_SPACE) {
				g_pause_achievements_open = false;
				if (wparam < blocked_key_releases.size()) blocked_key_releases[wparam] = true;
				return 0;
			}
			if (wparam == VK_UP || wparam == 'W') {
				g_achievement_selected_row = std::max(0, row - 1);
				RefreshAchievementPage();
				return CallWindowProcW(original, window, message, wparam, lparam);
			}
			if (wparam == VK_DOWN || wparam == 'S') {
				g_achievement_selected_row = std::min(5, row + 1);
				RefreshAchievementPage();
				return CallWindowProcW(original, window, message, wparam, lparam);
			}
			if (wparam < blocked_key_releases.size()) blocked_key_releases[wparam] = true;
			if (wparam == VK_LEFT || wparam == 'A')
				g_achievement_page = std::max(0, g_achievement_page.load() - 1);
			else if (wparam == VK_RIGHT || wparam == 'D' ||
			         ((wparam == VK_RETURN || wparam == VK_SPACE) && row == 4))
				++g_achievement_page;
			RefreshAchievementPage();
			return 0;
		}
		if (key_up) return CallWindowProcW(original, window, message, wparam, lparam);
	}
	if (g_achievements_open.load()) {
		if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN) {
			g_achievements_open = false;
			g_help_options_active = false;
			g_achievement_back_key = message == WM_LBUTTONDOWN ? VK_LBUTTON : VK_RBUTTON;
			return CallWindowProcW(original, window, WM_KEYDOWN, 'C', 0);
		}
		if (message == WM_LBUTTONUP || message == WM_RBUTTONUP) return 0;
		if (key_down) {
			const int row = g_achievement_selected_row.load();
			if (wparam == VK_ESCAPE || wparam == VK_BACK || wparam == 'C' ||
			    wparam == VK_RETURN || wparam == VK_SPACE) {
				g_achievements_open = false;
				g_help_options_active = false;
				g_achievement_back_key = wparam;
				return CallWindowProcW(original, window, WM_KEYDOWN, 'C', lparam);
			}
			if (wparam < blocked_key_releases.size()) blocked_key_releases[wparam] = true;
			if (wparam == VK_UP || wparam == 'W')
				g_achievement_selected_row = std::max(0, row - 1);
			else if (wparam == VK_DOWN || wparam == 'S')
				g_achievement_selected_row = std::min(4, row + 1);
			else if (wparam == VK_LEFT || wparam == 'A') {
				g_achievement_page = std::max(0, g_achievement_page.load() - 1);
				RefreshAchievementPage();
			}
			else if (wparam == VK_RIGHT || wparam == 'D' ||
			         ((wparam == VK_RETURN || wparam == VK_SPACE) && row == 4)) {
				++g_achievement_page;
				RefreshAchievementPage();
			}
			RefreshAchievementPage();
			return 0;
		}
		if (key_up) return 0;
	}
	if ((key_down && (wparam == VK_RETURN || wparam == VK_SPACE)) ||
	    message == WM_LBUTTONDOWN) {
		const uint32_t main_menu = g_main_menu_object.load();
		if (base && main_menu && GetTickCount64() - g_main_menu_draw_tick.load() < 250 &&
		    REX_LOAD_U32(main_menu + 76) == 4) {
			g_achievement_page = 0;
			g_achievement_selected_row = 0;
			RefreshAchievementPage();
			g_achievements_open = true;
			g_achievement_entering = true;
			REX_STORE_U32(main_menu + 76, 5);
			REXLOG_INFO("Opened achievements in the game's full-size menu scene");
			if (message == WM_LBUTTONDOWN)
				return CallWindowProcW(original, window, WM_KEYDOWN, VK_SPACE, 0);
			return CallWindowProcW(original, window, message, wparam, lparam);
		}
	}
	if (g_recomp_settings_open.load()) {
		if (!IsHelpOptionsWindowActive(window)) {
			CloseRecompSettings();
			g_help_options_active = false;
			g_help_options_draw_tick = 0;
		} else {
		if (message == WM_LBUTTONDOWN) {
			CloseRecompSettings();
			return 0;
		}
		if (message == WM_LBUTTONUP) return 0;
		if (message == WM_RBUTTONDOWN) {
			CloseRecompSettings();
			return 0;
		}
		if (message == WM_RBUTTONUP) return 0;
		if (key_down) {
			if (wparam < blocked_key_releases.size()) blocked_key_releases[wparam] = true;
			if (wparam == VK_F6) {
				ResetRemapBindings();
				g_reset_dialog_open = true;
				MessageBoxW(window, L"Controls were reset to the default layout.",
				            L"Sonic The Fighters Recompiled", MB_OK | MB_ICONINFORMATION);
				g_reset_dialog_open = false;
				return 0;
			}
			int row = g_pc_selected_row.load();
			switch (wparam) {
			case VK_UP: case 'W': g_pc_selected_row = std::max(0, row - 1); break;
			case VK_DOWN: case 'S': g_pc_selected_row = std::min(kPcSettingRows - 1, row + 1); break;
			case VK_LEFT: case 'A': AdjustPcMenuValue(row, -1); break;
			case VK_RIGHT: case 'D': AdjustPcMenuValue(row, 1); break;
			case VK_SPACE: case VK_RETURN:
			case 'C':
				CloseRecompSettings();
				break;
			default: break;
			}
			if (!g_recomp_settings_open.load()) REXLOG_INFO("Recomp menu closed");
			return 0;
		}
		if (key_up) return 0;
		}
	}
	if (g_remap_open.load()) {
		if (!IsHelpOptionsWindowActive(window)) {
			CloseRemapInputs();
			g_help_options_active = false;
			g_help_options_draw_tick = 0;
		} else {
			if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
			    message == WM_MBUTTONDOWN) {
				if (g_remap_capture_binding.load() >= 0) {
					if (!g_remap_capture_activation_key.load()) {
						AssignRemapKey(message == WM_LBUTTONDOWN ? "LMB" :
						               message == WM_RBUTTONDOWN ? "RMB" : "MMB");
					}
				} else if (message == WM_RBUTTONDOWN) {
					CloseRemapInputs();
				} else if (message == WM_LBUTTONDOWN) {
					if (g_remap_selected_row.load() == 4) ChangeRemapPage(1);
					else StartRemapCapture(VK_LBUTTON);
				}
				return 0;
			}
			if (message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
			    message == WM_MBUTTONUP) return 0;
			if (key_down) {
				if (wparam < blocked_key_releases.size()) blocked_key_releases[wparam] = true;
				if (wparam == VK_F6) {
					g_remap_capture_binding = -1;
					g_remap_capture_activation_key = 0;
					ResetRemapBindings();
					g_remap_status = "All controls reset to defaults and saved.";
					RefreshRemapPage();
					g_reset_dialog_open = true;
					MessageBoxW(window, L"Controls were reset to the default layout.",
					            L"Sonic The Fighters Recompiled", MB_OK | MB_ICONINFORMATION);
					g_reset_dialog_open = false;
					return 0;
				}
				if (g_remap_capture_binding.load() >= 0) {
					if (g_remap_capture_activation_key.load() ||
					    (lparam & (1LL << 30))) return 0;
					if (wparam == VK_ESCAPE) {
						g_remap_capture_binding = -1;
						g_remap_status = "Key selection cancelled.";
						RefreshRemapPage();
						return 0;
					}
					const std::string key = rex::ui::VirtualKeyToString(
					    static_cast<rex::ui::VirtualKey>(wparam));
					if (key.empty()) {
						g_remap_status = "Unsupported key. Choose another key.";
						RefreshRemapPage();
						return 0;
					}
					AssignRemapKey(key);
					return 0;
				}
				const int row = g_remap_selected_row.load();
				const int visible = RemapBindingsOnPage(g_remap_page.load());
				switch (wparam) {
				case VK_UP: case 'W':
					g_remap_selected_row = row == 4 ? visible - 1 : std::max(0, row - 1);
					break;
				case VK_DOWN: case 'S':
					g_remap_selected_row = row >= visible - 1 ? 4 : row + 1;
					break;
				case VK_LEFT: case 'A':
					if (row == 4) ChangeRemapPage(-1);
					break;
				case VK_RIGHT: case 'D':
					if (row == 4) ChangeRemapPage(1);
					break;
				case VK_RETURN: case VK_SPACE:
					if (row == 4) ChangeRemapPage(1);
					else StartRemapCapture(wparam);
					break;
				case 'C': case VK_ESCAPE: case VK_BACK:
					CloseRemapInputs();
					break;
				default: break;
				}
				if (g_remap_open.load()) {
					g_remap_status.clear();
					RefreshRemapPage();
				}
				return 0;
			}
			if (key_up) return 0;
		}
	}
	auto route_help_options = [base, window]() -> bool {
		if (!g_help_options_active.load()) return false;
		const ULONGLONG last_draw = g_help_options_draw_tick.load();
		if (!last_draw || GetTickCount64() - last_draw >= 300 ||
		    GetForegroundWindow() != window) return false;
		const uint32_t object = g_help_options_object.load();
		if (!object || !base) return false;
		const uint32_t selected = REX_LOAD_U32(object + 76);
		if (selected == 3) {
			InitializePcMenuValues();
			g_pc_selected_row = 0;
			g_recomp_settings_open = true;
			REXLOG_INFO("Opened Recomp menu inside Help & Options scene");
			return true;
		}
		if (selected == 4) {
			g_remap_page = 0;
			g_remap_selected_row = 0;
			g_remap_capture_binding = -1;
			g_remap_capture_activation_key = 0;
			g_remap_status.clear();
			RefreshRemapPage();
			g_remap_open = true;
			REXLOG_INFO("Opened Remap Inputs inside Help & Options scene");
			return true;
		}
		if (selected < 2 || selected > 6) return false;
		REX_STORE_U32(object + 76, StfrMapHelpOptionsSelection(selected));
		g_help_options_active = false;
		return false;
	};
	// Mouse buttons also work as the two main buttons of gamepad
	if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) {
		if (message == WM_LBUTTONDOWN && route_help_options()) return 0;
		const UINT key_message = message == WM_LBUTTONDOWN ? WM_KEYDOWN : WM_KEYUP;
		CallWindowProcW(original, window, key_message, VK_SPACE, 0);
	}
	if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) {
		const UINT key_message = message == WM_RBUTTONDOWN ? WM_KEYDOWN : WM_KEYUP;
		CallWindowProcW(original, window, key_message, 'C', 0);
	}
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && wparam == VK_F6) {
        if ((lparam & (1LL << 30)) == 0) {
            ResetRemapBindings();
            g_reset_dialog_open = true;
            MessageBoxW(window,
                        L"Controls were reset to the default layout.",
                        L"Sonic The Fighters Recompiled",
                        MB_OK | MB_ICONINFORMATION);
            g_reset_dialog_open = false;
        }
        return 0;
    }
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
        (wparam == VK_SPACE || wparam == VK_RETURN)) {
		if (route_help_options()) {
			blocked_key_releases[wparam] = true;
			return 0;
		}
	}
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_KEYUP ||
         message == WM_SYSKEYUP) && wparam >= VK_F1 && wparam <= VK_F12) {
        return 0;
    }
    return CallWindowProcW(original, window, message, wparam, lparam);
}

void DisableDebugKeys(HWND window)
{
    if (!window || GetPropW(window, L"STFR_OriginalWndProc")) return;
    WNDPROC original = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
    SetPropW(window, L"STFR_OriginalWndProc", reinterpret_cast<HANDLE>(original));
    SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GameWindowProc));
}

struct DebugKeyBlocker {
    DebugKeyBlocker() {
        LoadPcSettings(false);
        std::thread([] {
            for (int attempt = 0; attempt < 600; ++attempt) {
                if (HWND window = FindGameWindow()) {
                    DisableDebugKeys(window);
                    LoadPcSettings();
                    for (int title_attempt = 0; title_attempt < 110; ++title_attempt) {
                        if (!IsWindow(window)) break;
                        SetWindowTextW(window, L"Sonic the Fighters Recompiled");
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }).detach();
    }
} g_debug_key_blocker;

// Like the SDK's SlotAssignment (pad N is player N+1, keyboard shares player 1)
// but can move the keyboard to player 2, so one person on the keyboard can play
// against one on a controller. Menus only listen to player 1, so the keyboard
// stays there while no controller is connected.
class StfrDeviceAssignment final : public rex::input::DeviceAssignment {
public:
    void OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) override
    {
        std::lock_guard lock(mutex_);
        devices_ = devices;
    }

    void DevicesForUser(uint32_t user_index, std::vector<rex::input::DeviceId>& out) const override
    {
        out.clear();
        std::lock_guard lock(mutex_);
        const bool has_pad = std::any_of(devices_.begin(), devices_.end(),
                                         [](const auto& device) { return !device.synthetic; });
        const uint32_t keyboard_user = g_keyboard_player.load() == 2 && has_pad ? 1 : 0;
        for (const auto& device : devices_) {
            uint32_t user = device.ordinal;
            if (device.synthetic) {
                user = device.name == "Keyboard and Mouse" ? keyboard_user : 0;
            }
            if (user == user_index) out.push_back(device.id);
        }
    }

private:
    mutable std::mutex mutex_;
    std::vector<rex::input::DeviceInfo> devices_;
};

}

REX_DEFINE_APP(stf_xbla, StfXblaApp::Create)

void StfXblaApp::OnConfigurePaths(rex::PathConfig& paths)
{
    const std::filesystem::path root(ExecutableDirectory());
    const std::filesystem::path legacy_achievement =
        paths.metadata_root / L"achievements" / L"5841129E.toml";
    const std::filesystem::path legacy_named_achievement =
        paths.metadata_root / L"stf_xbla" / L"achievements" / L"5841129E.toml";
    const std::filesystem::path metadata = root / L"userdata" / L"stf_xbla";
    const std::filesystem::path local_achievement =
        metadata / L"achievements" / L"5841129E.toml";

    std::error_code error;
    std::filesystem::create_directories(local_achievement.parent_path(), error);
    if (!std::filesystem::exists(local_achievement, error)) {
        const std::filesystem::path source = std::filesystem::exists(legacy_achievement, error)
            ? legacy_achievement : legacy_named_achievement;
        if (std::filesystem::exists(source, error)) {
            std::filesystem::copy_file(source, local_achievement,
                                       std::filesystem::copy_options::overwrite_existing, error);
        }
    }

    std::filesystem::path game_data_root = root / L"_unpacked";
    if (!std::filesystem::exists(game_data_root / L"default.xex")) {
        for (std::filesystem::path candidate = root.parent_path(); !candidate.empty(); candidate = candidate.parent_path()) {
            if (std::filesystem::exists(candidate / L"_unpacked" / L"default.xex")) {
                game_data_root = candidate / L"_unpacked";
                break;
            }
            if (candidate == candidate.root_path()) break;
        }
    }

    paths.game_data_root = game_data_root;
    paths.user_data_root = root / L"userdata";
    paths.cache_root = root / L"userdata" / L"cache";
    paths.metadata_root = metadata;
    paths.config_path = root / L"stf_xbla.toml";
}

void StfXblaApp::OnPreSetup(rex::RuntimeConfig& config)
{
    if (rex::cvar::GetFlagSource("mnk_mode") == rex::cvar::Source::kDefault) {
        if (rex::cvar::SetFlagByName("mnk_mode", "true")) {
            REXLOG_INFO("Keyboard/mouse input enabled for direct launch");
        }
    }
    LoadOriginalScreenSize();

    // 1 ms timer resolution for the runtime's short sleeps (GPU waits, audio);
    // the Windows default of 15.6 ms makes frame pacing uneven.
    timeBeginPeriod(1);

    if (config.gpu_plugin.empty()) {
        config.gpu_plugin = "xenos";
    }

    config.input_factory = [](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
        auto input = rex::input::CreateDefaultInputSystem(tool_mode);
        input->SetDeviceAssignment(std::make_unique<StfrDeviceAssignment>());
        return input;
    };

    ApplyPcSettings();
}

void StfXblaApp::OnPostSetup()
{
    g_guest_memory = runtime()->memory();
    g_guest_base = runtime()->memory()->virtual_membase();
    g_achievement_kernel = runtime()->kernel_state();
    g_graphics_system = runtime()->graphics_system();
    // Experimental path tracing (GPU plugin, registered by now): the scene's
    // projection scales are in vertex shader constants c72.x and c73.y.
    if (rex::cvar::GetFlagSource("path_tracing_projection_constant") ==
        rex::cvar::Source::kDefault) {
        rex::cvar::SetFlagByName("path_tracing_projection_constant", "72");
    }
    HMODULE gpu_plugin = GetModuleHandleW(L"rexgpu-xenos.dll");
    g_set_gpu_post_effect = gpu_plugin
        ? reinterpret_cast<SetGpuPostEffect>(GetProcAddress(gpu_plugin, "stfr_set_gpu_post_effect"))
        : nullptr;
    if (!g_set_gpu_post_effect.load()) {
        REXLOG_ERROR("GPU plugin does not provide live anti-aliasing control");
    }
    ApplyPcSettings();
}

void StfXblaApp::OnShutdown()
{
    g_achievement_kernel = nullptr;
    g_set_gpu_post_effect = nullptr;
    g_graphics_system = nullptr;
    g_guest_memory = nullptr;
    timeEndPeriod(1);
}

void StfXblaApp::OnPreLaunchModule()
{
}

void StfrPrepareNativePcSettings(uint32_t object, uint8_t* base)
{
    // Watch the Screen Size setting and update the display right away
    if (object && base) {
        const uint32_t size = REX_LOAD_U32(object + 460);
        if (size <= 2) SetOriginalScreenSize(static_cast<int>(size), true);
    }
    g_help_options_active = false;
    g_help_options_draw_tick = 0;
    g_recomp_settings_open = false;
    g_remap_open = false;
    g_remap_capture_binding = -1;
}

// Rows the Help & Options scene shows on its current page.
int HelpOptionsRowCount()
{
    if (g_achievements_open.load() || g_remap_open.load()) return 5;
    if (g_recomp_settings_open.load()) return kPcSettingRows;
    return 7;
}

// Menus size their panel by opening it with the window animation authored for
// their row count: panel type 9 + rows (the main menu's 7 rows use type 16).
// Help & Options was authored for 5 rows, so the port's pages need others.
uint32_t PanelTypeForRows(int rows)
{
    return static_cast<uint32_t>(9 + rows);
}

void StfrPrepareHelpOptions(uint32_t object, uint32_t labels, uint8_t* base)
{
    g_help_options_draw_tick = GetTickCount64();
    if (g_achievements_open.load()) {
        for (uint32_t row = 0; row < 5; ++row) {
            REX_STORE_U32(labels + row * 4, 5320 + row);
            REX_STORE_U8(object + 565 + row, 0);
        }
        REX_STORE_U32(object + 436, 4);
        REX_STORE_U32(object + 76,
                      static_cast<uint32_t>(g_achievement_selected_row.load()));
        g_help_options_object = object;
        g_help_options_active = true;
        return;
    }
    if (g_recomp_settings_open.load()) {
        InitializePcMenuValues();
        std::array<int, kPcSettingRows> values;
        {
            std::lock_guard lock(g_pc_setting_values_mutex);
            values = g_pc_setting_values;
        }
        constexpr uint32_t first_ids[kPcSettingRows] = {5302, 5306, 5308, 5312, 5314, 5310};
        for (int i = 0; i < kPcSettingRows; ++i) {
            REX_STORE_U32(labels + i * 4, first_ids[i] + static_cast<uint32_t>(values[i]));
            REX_STORE_U8(object + 565 + i, 0);
        }
        REX_STORE_U32(object + 436, kPcSettingRows - 1);
        REX_STORE_U32(object + 76, static_cast<uint32_t>(g_pc_selected_row.load()));
        g_help_options_object = object;
        g_help_options_active = true;
        return;
    }
    if (g_remap_open.load()) {
        const int page = g_remap_page.load();
        const int visible = RemapBindingsOnPage(page);
        for (int row = 0; row < kRemapRowsPerPage; ++row) {
            REX_STORE_U32(labels + row * 4, 5329 + row);
            REX_STORE_U8(object + 565 + row, row < visible ? 0 : 2);
        }
        REX_STORE_U32(labels + 16, 5333);
        REX_STORE_U8(object + 569, 0);
        REX_STORE_U8(object + 570, 2);
        REX_STORE_U32(object + 436, 4);
        REX_STORE_U32(object + 76, static_cast<uint32_t>(g_remap_selected_row.load()));
        g_help_options_object = object;
        g_help_options_active = true;
        return;
    }
    g_recomp_settings_open = false;

    // Add two pc pages
    constexpr uint32_t rows[7] = {419, 420, 421, 5274, 5327, 422, 423};
    for (uint32_t i = 0; i < 7; ++i) {
        REX_STORE_U32(labels + i * 4, rows[i]);
        REX_STORE_U8(object + 565 + i, 0);
    }
    REX_STORE_U32(object + 436, 6);
    g_help_options_object = object;
    g_help_options_selection = REX_LOAD_U32(object + 76);
    g_help_options_active = true;
}

uint32_t StfrMapHelpOptionsSelection(uint32_t selection)
{
    // Keep the original menu
    uint32_t mapped = selection;
    switch (selection) {
    case 2:
        g_recomp_settings_open = false;
        g_remap_open = false;
        mapped = 2;
        break;
    case 3: case 4:
        mapped = selection;
        break;
    case 5:
        mapped = 3;
        break;
    case 6:
        mapped = 4;
        break;
    default:
        break;
    }
    return mapped;
}

uint32_t StfrResolvePauseAchievementString(uint32_t id, uint8_t* base)
{
    if (!g_pause_achievements_open.load()) return 0;
    uint32_t replacement = 0;
    switch (id) {
    case 190: case 191: case 192: replacement = 5319; break;
    case 193: replacement = 5320; break; // 1 tag
    case 195: replacement = 5321; break; // 2 tag
    case 186: replacement = 5322; break; // 3 tag
    case 183: replacement = 5323; break; // 4 tag
    case 185: replacement = 5324; break; // pages.
    case 194: replacement = 5326; break; // btn back.
    default: return 0;
    }
    return GetAchievementStringPointer(replacement, base);
}

uint32_t StfrResolveMenuDescription(uint8_t* base)
{
    if (g_remap_open.load()) return GetRemapStringPointer(5334, base);
    if (g_achievements_open.load()) return GetAchievementStringPointer(5325, base);
    std::string description;
    if (g_recomp_settings_open.load()) {
        constexpr std::string_view descriptions[kPcSettingRows] = {
            "Choose the game resolution.",
            "Choose windowed or fullscreen display.",
            "Frames shown per second. Game speed stays the same.",
            "Sync to the display. Off may tear but lowers latency.",
            "Choose an anti-aliasing mode.",
            "Player 2 lets the keyboard play against a controller."
        };
        description = descriptions[static_cast<size_t>(
            std::clamp(g_pc_selected_row.load(), 0, kPcSettingRows - 1))];
    } else if (g_help_options_active.load() && g_help_options_object.load() &&
               GetTickCount64() - g_help_options_draw_tick.load() < 300) {
        switch (REX_LOAD_U32(g_help_options_object.load() + 76)) {
        case 3: description = "Configure PC display and graphics settings."; break;
        case 4: description = "Configure keyboard and mouse controls."; break;
        case 5: description = "Manage save data."; break;
        case 6: description = "View credits."; break;
        default: break;
        }
    }
    if (description.empty()) return 0;
    {
        std::lock_guard lock(g_remap_rows_mutex);
        g_remap_row_text[7] = std::move(description);
    }
    return GetRemapStringPointer(5334, base);
}

// Mid-asm hooks declared in stf_xbla_manifest.toml. The generated code passes
// only the listed registers, so guest memory goes through g_guest_base.

void StfrPrepareHelpOptionsHook(PPCRegister& r29, PPCRegister& r1, PPCRegister& r13)
{
    uint8_t* base = g_guest_base.load();
    if (!base) return;
    StfrPrepareHelpOptions(r29.u32, r1.u32 + 208, base);

    // Switching pages changes the row count, so reopen the panel at the matching
    // size, the same way the game opens a menu. Only while it is fully open
    // (state 3), which leaves the closing animation alone.
    const uint32_t panel = r29.u32 + 508;
    const uint32_t type = PanelTypeForRows(HelpOptionsRowCount());
    if (REX_LOAD_U32(panel) == 3 && REX_LOAD_U32(panel + 4) != type) {
        PPCContext ctx{};
        ctx.r1 = r1;
        ctx.r13 = r13;
        ctx.fpscr.csr = ctx.fpscr.getcsr();
        ctx.r3.u64 = panel;
        ctx.r4.u64 = type;
        sub_82126948(ctx, base);
    }
}

void StfrHelpOptionsPanelTypeHook(PPCRegister& r4)
{
    r4.u64 = PanelTypeForRows(HelpOptionsRowCount());
}

void StfrHelpOptionsTitleHook(PPCRegister& r3)
{
    if (g_achievements_open.load()) r3.u64 = 5319;
    else if (g_remap_open.load()) r3.u64 = 5328;
    else if (g_recomp_settings_open.load()) r3.u64 = 5275;
}

void StfrPrepareNativeSettingsHook(PPCRegister& r27)
{
    StfrPrepareNativePcSettings(r27.u32, g_guest_base.load());
}

void StfrCaptureMainMenuHook(PPCRegister& r29)
{
    g_main_menu_object = r29.u32;
    g_main_menu_draw_tick = GetTickCount64();
}

bool StfrResolveStringHook(PPCRegister& r3)
{
    uint8_t* base = g_guest_base.load();
    if (!base) return false;
    const uint32_t id = r3.u32;
    if (const uint32_t replacement = StfrResolvePauseAchievementString(id, base)) {
        r3.u64 = replacement;
        return true;
    }
    if (id >= 117 && id <= 123) {
        if (const uint32_t description = StfrResolveMenuDescription(base)) {
            r3.u64 = description;
            return true;
        }
    }
    if (id >= 5274 && id <= 5334) {
        if (id >= 5327) r3.u64 = GetRemapStringPointer(id, base);
        else if (id >= 5319) r3.u64 = GetAchievementStringPointer(id, base);
        else r3.u64 = GetRecompStringPointer(id, base);
        return true;
    }
    return false;
}

void StfrOpenPauseAchievementsHook(PPCRegister& r3)
{
    g_achievement_page = 0;
    g_achievement_selected_row = 4;
    RefreshAchievementPage();
    g_pause_achievements_open = true;
    REXLOG_INFO("Opened achievements inside the game's pause menu");
    // Keep the in-game pause menu active instead of calling the Xbox Guide.
    r3.u64 = 1;
}

// checks online (TO DO)
void __imp__XamUserCheckPrivilege(PPCContext& ctx, uint8_t* base)
{
    const uint32_t privilege = ctx.r4.u32;
    const bool is_online_privilege = privilege == 251 || privilege == 252;

    REX_STORE_U32(ctx.r5.u32, is_online_privilege ? 0 : 1);
    REXLOG_INFO("XamUserCheckPrivilege({}): {}", privilege,
                is_online_privilege ? "unavailable" : "available");
    
    ctx.r3.u64 = 0;
}

void __imp__XamShowAchievementsUI(PPCContext& ctx, uint8_t* base)
{
    REXLOG_WARN("Achievements import reached without an in-game menu route");
    ctx.r3.u64 = 0;
}

// make a full game
void __imp__XamContentGetLicenseMask(PPCContext& ctx, uint8_t* base) {
REXLOG_INFO("Intercepted XamContentGetLicenseMask!");
    
    uint32_t mask_ptr = ctx.r3.u32;

    REX_STORE_U32(mask_ptr, 0xFFFFFFFF);

    ctx.r3.u64 = 0;
}
