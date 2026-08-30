// sysmonbar — lightweight Windows telemetry strip

#include "sysmon.h"
#include "settings.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace {
constexpr int kSettingsW = 480;
constexpr int kSettingsH = 560;
} // namespace

struct DisplayState {
    float cpu = 0.f, ram = 0.f, gpu = 0.f;
    float cpu_pwr = 0.f, gpu_pwr = 0.f;
    std::array<float, 32> cpu_hist{};
    std::array<float, 32> ram_hist{};
    std::array<float, 32> gpu_hist{};
    int hist_idx = 0;
    double anim_t = 0.0;
    double last_hist_push = 0.0;
};

struct DragState { bool active = false; double sx = 0, sy = 0; int wx = 0, wy = 0; };

static float lerp(float a, float b, float t) { return a + (b - a) * t; }

static ImU32 withAlpha(const ImVec4& c, float alpha_mul = 1.f) {
    ImVec4 v = c;
    v.w *= alpha_mul;
    return toU32(v);
}

static ImU32 loadColor(float pct, const ImVec4& normal, const ImVec4& warn) {
    if (pct >= 90.f) return toU32(warn);
    if (pct >= 75.f) return IM_COL32(255, 200, 60, 255);
    return toU32(normal);
}

static void applyImGuiTheme(const AppSettings& s) {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowPadding = {10, 8};
    st.FramePadding = {6, 4};
    st.ItemSpacing = {8, 6};
    st.WindowRounding = 8.f;
    st.FrameRounding = 5.f;
    st.WindowBorderSize = 1.f;

    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = s.colors.bg;
    c[ImGuiCol_Text] = ImVec4(0.85f, 0.92f, 0.95f, 1.f);
    c[ImGuiCol_PopupBg] = ImVec4(0.05f, 0.08f, 0.10f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.20f, 0.38f, 0.48f, 0.55f);
    c[ImGuiCol_FrameBg] = ImVec4(0.08f, 0.12f, 0.16f, 0.9f);
    c[ImGuiCol_Button] = ImVec4(0.10f, 0.18f, 0.22f, 0.95f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.14f, 0.28f, 0.34f, 1.f);
    c[ImGuiCol_Header] = ImVec4(0.12f, 0.22f, 0.28f, 0.9f);
    c[ImGuiCol_SliderGrab] = s.colors.cpu;
}

static void enableWin32Transparency(GLFWwindow* win) {
    HWND hwnd = glfwGetWin32Window(win);
    if (!hwnd) return;
    const HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    using Fn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (fn) {
        const int backdrop = 3;
        fn(hwnd, 38, &backdrop, sizeof(backdrop));
    }
    FreeLibrary(dwm);
}

static HWND nativeHwnd(GLFWwindow* win) {
    return win ? glfwGetWin32Window(win) : nullptr;
}

static void applyTopmost(GLFWwindow* win, bool on) {
    glfwSetWindowAttrib(win, GLFW_FLOATING, on ? GLFW_TRUE : GLFW_FALSE);
    HWND hwnd = nativeHwnd(win);
    if (!hwnd) return;
    SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void applyClickThrough(GLFWwindow* win, bool on) {
    HWND hwnd = nativeHwnd(win);
    if (!hwnd) return;
    LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    ex |= WS_EX_LAYERED;
    if (on)
        ex |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    else
        ex &= ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);
}

static void applyOpacity(GLFWwindow* win, float opacity) {
    opacity = std::clamp(opacity, 0.20f, 1.f);
    glfwSetWindowOpacity(win, opacity);
}

static void applyResizable(GLFWwindow* win, bool resizable) {
    glfwSetWindowAttrib(win, GLFW_RESIZABLE, resizable ? GLFW_TRUE : GLFW_FALSE);
}

static void applyAutostart(bool on) {
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (on) {
        const DWORD bytes = static_cast<DWORD>((wcslen(exe) + 1) * sizeof(wchar_t));
        RegSetValueExW(key, L"sysmonbar", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(exe), bytes);
    } else {
        RegDeleteValueW(key, L"sysmonbar");
    }
    RegCloseKey(key);
}

static void snapToEdge(GLFWwindow* win, const AppSettings& cfg, int screen_w, int screen_h) {
    int cw = 0, ch = 0;
    glfwGetWindowSize(win, &cw, &ch);
    const int ypos = cfg.snap_top ? 8 : (std::max)(8, screen_h - ch - 8);
    glfwSetWindowPos(win, (screen_w - cw) / 2, ypos);
}

// Grow/shrink the native window: bar only, or bar + settings panel stacked below.
static void syncNativeWindowSize(GLFWwindow* win, const AppSettings& cfg, bool show_settings) {
    const int target_w = show_settings ? (std::max)(cfg.bar_width, kSettingsW) : cfg.bar_width;
    const int target_h = cfg.bar_height + (show_settings ? kSettingsH : 0);

    int cw = 0, ch = 0;
    glfwGetWindowSize(win, &cw, &ch);
    if (cw != target_w || ch != target_h)
        glfwSetWindowSize(win, target_w, target_h);
}

// When settings are closed, user edge-resize updates stored bar dimensions.
static void syncBarSizeFromWindow(GLFWwindow* win, AppSettings& cfg, bool show_settings) {
    if (show_settings || cfg.lock_size) return;

    int cw = 0, ch = 0;
    glfwGetWindowSize(win, &cw, &ch);
    cw = (std::clamp)(cw, 280, 2560);
    ch = (std::clamp)(ch, 28, 96);
    cfg.bar_width = cw;
    cfg.bar_height = ch;
}

static void drawMetricBlock(ImDrawList* dl, ImVec2 pos, float width, float height,
                            const char* label, float value_pct, float display_pct,
                            const AppSettings& cfg, const ImVec4& accent,
                            const std::array<float, 32>& hist, int hist_idx,
                            double anim_t) {
    const ImU32 panel = withAlpha(cfg.colors.bg, 0.75f);
    const ImU32 border = withAlpha(cfg.colors.text, 0.55f);
    const ImU32 accent_u32 = toU32(accent);

    const ImVec2 p0 = pos;
    const ImVec2 p1 = {pos.x + width, pos.y + height};
    dl->AddRectFilled(p0, p1, panel, 5.f);
    dl->AddRect(p0, p1, border, 5.f);

    if (cfg.sparklines) {
        const float spark_h = height * 0.5f;
        const float spark_y = p1.y - spark_h - 3.f;
        const float step = (width - 10.f) / static_cast<float>(hist.size() - 1);
        for (size_t i = 1; i < hist.size(); ++i) {
            const int a = (static_cast<int>(i) + hist_idx) % static_cast<int>(hist.size());
            const int b = (static_cast<int>(i - 1) + hist_idx) % static_cast<int>(hist.size());
            const float x1 = p0.x + 5.f + (i - 1) * step;
            const float x2 = p0.x + 5.f + i * step;
            const float y1 = spark_y + spark_h * (1.f - std::clamp(hist[b] / 100.f, 0.f, 1.f));
            const float y2 = spark_y + spark_h * (1.f - std::clamp(hist[a] / 100.f, 0.f, 1.f));
            ImU32 spark_col;
            if (cfg.spark_use_metric) {
                const int a8 = static_cast<int>(std::clamp(cfg.spark_alpha, 0.08f, 1.f) * 255.f);
                spark_col = (accent_u32 & 0x00FFFFFF) | (a8 << 24);
            } else {
                spark_col = toU32(cfg.colors.spark);
            }
            dl->AddLine({x1, y1}, {x2, y2}, spark_col, cfg.spark_thickness);
        }
    }

    dl->AddText({p0.x + 8.f, p0.y + 4.f}, toU32(cfg.colors.text), label);

    char val[24];
    snprintf(val, sizeof(val), "%.1f%%", display_pct);
    const ImVec2 val_sz = ImGui::CalcTextSize(val);
    dl->AddText({p1.x - val_sz.x - 8.f, p0.y + 4.f},
                loadColor(value_pct, accent, cfg.colors.warn), val);

    constexpr int segments = 14;
    const float bar_y = p0.y + height - 11.f;
    const float seg_gap = 2.f;
    const float seg_w = (width - 16.f - seg_gap * (segments - 1)) / segments;
    const int lit = static_cast<int>(std::ceil(std::clamp(display_pct / 100.f, 0.f, 1.f) * segments));

    for (int i = 0; i < segments; ++i) {
        const float sx = p0.x + 8.f + i * (seg_w + seg_gap);
        const ImU32 col = (i < lit)
            ? loadColor(value_pct, accent, cfg.colors.warn)
            : IM_COL32(25, 35, 45, 180);
        dl->AddRectFilled({sx, bar_y}, {sx + seg_w, bar_y + 4.f}, col, 1.f);
    }

    if (cfg.animations && value_pct >= 85.f) {
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(anim_t * 5.0));
        dl->AddRect(p0, p1, IM_COL32(255, 80, 50, static_cast<int>(35 + 45 * pulse)), 5.f, 0, 1.5f);
    }
}

static void drawPowerChip(ImDrawList* dl, ImVec2 pos, const char* label,
                          float watts, bool estimated, const AppSettings& cfg,
                          const ImVec4& accent, double anim_t) {
    char val[24];
    snprintf(val, sizeof(val), estimated ? "~%.0fW" : "%.0fW", watts);

    const ImVec2 txt = ImGui::CalcTextSize(val);
    const float w = txt.x + 24.f;
    const float h = 28.f;

    dl->AddRectFilled(pos, {pos.x + w, pos.y + h}, withAlpha(cfg.colors.bg, 0.75f), 5.f);
    dl->AddRect(pos, {pos.x + w, pos.y + h}, withAlpha(cfg.colors.text, 0.5f), 5.f);
    dl->AddText({pos.x + 8.f, pos.y + 3.f}, toU32(cfg.colors.text), label);
    dl->AddText({pos.x + 8.f, pos.y + 14.f}, toU32(accent), val);

    if (cfg.animations) {
        const float shimmer = 0.5f + 0.5f * std::sin(static_cast<float>(anim_t * 2.5f));
        const float load_frac = watts / 250.f < 1.f ? watts / 250.f : 1.f;
        const ImVec2 shimmer_p0 = {pos.x + 4.f, pos.y + h - 2.f};
        const ImVec2 shimmer_p1 = {pos.x + 4.f + (w - 8.f) * shimmer * load_frac, pos.y + h};
        dl->AddRectFilled(shimmer_p0, shimmer_p1,
                          (toU32(accent) & 0x00FFFFFF) | 0x22000000, 1.f);
    }
}

static void renderSettings(AppSettings& cfg, SystemMonitor& mon, GLFWwindow* win,
                           bool* open, float bar_height) {
    if (!*open) return;

    // Draw inside the expanded native window, directly below the bar strip.
    ImGui::SetNextWindowPos({0.f, bar_height}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({static_cast<float>(kSettingsW), static_cast<float>(kSettingsH)},
                             ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.f);
    if (ImGui::Begin("sysmonbar settings", open,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
        if (ImGui::BeginTabBar("settings_tabs")) {
            if (ImGui::BeginTabItem("Performance")) {
                ImGui::TextDisabled("Lower values = less CPU/GPU use");
                ImGui::SliderInt("Poll interval (ms)", &cfg.poll_ms, 250, 5000);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    mon.setInterval(cfg.poll_ms);

                ImGui::SliderInt("UI refresh (FPS)", &cfg.ui_fps, 4, 60);
                ImGui::Checkbox("VSync", &cfg.vsync);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    glfwSwapInterval(cfg.vsync ? 1 : 0);

                ImGui::Checkbox("Animations", &cfg.animations);
                ImGui::SliderFloat("Smoothing", &cfg.smooth, 0.05f, 0.45f);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Graph")) {
                ImGui::Checkbox("Show sparkline graph", &cfg.sparklines);
                ImGui::BeginDisabled(!cfg.sparklines);
                ImGui::SliderFloat("Line thickness", &cfg.spark_thickness, 0.5f, 5.f, "%.1f px");
                ImGui::SliderFloat("Line opacity", &cfg.spark_alpha, 0.10f, 1.f, "%.2f");
                ImGui::Checkbox("Line color matches CPU / RAM / GPU", &cfg.spark_use_metric);
                ImGui::BeginDisabled(cfg.spark_use_metric);
                ImGui::ColorEdit4("Custom line color", &cfg.colors.spark.x);
                ImGui::EndDisabled();
                ImGui::EndDisabled();
                ImGui::TextDisabled("History graph drawn behind each usage panel.");
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Appearance")) {
                if (ImGui::SliderInt("Bar height", &cfg.bar_height, 28, 96))
                    syncNativeWindowSize(win, cfg, true);
                if (ImGui::SliderInt("Bar width", &cfg.bar_width, 280, 1920))
                    syncNativeWindowSize(win, cfg, true);
                if (ImGui::SliderFloat("Opacity", &cfg.opacity, 0.20f, 1.f))
                    applyOpacity(win, cfg.opacity);
                ImGui::TextDisabled("Width and height apply live. Drag an edge when settings are closed.");
                ImGui::Separator();
                ImGui::ColorEdit3("CPU", &cfg.colors.cpu.x);
                ImGui::ColorEdit3("RAM", &cfg.colors.ram.x);
                ImGui::ColorEdit3("GPU", &cfg.colors.gpu.x);
                ImGui::ColorEdit3("Power", &cfg.colors.power.x);
                ImGui::ColorEdit3("Warning", &cfg.colors.warn.x);
                ImGui::ColorEdit3("Background", &cfg.colors.bg.x);
                ImGui::ColorEdit3("Label text", &cfg.colors.text.x);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Window")) {
                if (ImGui::Checkbox("Always on top", &cfg.always_on_top))
                    applyTopmost(win, cfg.always_on_top);
                ImGui::Checkbox("Re-assert topmost (keeps overlays from covering the bar)", &cfg.reassert_topmost);
                if (ImGui::Checkbox("Click-through (Ctrl+Shift+T to toggle)", &cfg.click_through))
                    applyClickThrough(win, cfg.click_through);
                if (ImGui::Checkbox("Lock size", &cfg.lock_size))
                    applyResizable(win, !cfg.lock_size);
                ImGui::Checkbox("Lock position", &cfg.lock_position);
                ImGui::Checkbox("Remember position", &cfg.remember_position);
                if (ImGui::Checkbox("Start with Windows", &cfg.start_with_windows))
                    applyAutostart(cfg.start_with_windows);

                int snap = cfg.snap_top ? 1 : 0;
                if (ImGui::RadioButton("Snap top", &snap, 1)) {
                    cfg.snap_top = true;
                    GLFWmonitor* primary = glfwGetPrimaryMonitor();
                    const GLFWvidmode* mode = primary ? glfwGetVideoMode(primary) : nullptr;
                    if (mode) snapToEdge(win, cfg, mode->width, mode->height);
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Snap bottom", &snap, 0)) {
                    cfg.snap_top = false;
                    GLFWmonitor* primary = glfwGetPrimaryMonitor();
                    const GLFWvidmode* mode = primary ? glfwGetVideoMode(primary) : nullptr;
                    if (mode) snapToEdge(win, cfg, mode->width, mode->height);
                }
                ImGui::TextDisabled("Fullscreen exclusive apps can still cover any overlay.");
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Panels")) {
                ImGui::Checkbox("CPU", &cfg.show_cpu);
                ImGui::Checkbox("RAM", &cfg.show_ram);
                ImGui::Checkbox("GPU", &cfg.show_gpu);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    mon.setCollectGpu(cfg.show_gpu || cfg.show_gpu_pwr);
                ImGui::Checkbox("CPU power", &cfg.show_cpu_pwr);
                ImGui::Checkbox("GPU power", &cfg.show_gpu_pwr);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    mon.setCollectGpu(cfg.show_gpu || cfg.show_gpu_pwr);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Spacing();
        if (ImGui::Button("Save")) {
            cfg.save();
            applyImGuiTheme(cfg);
            applyOpacity(win, cfg.opacity);
            applyTopmost(win, cfg.always_on_top);
            applyClickThrough(win, cfg.click_through);
            applyResizable(win, !cfg.lock_size);
            applyAutostart(cfg.start_with_windows);
            syncNativeWindowSize(win, cfg, true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset defaults")) {
            cfg.resetDefaults();
            mon.setInterval(cfg.poll_ms);
            mon.setCollectGpu(true);
            applyImGuiTheme(cfg);
            applyOpacity(win, cfg.opacity);
            applyTopmost(win, cfg.always_on_top);
            applyClickThrough(win, cfg.click_through);
            applyResizable(win, !cfg.lock_size);
            applyAutostart(cfg.start_with_windows);
            syncNativeWindowSize(win, cfg, true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) *open = false;

        ImGui::TextDisabled("Settings: %s", AppSettings::settingsPath().c_str());
        ImGui::TextDisabled("Ctrl+Q to quit");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

static void renderBar(const AllStats& target, DisplayState& disp, AppSettings& cfg,
                      int win_w, float bar_h, bool* show_settings) {
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;

    ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({static_cast<float>(win_w), bar_h}, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.f, 3.f});
    ImGui::Begin("##bar", nullptr, flags);
    ImGui::PopStyleVar();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();

    dl->AddRectFilled(wp, {wp.x + ws.x, wp.y + ws.y}, toU32(cfg.colors.bg), 6.f);
    dl->AddRect(wp, {wp.x + ws.x, wp.y + ws.y}, withAlpha(cfg.colors.text, 0.45f), 6.f);

    if (cfg.animations) {
        const float scan = static_cast<float>(fmod(disp.anim_t * 25.0, ws.x));
        dl->AddLine({wp.x + scan, wp.y + 1.f}, {wp.x + scan + 24.f, wp.y + 1.f},
                    (toU32(cfg.colors.cpu) & 0x00FFFFFF) | 0x55000000, 1.f);
    }

    float x = wp.x + 14.f;
    const float y = wp.y + 3.f;
    const float block_h = ws.y - 6.f;
    int n_metrics = 0;
    if (cfg.show_cpu) n_metrics++;
    if (cfg.show_ram) n_metrics++;
    if (cfg.show_gpu && target.gpu.available) n_metrics++;
    float reserved = 14.f + 8.f + 38.f;
    if (cfg.show_cpu_pwr) reserved += 80.f;
    if (cfg.show_gpu_pwr && target.gpu.available) reserved += 80.f;
    float block_w = 138.f;
    if (n_metrics > 0) {
        const float remain = ws.x - reserved - 8.f * static_cast<float>(n_metrics);
        block_w = remain / static_cast<float>(n_metrics);
        block_w = std::clamp(block_w, 96.f, 280.f);
    }

    if (cfg.show_cpu) {
        drawMetricBlock(dl, {x, y}, block_w, block_h, "CPU",
                        target.cpu.avg_pct, disp.cpu, cfg, cfg.colors.cpu,
                        disp.cpu_hist, disp.hist_idx, disp.anim_t);
        x += block_w + 8.f;
    }
    if (cfg.show_ram) {
        drawMetricBlock(dl, {x, y}, block_w, block_h, "RAM",
                        target.mem.pct, disp.ram, cfg, cfg.colors.ram,
                        disp.ram_hist, disp.hist_idx, disp.anim_t);
        x += block_w + 8.f;
    }
    if (cfg.show_gpu && target.gpu.available) {
        drawMetricBlock(dl, {x, y}, block_w, block_h, "GPU",
                        target.gpu.usage_pct, disp.gpu, cfg, cfg.colors.gpu,
                        disp.gpu_hist, disp.hist_idx, disp.anim_t);
        x += block_w + 8.f;
    }
    if (cfg.show_cpu_pwr) {
        drawPowerChip(dl, {x, y + 2.f}, "CPU", disp.cpu_pwr,
                      target.cpu.power_estimated, cfg, cfg.colors.power, disp.anim_t);
        x += 72.f;
    }
    if (cfg.show_gpu_pwr && target.gpu.available) {
        drawPowerChip(dl, {x, y + 2.f}, "GPU", disp.gpu_pwr,
                      !target.gpu.power_available, cfg, cfg.colors.gpu, disp.anim_t);
        x += 72.f;
    }

    // cfg button — real ImGui hit target (not just draw-list hover)
    const float btn_w = 34.f;
    const float btn_h = ws.y - 8.f;
    ImGui::SetCursorScreenPos({wp.x + ws.x - btn_w - 4.f, wp.y + 4.f});
    if (ImGui::InvisibleButton("##cfg_btn", {btn_w, btn_h})) {
        *show_settings = !*show_settings;
    }
    const ImVec2 btn_p = ImGui::GetItemRectMin();
    const ImVec2 btn_p2 = ImGui::GetItemRectMax();
    const bool btn_hov = ImGui::IsItemHovered();
    dl->AddRectFilled(btn_p, btn_p2,
                      btn_hov ? withAlpha(cfg.colors.cpu, 0.35f) : withAlpha(cfg.colors.bg, 0.6f), 4.f);
    dl->AddRect(btn_p, btn_p2, withAlpha(cfg.colors.text, 0.5f), 4.f);
    dl->AddText({btn_p.x + 6.f, btn_p.y + btn_h * 0.5f - 7.f},
                toU32(cfg.colors.cpu), "cfg");

    // Resize hint (bottom-right) when settings panel is closed
    if (!*show_settings) {
        dl->AddTriangleFilled(
            {wp.x + ws.x - 4.f, wp.y + ws.y - 10.f},
            {wp.x + ws.x - 4.f, wp.y + ws.y - 4.f},
            {wp.x + ws.x - 10.f, wp.y + ws.y - 4.f},
            withAlpha(cfg.colors.text, 0.45f));
    }

    for (int i = 0; i < 3; ++i)
        dl->AddCircleFilled({wp.x + 6.f, wp.y + 10.f + i * 6.f}, 1.4f, withAlpha(cfg.colors.text, 0.7f));

    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup("ctx");
    if (ImGui::BeginPopup("ctx")) {
        if (ImGui::MenuItem("Settings")) *show_settings = true;
        if (ImGui::MenuItem("Always on top", nullptr, cfg.always_on_top)) {
            cfg.always_on_top = !cfg.always_on_top;
            applyTopmost(glfwGetCurrentContext(), cfg.always_on_top);
        }
        if (ImGui::MenuItem("Quit")) glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
        ImGui::EndPopup();
    }

    ImGui::End();
}

static void handleDrag(GLFWwindow* win, DragState& drag, bool show_settings, bool lock_position) {
    if (show_settings || lock_position) return;

    double cx, cy;
    glfwGetCursorPos(win, &cx, &cy);
    int wx, wy;
    glfwGetWindowPos(win, &wx, &wy);
    const double sx = wx + cx, sy = wy + cy;
    const bool lmb = glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const bool in_grip = cx <= 12.0;

    if (!drag.active && lmb && in_grip)
        drag = {true, sx, sy, wx, wy};
    if (drag.active && lmb) {
        glfwSetWindowPos(win, drag.wx + static_cast<int>(sx - drag.sx),
                              drag.wy + static_cast<int>(sy - drag.sy));
    }
    if (!lmb) drag.active = false;
}

int main() {
    if (!glfwInit()) return 1;

    AppSettings settings;
    settings.load();

    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    const int screen_w = mode->width;
    const int screen_h = mode->height;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);

    GLFWwindow* win = glfwCreateWindow(settings.bar_width, settings.bar_height,
                                       "sysmonbar", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }

    if (settings.remember_position && settings.pos_x >= 0 && settings.pos_y >= 0
        && settings.pos_x < screen_w && settings.pos_y < screen_h) {
        glfwSetWindowPos(win, settings.pos_x, settings.pos_y);
    } else {
        const int ypos = settings.snap_top ? 8 : screen_h - settings.bar_height - 8;
        glfwSetWindowPos(win, (screen_w - settings.bar_width) / 2, ypos);
    }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(settings.vsync ? 1 : 0);
    enableWin32Transparency(win);
    applyOpacity(win, settings.opacity);
    applyTopmost(win, settings.always_on_top);
    applyClickThrough(win, settings.click_through);
    applyResizable(win, !settings.lock_size);
    if (settings.start_with_windows)
        applyAutostart(true);
    if (HWND hwnd = nativeHwnd(win))
        RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_SHIFT, 'T');

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImFontConfig fc;
    fc.OversampleH = 1;
    fc.OversampleV = 1;
    const char* fonts[] = {
        "C:\\Windows\\Fonts\\CascadiaMono.ttf",
        "C:\\Windows\\Fonts\\consola.ttf",
        nullptr
    };
    bool font_ok = false;
    for (int i = 0; fonts[i]; ++i) {
        FILE* f = nullptr;
        if (fopen_s(&f, fonts[i], "rb") == 0 && f) {
            fclose(f);
            io.Fonts->AddFontFromFileTTF(fonts[i], 13.f, &fc);
            font_ok = true;
            break;
        }
    }
    if (!font_ok) io.Fonts->AddFontDefault(&fc);

    applyImGuiTheme(settings);
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    SystemMonitor mon;
    mon.start(settings.poll_ms);
    mon.setCollectGpu(settings.show_gpu || settings.show_gpu_pwr);

    DisplayState display;
    DragState drag;
    bool show_settings = false;
    bool prev_show_settings = false;
    AllStats stats;
    double last_frame = glfwGetTime();
    double last_topmost = 0.0;
    bool click_toggle_held = false;

    while (!glfwWindowShouldClose(win)) {
        if (show_settings != prev_show_settings) {
            syncNativeWindowSize(win, settings, show_settings);
            applyClickThrough(win, show_settings ? false : settings.click_through);
            prev_show_settings = show_settings;
        }

        syncBarSizeFromWindow(win, settings, show_settings);

        const double now = glfwGetTime();
        const double frame_budget = 1.0 / (std::max)(settings.ui_fps, 4);
        const double wait = frame_budget - (now - last_frame);

        if (wait > 0.001 && !show_settings)
            glfwWaitEventsTimeout(wait);
        else
            glfwPollEvents();

        const bool ctrl = glfwGetKey(win, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS
                       || glfwGetKey(win, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
        const bool shift = glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS
                        || glfwGetKey(win, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        if (ctrl && glfwGetKey(win, GLFW_KEY_Q) == GLFW_PRESS)
            glfwSetWindowShouldClose(win, GLFW_TRUE);
        bool hotkey_hit = false;
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, WM_HOTKEY, WM_HOTKEY, PM_REMOVE)) {
            if (msg.wParam == 1) hotkey_hit = true;
        }
        const bool click_combo = ctrl && shift && glfwGetKey(win, GLFW_KEY_T) == GLFW_PRESS;
        if ((hotkey_hit || (click_combo && !click_toggle_held))) {
            settings.click_through = !settings.click_through;
            applyClickThrough(win, show_settings ? false : settings.click_through);
        }
        click_toggle_held = click_combo;

        const double now_top = glfwGetTime();
        if (settings.always_on_top && settings.reassert_topmost
            && (now_top - last_topmost > 1.5)) {
            applyTopmost(win, true);
            last_topmost = now_top;
        }

        const double frame_now = glfwGetTime();
        if (!show_settings && (frame_now - last_frame) < frame_budget)
            continue;
        last_frame = frame_now;

        stats = mon.snapshot();
        display.anim_t = frame_now;

        display.cpu = lerp(display.cpu, stats.cpu.avg_pct, settings.smooth);
        display.ram = lerp(display.ram, stats.mem.pct, settings.smooth);
        display.gpu = lerp(display.gpu, stats.gpu.usage_pct, settings.smooth);
        display.cpu_pwr = lerp(display.cpu_pwr, stats.cpu.power_w, settings.smooth);
        display.gpu_pwr = lerp(display.gpu_pwr, stats.gpu.power_w, settings.smooth);

        if (frame_now - display.last_hist_push >= settings.poll_ms * 0.001 * 0.9) {
            display.cpu_hist[display.hist_idx] = stats.cpu.avg_pct;
            display.ram_hist[display.hist_idx] = stats.mem.pct;
            display.gpu_hist[display.hist_idx] = stats.gpu.usage_pct;
            display.hist_idx = (display.hist_idx + 1) % static_cast<int>(display.cpu_hist.size());
            display.last_hist_push = frame_now;
        }

        handleDrag(win, drag, show_settings, settings.lock_position);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int fb_w, fb_h;
        glfwGetFramebufferSize(win, &fb_w, &fb_h);
        const float bar_h = static_cast<float>(settings.bar_height);

        renderBar(stats, display, settings, fb_w, bar_h, &show_settings);
        renderSettings(settings, mon, win, &show_settings, bar_h);

        ImGui::Render();
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    if (HWND hwnd = nativeHwnd(win))
        UnregisterHotKey(hwnd, 1);
    if (settings.remember_position) {
        glfwGetWindowPos(win, &settings.pos_x, &settings.pos_y);
    }
    settings.save();
    mon.stop();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}