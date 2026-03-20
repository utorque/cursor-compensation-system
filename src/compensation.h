#pragma once
#include <string>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>

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
        std::thread          thread;
        std::atomic<bool>    running{false};
    };

    mutable std::mutex                            m_mutex;
    std::map<std::string, std::unique_ptr<RunState>> m_runs;

    void runLoop(const std::string& macroUUID, std::atomic<bool>* running);
    static void sendMouseMove(int dx, int dy);
};
