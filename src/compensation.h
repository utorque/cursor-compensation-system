#pragma once
#include <string>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

// Runs the pre-computed cursor compensation loop in a background thread.
// One thread per macro UUID — start / stop driven by the mouse hook.
class CompensationEngine {
public:
    static CompensationEngine& get();

    void start(const std::string& macroUUID);
    void stop(const std::string& macroUUID);
    void stopAll();
    bool isRunning(const std::string& macroUUID) const;

private:
    CompensationEngine() = default;

    struct RunState {
        std::thread                           thread;
        std::shared_ptr<std::atomic<bool>>    running;
        bool                                  autoLMB = false; // if true, stop() releases LMB
        RunState() : running(std::make_shared<std::atomic<bool>>(false)) {}
    };

    // Persistent state for semi-auto macros — survives between individual clicks.
    struct SemiState {
        int                                      stepIdx  = 0;
        float                                    accX     = 0.0f;
        float                                    accY     = 0.0f;
        std::chrono::steady_clock::time_point    lastClick{};
    };

    mutable std::mutex                               m_mutex;
    std::map<std::string, std::unique_ptr<RunState>> m_runs;
    std::map<std::string, SemiState>                 m_semiStates;

    void runLoop(const std::string& macroUUID,
                 std::shared_ptr<std::atomic<bool>> running);

    static void sendMouseMove(int dx, int dy);
    static void sendLMB(bool down);
};
