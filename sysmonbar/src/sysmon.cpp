#include "sysmon.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Pdh.h>
#include <PdhMsg.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "pdh.lib")

namespace {

uint64_t fileTimeToUint64(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

std::string wideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::string readCpuName() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            0, KEY_READ, &key) != ERROR_SUCCESS) {
        return "CPU";
    }

    wchar_t buf[256] = {};
    DWORD size = sizeof(buf);
    DWORD type = REG_SZ;
    std::string name = "CPU";
    if (RegQueryValueExW(key, L"ProcessorNameString", nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS) {
        name = wideToUtf8(buf);
    }
    RegCloseKey(key);

    // Trim repeated spaces from registry string.
    std::string compact;
    compact.reserve(name.size());
    bool space = false;
    for (char c : name) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!space) compact.push_back(' ');
            space = true;
        } else {
            compact.push_back(c);
            space = false;
        }
    }
    return compact.empty() ? "CPU" : compact;
}

float guessTdpFromName(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower.find("ryzen 9") != std::string::npos) return 170.f;
    if (lower.find("ryzen 7") != std::string::npos) return 105.f;
    if (lower.find("ryzen 5") != std::string::npos) return 65.f;
    if (lower.find("ryzen 3") != std::string::npos) return 45.f;
    if (lower.find("i9") != std::string::npos) return 125.f;
    if (lower.find("i7") != std::string::npos) return 95.f;
    if (lower.find("i5") != std::string::npos) return 65.f;
    if (lower.find("i3") != std::string::npos) return 45.f;
    if (lower.find("threadripper") != std::string::npos) return 280.f;
    if (lower.find("xeon") != std::string::npos) return 150.f;
    return 65.f;
}

// NVML types — loaded dynamically from nvml.dll shipped with NVIDIA drivers.
using nvmlReturn_t = int;
using nvmlDevice_t = void*;
struct nvmlUtilization_t { unsigned int gpu, memory; };

struct NvmlApi {
    HMODULE dll = nullptr;
    nvmlReturn_t (WINAPI *Init)() = nullptr;
    nvmlReturn_t (WINAPI *Shutdown)() = nullptr;
    nvmlReturn_t (WINAPI *GetCount)(unsigned int*) = nullptr;
    nvmlReturn_t (WINAPI *GetHandleByIndex)(unsigned int, nvmlDevice_t*) = nullptr;
    nvmlReturn_t (WINAPI *GetUtilizationRates)(nvmlDevice_t, nvmlUtilization_t*) = nullptr;
    nvmlReturn_t (WINAPI *GetPowerUsage)(nvmlDevice_t, unsigned int*) = nullptr;
    nvmlReturn_t (WINAPI *GetTemperature)(nvmlDevice_t, int, unsigned int*) = nullptr;
    nvmlReturn_t (WINAPI *GetName)(nvmlDevice_t, char*, unsigned int) = nullptr;

    bool load() {
        dll = LoadLibraryW(L"nvml.dll");
        if (!dll) return false;

        #define LOAD(fn, name) fn = reinterpret_cast<decltype(fn)>(GetProcAddress(dll, name))
        LOAD(Init, "nvmlInit_v2");
        if (!Init) LOAD(Init, "nvmlInit");
        LOAD(Shutdown, "nvmlShutdown");
        LOAD(GetCount, "nvmlDeviceGetCount_v2");
        if (!GetCount) LOAD(GetCount, "nvmlDeviceGetCount");
        LOAD(GetHandleByIndex, "nvmlDeviceGetHandleByIndex_v2");
        if (!GetHandleByIndex) LOAD(GetHandleByIndex, "nvmlDeviceGetHandleByIndex");
        LOAD(GetUtilizationRates, "nvmlDeviceGetUtilizationRates");
        LOAD(GetPowerUsage, "nvmlDeviceGetPowerUsage");
        LOAD(GetTemperature, "nvmlDeviceGetTemperature");
        LOAD(GetName, "nvmlDeviceGetName");
        #undef LOAD

        if (!Init || !Shutdown || !GetCount || !GetHandleByIndex ||
            !GetUtilizationRates || !GetPowerUsage || !GetTemperature || !GetName) {
            unload();
            return false;
        }
        return Init() == 0;
    }

    void unload() {
        if (dll) {
            if (Shutdown) Shutdown();
            FreeLibrary(dll);
        }
        dll = nullptr;
        Init = nullptr;
        Shutdown = nullptr;
        GetCount = nullptr;
        GetHandleByIndex = nullptr;
        GetUtilizationRates = nullptr;
        GetPowerUsage = nullptr;
        GetTemperature = nullptr;
        GetName = nullptr;
    }
};

} // namespace

struct SystemMonitor::NvmlState {
    ::NvmlApi api;
};

SystemMonitor::SystemMonitor() = default;

SystemMonitor::~SystemMonitor() {
    stop();

    if (pdh_query_) {
        PdhCloseQuery(reinterpret_cast<PDH_HQUERY>(pdh_query_));
        pdh_query_ = nullptr;
    }

    if (nvml_) {
        nvml_->api.unload();
        delete nvml_;
        nvml_ = nullptr;
    }
}

void SystemMonitor::start(int interval_ms) {
    interval_ms_ = std::clamp(interval_ms, 250, 10000);
    initCPU();
    initGPU();
    last_collect_ = std::chrono::steady_clock::now();
    running_ = true;
    thread_ = std::thread(&SystemMonitor::worker, this);
}

void SystemMonitor::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void SystemMonitor::setInterval(int interval_ms) {
    interval_ms_ = std::clamp(interval_ms, 250, 10000);
}

void SystemMonitor::setCollectGpu(bool enabled) {
    collect_gpu_ = enabled;
}

AllStats SystemMonitor::snapshot() {
    std::lock_guard<std::mutex> lk(mtx_);
    return stats_;
}

void SystemMonitor::worker() {
    while (running_) {
        collect();
        std::this_thread::sleep_for(
            std::chrono::milliseconds(interval_ms_.load()));
    }
}

void SystemMonitor::collect() {
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - last_collect_).count();
    last_collect_ = now;

    collectCPU(dt);
    collectMem();
    if (collect_gpu_.load()) {
        collectGPU();
    }
}

void SystemMonitor::initCPU() {
    const std::string name = readCpuName();
    cpu_tdp_w_ = guessTdpFromName(name);

    std::lock_guard<std::mutex> lk(mtx_);
    stats_.cpu.model_name = name;
}

void SystemMonitor::initGPU() {
    // Try NVML first (real GPU util + power on NVIDIA).
    nvml_ = new NvmlState();
    if (!nvml_->api.load()) {
        delete nvml_;
        nvml_ = nullptr;
    }

    // PDH fallback for GPU utilization on any vendor.
    PDH_HQUERY query = nullptr;
    if (PdhOpenQuery(nullptr, 0, &query) != ERROR_SUCCESS) return;

    DWORD pathSize = 0;
    PdhExpandCounterPathW(L"\\GPU Engine(*engtype_3D)\\Utilization Percentage",
                          nullptr, &pathSize);

    if (pathSize > 0) {
        std::vector<wchar_t> paths(pathSize);
        if (PdhExpandCounterPathW(L"\\GPU Engine(*engtype_3D)\\Utilization Percentage",
                                 paths.data(), &pathSize) == ERROR_SUCCESS) {
            const wchar_t* cursor = paths.data();
            while (*cursor) {
                PDH_HCOUNTER counter = nullptr;
                if (PdhAddCounterW(query, cursor, 0, &counter) == ERROR_SUCCESS) {
                    pdh_gpu_counters_.push_back(counter);
                }
                cursor += wcslen(cursor) + 1;
            }
        }
    }

    if (!pdh_gpu_counters_.empty()) {
        PdhCollectQueryData(query);
        pdh_query_ = query;
        pdh_ready_ = false; // need one more sample before reading
    } else {
        PdhCloseQuery(query);
    }
}

void SystemMonitor::collectCPU(double dt_sec) {
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return;

    const uint64_t idle_t = fileTimeToUint64(idle);
    const uint64_t kernel_t = fileTimeToUint64(kernel);
    const uint64_t user_t = fileTimeToUint64(user);

    if (first_cpu_sample_) {
        prev_idle_ = idle_t;
        prev_kernel_ = kernel_t;
        prev_user_ = user_t;
        first_cpu_sample_ = false;
        return;
    }

    const uint64_t idle_delta = idle_t - prev_idle_;
    const uint64_t kernel_delta = kernel_t - prev_kernel_;
    const uint64_t user_delta = user_t - prev_user_;

    const uint64_t total = kernel_delta + user_delta;
    const uint64_t busy = total > idle_delta ? total - idle_delta : 0;
    const float pct = total ? 100.f * static_cast<float>(busy) / static_cast<float>(total) : 0.f;

    prev_idle_ = idle_t;
    prev_kernel_ = kernel_t;
    prev_user_ = user_t;

    // ~2 second exponential moving average regardless of poll interval.
    const float dt = static_cast<float>(std::max(dt_sec, 0.001));
    const float alpha = 1.f - std::exp(-dt / 2.f);
    cpu_avg_pct_ = alpha * pct + (1.f - alpha) * cpu_avg_pct_;

    std::lock_guard<std::mutex> lk(mtx_);
    stats_.cpu.total_pct = pct;
    stats_.cpu.avg_pct = cpu_avg_pct_;
    stats_.cpu.history.push(cpu_avg_pct_);
    stats_.cpu.power_w = estimateCPUPowerW(cpu_avg_pct_);
    stats_.cpu.power_estimated = true;
}

float SystemMonitor::estimateCPUPowerW(float usage_pct) const {
    // Package power estimate: idle draw + scaled TDP based on rolling CPU load.
    constexpr float kIdleW = 12.f;
    const float load = std::clamp(usage_pct / 100.f, 0.f, 1.f);
    return kIdleW + (cpu_tdp_w_ - kIdleW) * load * 0.82f;
}

void SystemMonitor::collectMem() {
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (!GlobalMemoryStatusEx(&mem)) return;

    std::lock_guard<std::mutex> lk(mtx_);
    stats_.mem.total_bytes = mem.ullTotalPhys;
    stats_.mem.used_bytes = mem.ullTotalPhys - mem.ullAvailPhys;
    stats_.mem.pct = mem.dwMemoryLoad;
}

void SystemMonitor::collectGPU() {
    float util = 0.f;
    float power_mw = 0.f;
    float temp = 0.f;
    std::string name = "GPU";
    bool have_util = false;
    bool have_power = false;

    if (nvml_) {
        unsigned int count = 0;
        if (nvml_->api.GetCount(&count) == 0 && count > 0) {
            nvmlDevice_t device = nullptr;
            if (nvml_->api.GetHandleByIndex(0, &device) == 0) {
                nvmlUtilization_t rates{};
                if (nvml_->api.GetUtilizationRates(device, &rates) == 0) {
                    util = static_cast<float>(rates.gpu);
                    have_util = true;
                }

                unsigned int mw = 0;
                if (nvml_->api.GetPowerUsage(device, &mw) == 0) {
                    power_mw = static_cast<float>(mw);
                    have_power = true;
                }

                unsigned int temp_c = 0;
                if (nvml_->api.GetTemperature(device, 0, &temp_c) == 0) {
                    temp = static_cast<float>(temp_c);
                }

                char gpu_name[96] = {};
                if (nvml_->api.GetName(device, gpu_name, sizeof(gpu_name)) == 0) {
                    name = gpu_name;
                }
            }
        }
    }

    // PDH fallback / supplement for non-NVML GPUs.
    if (!have_util && pdh_query_ && !pdh_gpu_counters_.empty()) {
        PdhCollectQueryData(reinterpret_cast<PDH_HQUERY>(pdh_query_));

        if (pdh_ready_) {
            float peak = 0.f;
            for (void* raw : pdh_gpu_counters_) {
                PDH_FMT_COUNTERVALUE val{};
                if (PdhGetFormattedCounterValue(reinterpret_cast<PDH_HCOUNTER>(raw),
                        PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS) {
                    peak = std::max(peak, static_cast<float>(val.doubleValue));
                }
            }
            util = peak;
            have_util = true;
        } else {
            pdh_ready_ = true;
        }
    }

    std::lock_guard<std::mutex> lk(mtx_);
    auto& g = stats_.gpu;
    g.available = have_util || have_power;
    g.usage_pct = util;
    g.power_w = power_mw / 1000.f;
    g.power_available = have_power;
    g.temperature = temp;
    g.name = name;
}