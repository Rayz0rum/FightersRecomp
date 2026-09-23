#include "generated/default/stf_xbla_init.h"
#include <rex/ppc.h>
#include <rex/image_info.h>
#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/function_dispatcher.h>
#include <rex/ui/keybinds.h>
#include <Windows.h>
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

uint32_t StfrMapHelpOptionsSelection(uint32_t selection);

namespace {
std::atomic<int> g_pc_width{1920}, g_pc_height{1080}, g_pc_fps{60};
std::atomic<bool> g_pc_fullscreen{true}, g_pc_vsync{true};
std::atomic<int> g_pc_aa{1};
std::atomic<int> g_original_screen_size{-1};
using SetGpuPostEffect = void (*)(void*, int);
std::atomic<void*> g_graphics_system{nullptr};
std::atomic<SetGpuPostEffect> g_set_gpu_post_effect{nullptr};
bool g_pc_page = false;
std::atomic<bool> g_recomp_settings_open{false};
std::atomic<int> g_native_settings_page{0};
std::atomic<bool> g_remap_open{false};
std::atomic<int> g_remap_page{0};
std::atomic<int> g_remap_selected_row{0};
std::atomic<int> g_remap_capture_binding{-1};
std::atomic<WPARAM> g_remap_capture_activation_key{0};
std::atomic<int> g_requested_settings_page{0};
std::atomic<uint32_t> g_remap_reset_generation{0};
std::array<uint32_t, 5> g_original_setting_values{};
std::array<uint32_t, 5> g_original_setting_max{};
std::array<int, 5> g_pc_setting_values{{1, 1, 1, 1, 1}};
bool g_pc_setting_values_initialized = false;
std::atomic<int> g_pc_selected_row{0};
std::mutex g_pc_setting_values_mutex;
bool g_original_settings_snapshot_valid = false;
std::atomic<uint32_t> g_help_options_object{0};
std::atomic<uint32_t> g_help_options_selection{0xFFFFFFFFu};
std::atomic<bool> g_help_options_active{false};
std::atomic<ULONGLONG> g_help_options_draw_tick{0};
std::atomic<bool> g_reset_dialog_open{false};
std::atomic<uint8_t*> g_guest_base{nullptr};
uint32_t g_active_settings_object = 0;
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
    rex::cvar::SetFlagByName("vsync", g_pc_vsync.load() ? "true" : "false");
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
        else if (key == "fps") g_pc_fps = value;
        else if (key == "aa") g_pc_aa = value == 3 ? 1 : std::clamp(value, 0, 2);
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
         << "aa=" << g_pc_aa.load() << '\n';
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

uint32_t AllocateGameBuffer(PPCContext& ctx, uint8_t* base, uint32_t size)
{
    ctx.r3.u64 = size;
    ctx.r4.u64 = 0;
    sub_82150B60(ctx, base);
    return ctx.r3.u32;
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

uint32_t GetRecompStringPointer(uint32_t id, PPCContext& ctx, uint8_t* base)
{
    static constexpr std::array<std::string_view, 45> strings{{
        "Recompilation Settings", "RECOMPILATION SETTINGS",
        "Resolution", "HD", "FHD", "2K", "4K",
        "Screen Type", "Windowed", "Fullscreen",
        "Frame Rate", "30 FPS", "60 FPS", "120 FPS", "144 FPS",
        "VSync", "Off", "On",
        "Anti-Aliasing", "Off", "FXAA", "FXAA Extreme", "",
        "Choose the game resolution.",
        "Choose windowed or fullscreen display.",
        "Choose the frame-rate limit.",
        "Enable or disable vertical synchronization.",
        "Choose an anti-aliasing mode.",
        "Resolution: HD >", "Resolution: < FHD >", "Resolution: < 2K >", "Resolution: < 4K",
        "Screen Type: Windowed >", "Screen Type: < Fullscreen",
        "Frame Rate: 30 FPS >", "Frame Rate: < 60 FPS >", "Frame Rate: < 120 FPS >", "Frame Rate: < 144 FPS",
        "VSync: Off >", "VSync: < On",
        "Anti-Aliasing: Off >", "Anti-Aliasing: < FXAA >",
        "Anti-Aliasing: < FXAA Extreme", "",
        ""
    }};
    if (id < 5274 || id > 5318) return 0;
    const size_t index = id - 5274;
    uint32_t& buffer = g_recomp_string_buffers[index];
    if (!buffer) {
        ctx.r3.u64 = 128;
        ctx.r4.u64 = 0;
        sub_82150B60(ctx, base);
        buffer = ctx.r3.u32;
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

uint32_t GetAchievementStringPointer(uint32_t id, PPCContext& ctx, uint8_t* base)
{
    if (id < 5319 || id > 5326) return 0;
    const size_t index = id - 5319;
    uint32_t& buffer = g_achievement_row_buffers[index];
    if (!buffer) buffer = AllocateGameBuffer(ctx, base, 128);
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

uint32_t GetRemapStringPointer(uint32_t id, PPCContext& ctx, uint8_t* base)
{
    if (id < 5327 || id > 5334) return 0;
    const size_t index = id - 5327;
    uint32_t& buffer = g_remap_string_buffers[index];
    if (!buffer) buffer = AllocateGameBuffer(ctx, base, 128);
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

constexpr std::array<int, 5> kPcMenuMaxima{{3, 1, 3, 1, 2}};

void InitializePcMenuValues()
{
    std::lock_guard lock(g_pc_setting_values_mutex);
    if (g_pc_setting_values_initialized) return;
    g_pc_setting_values = {{
        g_pc_width.load() >= 3840 ? 3 : g_pc_width.load() >= 2560 ? 2 :
            g_pc_width.load() >= 1920 ? 1 : 0,
        g_pc_fullscreen.load() ? 1 : 0,
        g_pc_fps.load() >= 144 ? 3 : g_pc_fps.load() >= 120 ? 2 :
            g_pc_fps.load() >= 60 ? 1 : 0,
        g_pc_vsync.load() ? 1 : 0,
        std::clamp(g_pc_aa.load(), 0, 2),
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
        static constexpr int limits[4] = {30, 60, 120, 144};
        g_pc_width = widths[g_pc_setting_values[0]];
        g_pc_height = heights[g_pc_setting_values[0]];
        g_pc_fullscreen = g_pc_setting_values[1] != 0;
        g_pc_fps = limits[g_pc_setting_values[2]];
        g_pc_vsync = g_pc_setting_values[3] != 0;
        g_pc_aa = g_pc_setting_values[4];
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
    g_pc_page = false;
    g_native_settings_page = 0;
    g_requested_settings_page = 0;
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
			case VK_DOWN: case 'S': g_pc_selected_row = std::min(4, row + 1); break;
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
			g_native_settings_page = 0;
			g_requested_settings_page = 0;
			g_pc_page = false;
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

    if (config.gpu_plugin.empty()) {
        config.gpu_plugin = "xenos";
    }

    ApplyPcSettings();
}

void StfXblaApp::OnPostSetup()
{
    g_achievement_kernel = runtime()->kernel_state();
    g_graphics_system = runtime()->graphics_system();
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
}

void StfXblaApp::OnPreLaunchModule()
{
}

void StfrApplyPcFrameLimit()
{
    static auto next_frame = std::chrono::steady_clock::now();
    const int limit = g_pc_fps.load();
    if (limit <= 0) { next_frame = std::chrono::steady_clock::now(); return; }
    const auto interval = std::chrono::nanoseconds(1'000'000'000LL / limit);
    next_frame += interval;
    const auto now = std::chrono::steady_clock::now();
    if (next_frame > now) std::this_thread::sleep_until(next_frame);
    else if (now - next_frame > interval * 2) next_frame = now;
}

void StfrPrepareNativePcSettings(uint32_t object, uint32_t, uint8_t* base)
{
    // Watch the Screen Size setting and update the display right away
    if (object && base) {
        const uint32_t size = REX_LOAD_U32(object + 460);
        if (size <= 2) SetOriginalScreenSize(static_cast<int>(size), true);
    }
    g_guest_base = base;
    g_help_options_active = false;
    g_help_options_draw_tick = 0;
    g_recomp_settings_open = false;
    g_remap_open = false;
    g_remap_capture_binding = -1;
    g_native_settings_page = 0;
    g_requested_settings_page = 0;
    g_pc_page = false;
    g_active_settings_object = 0;
}

extern "C" bool StfrAdjustPcSettingBridge(uint32_t, int)
{
    return false;
}

extern "C" bool StfrIsPcSettingsPageBridge()
{
    return false;
}

extern "C" void StfrCapturePcSettingWidgetBridge(uint32_t)
{
}

void StfrPrepareHelpOptions(uint32_t object, uint32_t labels, uint8_t* base)
{
    g_guest_base = base;
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
        std::array<int, 5> values;
        {
            std::lock_guard lock(g_pc_setting_values_mutex);
            values = g_pc_setting_values;
        }
        constexpr uint32_t first_ids[5] = {5302, 5306, 5308, 5312, 5314};
        for (int i = 0; i < 5; ++i) {
            REX_STORE_U32(labels + i * 4, first_ids[i] + static_cast<uint32_t>(values[i]));
            REX_STORE_U8(object + 565 + i, 0);
        }
        REX_STORE_U8(object + 570, 1);
        REX_STORE_U32(object + 436, 4);
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
    if (g_original_settings_snapshot_valid && g_active_settings_object) {
        for (int i = 0; i < 5; ++i) {
            REX_STORE_U32(g_active_settings_object + 452 + i * 4, g_original_setting_values[i]);
            REX_STORE_U32(g_active_settings_object + 472 + i * 4, g_original_setting_max[i]);
        }
        g_original_settings_snapshot_valid = false;
    }
    g_active_settings_object = 0;
    g_native_settings_page = 0;
    g_pc_page = false;

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

void StfrRestoreOriginalSettingsBeforeClose(uint32_t object, uint8_t* base)
{
    if (!g_original_settings_snapshot_valid || g_native_settings_page.load() == 0) return;
    for (int i = 0; i < 5; ++i) {
        REX_STORE_U32(object + 452 + i * 4, g_original_setting_values[i]);
        REX_STORE_U32(object + 472 + i * 4, g_original_setting_max[i]);
    }
    g_original_settings_snapshot_valid = false;
    g_recomp_settings_open = false;
    g_remap_open = false;
    g_native_settings_page = 0;
    g_pc_page = false;
}

uint32_t StfrMapHelpOptionsSelection(uint32_t selection)
{
    // Keep the original menu
    uint32_t mapped = selection;
    switch (selection) {
    case 2: 
        g_recomp_settings_open = false;
        g_remap_open = false;
        g_requested_settings_page = 0;
        g_pc_page = false;
        g_native_settings_page = 0;
        mapped = 2;
        break;
    case 3: case 4: 
        mapped = selection;
        break;
    case 5:
        g_requested_settings_page = 0;
        mapped = 3;
        break;
    case 6:
        g_requested_settings_page = 0;
        mapped = 4;
        break;
    default:
        break;
    }
    return mapped;
}

void StfrPatchSettingsDescriptions(uint32_t descriptions, uint8_t* base)
{
    if (g_native_settings_page.load() == 2) {
        const uint32_t ids[5] = {411, 412, 413, 414, 415};
        for (int i = 0; i < 5; ++i) REX_STORE_U32(descriptions + i * 4, ids[i]);
        return;
    }
    if (g_native_settings_page.load() != 1) return;
    const uint32_t ids[5] = {5297, 5298, 5299, 5300, 5301};
    for (int i = 0; i < 5; ++i) REX_STORE_U32(descriptions + i * 4, ids[i]);
}

uint32_t StfrGetSettingsMax(uint32_t row, uint32_t original)
{
    if (g_native_settings_page.load() == 2) return original;
    if (g_native_settings_page.load() != 1 || row >= 5) return original;
    constexpr uint32_t pc_max[5] = {3, 1, 3, 1, 3};
    return pc_max[row];
}

bool StfrIsNativePcSettingsActive()
{
    return g_native_settings_page.load() != 0;
}

uint32_t StfrGetSettingsTitle(uint32_t original)
{
    if (g_native_settings_page.load() == 2) return 425;
    return g_native_settings_page.load() == 1 ? 5275 : original;
}

uint32_t StfrGetSettingsValueString(uint32_t label, uint32_t value, uint32_t original)
{
    if (g_native_settings_page.load() == 1) {
        if (label == 5276) return 5277 + static_cast<uint32_t>(std::clamp(g_pc_setting_values[0], 0, 3));
        if (label == 5281) return 5282 + static_cast<uint32_t>(std::clamp(g_pc_setting_values[1], 0, 1));
        if (label == 5284) return 5285 + static_cast<uint32_t>(std::clamp(g_pc_setting_values[2], 0, 3));
        if (label == 5289) return 5290 + static_cast<uint32_t>(std::clamp(g_pc_setting_values[3], 0, 1));
        if (label == 5292) return 5293 + static_cast<uint32_t>(std::clamp(g_pc_setting_values[4], 0, 3));
    }
    return original;
}

extern "C" void StfrPrepareNativePcSettingsBridge(uint32_t object, uint32_t labels, uint8_t* base)
{
    StfrPrepareNativePcSettings(object, labels, base);
}

extern "C" void StfrPrepareHelpOptionsBridge(uint32_t object, uint32_t labels, uint8_t* base)
{
    StfrPrepareHelpOptions(object, labels, base);
}

extern "C" uint32_t StfrGetSettingsTitleBridge(uint32_t original)
{
    return StfrGetSettingsTitle(original);
}

extern "C" uint32_t StfrGetHelpOptionsTitleBridge()
{
    if (g_achievements_open.load()) return 5319;
    if (g_remap_open.load()) return 5328;
    return g_recomp_settings_open.load() ? 5275 : 418;
}

extern "C" uint32_t StfrGetSettingsValueStringBridge(uint32_t label, uint32_t value, uint32_t original)
{
    return StfrGetSettingsValueString(label, value, original);
}

extern "C" uint32_t StfrResolveRecompStringBridge(uint32_t id, PPCContext& ctx, uint8_t* base)
{
    if (id >= 5327 && id <= 5334) return GetRemapStringPointer(id, ctx, base);
    if (id >= 5319 && id <= 5326) return GetAchievementStringPointer(id, ctx, base);
    return GetRecompStringPointer(id, ctx, base);
}

extern "C" uint32_t StfrResolvePauseAchievementStringBridge(uint32_t id,
    PPCContext& ctx, uint8_t* base)
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
    return GetAchievementStringPointer(replacement, ctx, base);
}

extern "C" void StfrOpenPauseAchievementsBridge()
{
    g_achievement_page = 0;
    g_achievement_selected_row = 4;
    RefreshAchievementPage();
    g_pause_achievements_open = true;
    REXLOG_INFO("Opened achievements inside the game's pause menu");
}

extern "C" uint32_t StfrResolveAchievementDescriptionBridge(PPCContext& ctx, uint8_t* base)
{
    if (g_remap_open.load()) return GetRemapStringPointer(5334, ctx, base);
    if (g_achievements_open.load()) return GetAchievementStringPointer(5325, ctx, base);
    std::string description;
    if (g_recomp_settings_open.load()) {
        constexpr std::string_view descriptions[5] = {
            "Choose the game resolution.",
            "Choose windowed or fullscreen display.",
            "Choose the frame-rate limit.",
            "Enable or disable vertical synchronization.",
            "Choose an anti-aliasing mode."
        };
        description = descriptions[static_cast<size_t>(std::clamp(g_pc_selected_row.load(), 0, 4))];
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
    return GetRemapStringPointer(5334, ctx, base);
}

extern "C" void StfrCaptureMainMenuBridge(uint32_t object, uint8_t* base)
{
    g_guest_base = base;
    g_main_menu_object = object;
    g_main_menu_draw_tick = GetTickCount64();
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
