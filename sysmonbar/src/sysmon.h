#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Fixed-size circular buffer for rolling averages and sparklines.
template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(int cap = 60) : data_(cap, T{}), cap_(cap) {}

    void push(T val) {
        data_[head_] = val;
        head_ = (head_ + 1) % cap_;
        if (filled_ < cap_) ++filled_;
    }

    T average() const {
        if (filled_ == 0) return T{};
        T sum = T{};
        for (int i = 0; i < filled_; ++i) sum += data_[i];
        return sum / static_cast<T>(filled_);
    }

    T latest() const {
        if (filled_ == 0) return T{};
        return data_[(head_ - 1 + cap_) % cap_];
    }

    int capacity() const { return cap_; }
    int filled() const { return filled_; }

private:
    std::vector<T> data_;
    int cap_, head_ = 0, filled_ = 0;
};

struct CPUStats {
    float total_pct = 0.f;       // latest sample
    float avg_pct = 0.f;         // ~2 second rolling average
    float power_w = 0.f;
    bool  power_estimated = true;
    std::string model_name;
    RingBuffer<float> history{24};
};

struct MemStats {
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    float    pct = 0.f;
};

struct GPUStats {
    std::string name;
    float usage_pct = 0.f;
    float power_w = 0.f;
    float temperature = 0.f;
    bool  available = false;
    bool  power_available = false;
};

struct AllStats {
    CPUStats cpu;
    MemStats mem;
    GPUStats gpu;
};

// Background collector — snapshots are thread-safe copies for the UI.
class SystemMonitor {
public:
    SystemMonitor();
    ~SystemMonitor();

    void start(int interval_ms = 1000);
    void stop();
    void setInterval(int interval_ms);
    void setCollectGpu(bool enabled);
    AllStats snapshot();

private:
    void worker();
    void collect();

    void initCPU();
    void initGPU();
    void collectCPU(double dt_sec);
    void collectMem();
    void collectGPU();

    float estimateCPUPowerW(float usage_pct) const;

    AllStats stats_;
    std::mutex mtx_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int>  interval_ms_{1000};
    std::atomic<bool> collect_gpu_{true};

    float cpu_avg_pct_ = 0.f; // exponential moving average (~2s time constant)
    std::chrono::steady_clock::time_point last_collect_{};

    // CPU timing baseline (GetSystemTimes).
    uint64_t prev_idle_ = 0;
    uint64_t prev_kernel_ = 0;
    uint64_t prev_user_ = 0;
    bool first_cpu_sample_ = true;

    float cpu_tdp_w_ = 65.f;

    // PDH query for GPU utilization fallback.
    void* pdh_query_ = nullptr;          // PDH_HQUERY
    std::vector<void*> pdh_gpu_counters_; // PDH_HCOUNTER
    bool pdh_ready_ = false;

    // NVML state (loaded from nvml.dll when NVIDIA drivers are present).
    struct NvmlState;
    NvmlState* nvml_ = nullptr;
};