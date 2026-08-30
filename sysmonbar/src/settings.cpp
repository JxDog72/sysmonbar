#include "settings.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

void AppSettings::resetDefaults() {
    *this = AppSettings{};
}

std::string AppSettings::settingsPath() {
    wchar_t* path = nullptr;
    std::string result = "sysmonbar_settings.ini";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
        fs::path dir = fs::path(path) / "sysmonbar";
        fs::create_directories(dir);
        result = (dir / "settings.ini").string();
        CoTaskMemFree(path);
    }
    return result;
}

static void writeColor(std::ostream& out, const char* key, const ImVec4& c) {
    out << key << '=' << c.x << ',' << c.y << ',' << c.z << ',' << c.w << '\n';
}

static bool readColor(const std::string& val, ImVec4& c) {
    return sscanf(val.c_str(), "%f,%f,%f,%f", &c.x, &c.y, &c.z, &c.w) == 4;
}

bool AppSettings::save() const {
    const std::string path = settingsPath();
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;

    out << "poll_ms=" << poll_ms << '\n';
    out << "ui_fps=" << ui_fps << '\n';
    out << "vsync=" << (vsync ? 1 : 0) << '\n';
    out << "animations=" << (animations ? 1 : 0) << '\n';
    out << "sparklines=" << (sparklines ? 1 : 0) << '\n';
    out << "spark_use_metric=" << (spark_use_metric ? 1 : 0) << '\n';
    out << "spark_thickness=" << spark_thickness << '\n';
    out << "spark_alpha=" << spark_alpha << '\n';
    out << "snap_top=" << (snap_top ? 1 : 0) << '\n';
    out << "bar_height=" << bar_height << '\n';
    out << "bar_width=" << bar_width << '\n';
    out << "opacity=" << opacity << '\n';
    out << "smooth=" << smooth << '\n';
    out << "always_on_top=" << (always_on_top ? 1 : 0) << '\n';
    out << "reassert_topmost=" << (reassert_topmost ? 1 : 0) << '\n';
    out << "click_through=" << (click_through ? 1 : 0) << '\n';
    out << "lock_size=" << (lock_size ? 1 : 0) << '\n';
    out << "lock_position=" << (lock_position ? 1 : 0) << '\n';
    out << "remember_position=" << (remember_position ? 1 : 0) << '\n';
    out << "start_with_windows=" << (start_with_windows ? 1 : 0) << '\n';
    out << "pos_x=" << pos_x << '\n';
    out << "pos_y=" << pos_y << '\n';
    out << "show_cpu=" << (show_cpu ? 1 : 0) << '\n';
    out << "show_ram=" << (show_ram ? 1 : 0) << '\n';
    out << "show_gpu=" << (show_gpu ? 1 : 0) << '\n';
    out << "show_cpu_pwr=" << (show_cpu_pwr ? 1 : 0) << '\n';
    out << "show_gpu_pwr=" << (show_gpu_pwr ? 1 : 0) << '\n';

    writeColor(out, "color_cpu", colors.cpu);
    writeColor(out, "color_ram", colors.ram);
    writeColor(out, "color_gpu", colors.gpu);
    writeColor(out, "color_power", colors.power);
    writeColor(out, "color_warn", colors.warn);
    writeColor(out, "color_bg", colors.bg);
    writeColor(out, "color_text", colors.text);
    writeColor(out, "color_spark", colors.spark);
    return true;
}

bool AppSettings::load() {
    const std::string path = settingsPath();
    std::ifstream in(path);
    if (!in) return false;

    AppSettings defaults;
    *this = defaults;

    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);

        if (key == "poll_ms") poll_ms = std::stoi(val);
        else if (key == "ui_fps") ui_fps = std::stoi(val);
        else if (key == "vsync") vsync = val == "1";
        else if (key == "animations") animations = val == "1";
        else if (key == "sparklines") sparklines = val == "1";
        else if (key == "spark_use_metric") spark_use_metric = val == "1";
        else if (key == "spark_thickness") spark_thickness = std::stof(val);
        else if (key == "spark_alpha") spark_alpha = std::stof(val);
        else if (key == "snap_top") snap_top = val == "1";
        else if (key == "bar_height") bar_height = std::stoi(val);
        else if (key == "bar_width") bar_width = std::stoi(val);
        else if (key == "opacity") opacity = std::stof(val);
        else if (key == "smooth") smooth = std::stof(val);
        else if (key == "always_on_top") always_on_top = val == "1";
        else if (key == "reassert_topmost") reassert_topmost = val == "1";
        else if (key == "click_through") click_through = val == "1";
        else if (key == "lock_size") lock_size = val == "1";
        else if (key == "lock_position") lock_position = val == "1";
        else if (key == "remember_position") remember_position = val == "1";
        else if (key == "start_with_windows") start_with_windows = val == "1";
        else if (key == "pos_x") pos_x = std::stoi(val);
        else if (key == "pos_y") pos_y = std::stoi(val);
        else if (key == "show_cpu") show_cpu = val == "1";
        else if (key == "show_ram") show_ram = val == "1";
        else if (key == "show_gpu") show_gpu = val == "1";
        else if (key == "show_cpu_pwr") show_cpu_pwr = val == "1";
        else if (key == "show_gpu_pwr") show_gpu_pwr = val == "1";
        else if (key == "color_cpu") readColor(val, colors.cpu);
        else if (key == "color_ram") readColor(val, colors.ram);
        else if (key == "color_gpu") readColor(val, colors.gpu);
        else if (key == "color_power") readColor(val, colors.power);
        else if (key == "color_warn") readColor(val, colors.warn);
        else if (key == "color_bg") readColor(val, colors.bg);
        else if (key == "color_text") readColor(val, colors.text);
        else if (key == "color_spark") readColor(val, colors.spark);
    }

    poll_ms = std::clamp(poll_ms, 250, 10000);
    ui_fps = std::clamp(ui_fps, 4, 60);
    bar_height = std::clamp(bar_height, 28, 96);
    bar_width = std::clamp(bar_width, 280, 2560);
    opacity = std::clamp(opacity, 0.20f, 1.f);
    smooth = std::clamp(smooth, 0.05f, 0.5f);
    spark_thickness = std::clamp(spark_thickness, 0.5f, 6.f);
    spark_alpha = std::clamp(spark_alpha, 0.08f, 1.f);
    return true;
}