#pragma once

#include <imgui.h>
#include <string>

struct ColorSet {
    ImVec4 cpu   = {0.24f, 1.00f, 0.66f, 1.f};
    ImVec4 ram   = {0.79f, 0.63f, 1.00f, 1.f};
    ImVec4 gpu   = {0.36f, 0.78f, 1.00f, 1.f};
    ImVec4 power = {1.00f, 0.70f, 0.28f, 1.f};
    ImVec4 warn  = {1.00f, 0.30f, 0.23f, 1.f};
    ImVec4 bg    = {0.04f, 0.06f, 0.08f, 0.92f};
    ImVec4 text  = {0.47f, 0.57f, 0.65f, 0.86f};
    ImVec4 spark = {0.55f, 0.95f, 1.00f, 0.55f};
};

struct AppSettings {
    // Polling / rendering efficiency
    int  poll_ms      = 1000;  // sensor sample interval
    int  ui_fps       = 12;    // max UI redraw rate (lower = less GPU)
    bool vsync        = false;
    bool animations   = true;
    bool sparklines   = true;
    bool spark_use_metric = true;
    float spark_thickness = 1.4f;
    float spark_alpha     = 0.35f;

    // Layout
    bool snap_top     = true;
    int  bar_height   = 34;
    int  bar_width    = 720;
    float opacity     = 0.92f;
    float smooth      = 0.15f;

    // Window behavior
    bool always_on_top      = true;
    bool reassert_topmost   = true;
    bool click_through      = false;
    bool lock_size          = false;
    bool lock_position      = false;
    bool remember_position  = true;
    bool start_with_windows = false;
    int  pos_x              = -1;
    int  pos_y              = -1;

    // Visible panels
    bool show_cpu     = true;
    bool show_ram     = true;
    bool show_gpu     = true;
    bool show_cpu_pwr = true;
    bool show_gpu_pwr = true;

    ColorSet colors;

    void resetDefaults();
    bool load();
    bool save() const;
    static std::string settingsPath();
};

inline ImU32 toU32(const ImVec4& c) {
    return ImGui::ColorConvertFloat4ToU32(c);
}