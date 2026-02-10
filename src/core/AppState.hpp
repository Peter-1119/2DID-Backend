#pragma once
#include <atomic>
struct AppState {
    // MES 連線狀態 (由 MonitorLoop 更新)
    static inline std::atomic<bool> isMesOnline{true};
};