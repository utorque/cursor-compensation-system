#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

struct DebugEntry {
    std::string timestamp;  // HH:MM:SS.mmm
    std::string category;   // ACTION / EVENT / ERROR / INFO
    std::string message;
};

class DebugLog {
public:
    static DebugLog& get();

    std::atomic<bool> enabled{ false };
    std::atomic<bool> advancedEnabled{ false };  // logs per-step thread internals

    void   log(const char* category, const std::string& msg);
    void   clear();
    bool   exportToFile(const std::string& path) const;

    // Thread-safe snapshot for UI rendering
    std::vector<DebugEntry> snapshot() const;
    size_t count() const;

private:
    DebugLog() = default;
    mutable std::mutex      m_mutex;
    std::vector<DebugEntry> m_entries;

    static constexpr size_t kMaxEntries = 10000;
};

// Zero-overhead macro — compiles to nothing when disabled at runtime
#define DLOG(cat, msg) \
    do { if (DebugLog::get().enabled.load(std::memory_order_relaxed)) \
             DebugLog::get().log(cat, msg); } while(0)
