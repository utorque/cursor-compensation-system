#include "compensation.h"
#include "app.h"
#include "debug.h"

// Only logs when Advanced Debug is enabled
#define ADLOG(cat, msg) \
    do { if (DebugLog::get().advancedEnabled.load(std::memory_order_relaxed)) \
             DebugLog::get().log(cat, msg); } while(0)

#include <cmath>
#include <random>
#include <chrono>
#include <string>
#include <sstream>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace std::chrono;

CompensationEngine& CompensationEngine::get() {
    static CompensationEngine instance;
    return instance;
}

void CompensationEngine::sendMouseMove(int dx, int dy) {
    if (dx == 0 && dy == 0) {
        ADLOG("MOVE", "skipped (0,0)");
        return;
    }
    INPUT in{};
    in.type       = INPUT_MOUSE;
    in.mi.dx      = dx;
    in.mi.dy      = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    UINT sent = SendInput(1, &in, sizeof(INPUT));
    if (sent == 0) {
        DLOG("ERROR", "SendInput failed (UIPI block or hook issue)");
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "SendInput dx=%d dy=%d", dx, dy);
        DLOG("MOVE", buf);
    }
}

void CompensationEngine::start(const std::string& macroUUID) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_runs.find(macroUUID);
    if (it != m_runs.end() && it->second->running.load()) {
        DLOG("INFO", "CompensationEngine::start called but already running: " + macroUUID);
        return;  // already running
    }

    DLOG("ACTION", "CompensationEngine: thread starting for macro " + macroUUID);
    auto state = std::make_unique<RunState>();
    state->running.store(true);
    std::atomic<bool>* ptr = &state->running;
    state->thread = std::thread([this, macroUUID, ptr]() {
        runLoop(macroUUID, ptr);
    });
    m_runs[macroUUID] = std::move(state);
}

void CompensationEngine::stop(const std::string& macroUUID) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_runs.find(macroUUID);
    if (it == m_runs.end()) return;
    DLOG("ACTION", "CompensationEngine: stopping thread for macro " + macroUUID);
    it->second->running.store(false);
    if (it->second->thread.joinable()) it->second->thread.detach();
    m_runs.erase(it);
}

void CompensationEngine::stopAll() {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& [uuid, state] : m_runs) {
        state->running.store(false);
        if (state->thread.joinable()) state->thread.detach();
    }
    m_runs.clear();
}

bool CompensationEngine::isRunning(const std::string& macroUUID) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_runs.find(macroUUID);
    return it != m_runs.end() && it->second->running.load();
}

void CompensationEngine::runLoop(const std::string& macroUUID, std::atomic<bool>* running) {
    // ── Capture all needed data before entering the hot loop ─────────────────
    Profile       profile;
    MacroSettings macroSettings;
    GlobalSettings global;
    {
        Macro* m = App::get().findMacro(macroUUID);
        if (!m) {
            DLOG("ERROR", "runLoop: macro not found: " + macroUUID);
            running->store(false); return;
        }

        Profile* p = App::get().findProfile(m->profile_uuid);
        if (!p || p->bullets == 0) {
            DLOG("ERROR", "runLoop: profile missing or zero bullets for macro: " + m->name);
            running->store(false); return;
        }

        DLOG("ACTION", "runLoop: starting for macro '" + m->name +
             "' profile '" + p->name + "' bullets=" + std::to_string(p->bullets));

        profile      = *p;
        macroSettings = m->settings;
        global        = App::get().settings();
    }

    profile.updateBullets();
    int bullets = profile.bullets;
    if (bullets == 0) {
        DLOG("ERROR", "runLoop: bullets=0 after update, aborting");
        running->store(false); return;
    }

    // ── Pre-compute parameters ────────────────────────────────────────────────
    float sm          = global.screenMultiplier();     // e.g. ≈ −0.028
    float mag         = macroSettings.magnification;
    float rng_range   = global.randomness_percent / 100.0f;  // e.g. 0.05

    // ms between each offset step  (60 000 / RPM)
    float stepMs      = 60000.0f / profile.rpm;

    {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "params: screenMul=%.6f  mag=%.2f  rng=%.1f%%  stepMs=%.1f  smooth=%s(%d)",
            sm, mag, global.randomness_percent, stepMs,
            macroSettings.use_smoothing ? "on" : "off",
            macroSettings.smoothing_steps);
        DLOG("INFO", buf);
    }

    std::mt19937                          rngEng(std::random_device{}());
    std::uniform_real_distribution<float> dist(-rng_range, rng_range);

    int smoothSteps = macroSettings.use_smoothing
                    ? std::max(1, macroSettings.smoothing_steps)
                    : 1;

    // ── Wait 5 ms (mirrors the original Lua script's initial delay) ──────────
    std::this_thread::sleep_for(milliseconds(5));

    float lastDx_f = 0.0f, lastDy_f = 0.0f;

    // Persistent sub-pixel accumulator — carries fractional remainders across
    // ALL bullets and sub-steps so small per-step values build up over time.
    float accX = 0.0f, accY = 0.0f;
    int   stepIdx = 0;

    auto applyStep = [&](float ox, float oy) {
        int curStep = stepIdx++;

        float rx = 1.0f + dist(rngEng);
        float ry = 1.0f + dist(rngEng);
        ox *= rx;
        oy *= ry;

        float dx_f = ox * mag * sm;
        float dy_f = oy * mag * sm;
        lastDx_f = dx_f;
        lastDy_f = dy_f;

        if (smoothSteps <= 1) {
            accX += dx_f;
            accY += dy_f;
            int idx = (int)std::round(accX);
            int idy = (int)std::round(accY);
            accX -= (float)idx;
            accY -= (float)idy;
            {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "step[%d] ox=%.4f oy=%.4f  dx=%.4f dy=%.4f  send=(%d,%d)  rem=(%.4f,%.4f)",
                    curStep, ox, oy, dx_f, dy_f, idx, idy, accX, accY);
                ADLOG("STEP", buf);
            }
            sendMouseMove(idx, idy);
            std::this_thread::sleep_for(duration<float, std::milli>(stepMs));
        } else {
            float subDx = dx_f / (float)smoothSteps;
            float subDy = dy_f / (float)smoothSteps;
            float subMs = stepMs / (float)smoothSteps;
            for (int s = 0; s < smoothSteps && running->load(); ++s) {
                accX += subDx;
                accY += subDy;
                int idx = (int)std::round(accX);
                int idy = (int)std::round(accY);
                accX -= (float)idx;
                accY -= (float)idy;
                {
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                        "step[%d.%d] subDx=%.4f subDy=%.4f  send=(%d,%d)  rem=(%.4f,%.4f)",
                        curStep, s, subDx, subDy, idx, idy, accX, accY);
                    ADLOG("STEP", buf);
                }
                sendMouseMove(idx, idy);
                std::this_thread::sleep_for(duration<float, std::milli>(subMs));
            }
        }
    };

    // ── Main pass — iterate through the offset table ──────────────────────────
    for (int i = 0; i < bullets && running->load(); ++i)
        applyStep(profile.offset_x[i], profile.offset_y[i]);

    // ── Hold at last step while trigger is still held ─────────────────────────
    float lastOX = profile.offset_x[bullets - 1];
    float lastOY = profile.offset_y[bullets - 1];
    while (running->load())
        applyStep(lastOX, lastOY);

    running->store(false);
}
