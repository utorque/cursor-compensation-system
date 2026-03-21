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
#include <timeapi.h>   // timeBeginPeriod / timeEndPeriod

using namespace std::chrono;

CompensationEngine& CompensationEngine::get() {
    static CompensationEngine instance;
    return instance;
}

// ── Low-level input helpers ───────────────────────────────────────────────────

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

void CompensationEngine::sendLMB(bool down) {
    INPUT in{};
    in.type       = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(INPUT));
    DLOG("ACTION", down ? "LMB down" : "LMB up");
}

// ── start ─────────────────────────────────────────────────────────────────────

void CompensationEngine::start(const std::string& macroUUID) {
    // Capture profile data outside the CE lock to avoid lock-order issues
    // with App's own mutex.
    Profile       profile;
    MacroSettings macroSettings;
    GlobalSettings global;
    bool valid = false;
    {
        Macro* m = App::get().findMacro(macroUUID);
        if (m) {
            Profile* p = App::get().findProfile(m->profile_uuid);
            if (p && p->bullets > 0) {
                profile      = *p;
                macroSettings = m->settings;
                global        = App::get().settings();
                profile.updateBullets();
                valid = profile.bullets > 0;
            }
        }
    }
    if (!valid) return;

    std::lock_guard<std::mutex> lk(m_mutex);

    // ── Semi-auto mode ────────────────────────────────────────────────────────
    if (profile.semi) {
        auto& ss    = m_semiStates[macroUUID];
        auto  now   = steady_clock::now();
        float dt    = 60000.0f / profile.rpm;   // ms per step

        bool first   = (ss.lastClick.time_since_epoch().count() == 0);
        float elapsed = first ? (4.0f * dt + 1.0f)
                               : duration<float, std::milli>(now - ss.lastClick).count();

        if (elapsed > 4.0f * dt) {
            ss.stepIdx = 0;
            ss.accX = ss.accY = 0.0f;
            DLOG("INFO", "Semi: reset to step 0 (timeout)");
        }

        int curIdx = std::min(ss.stepIdx, profile.bullets - 1);
        ss.stepIdx++;
        ss.lastClick = now;

        // Compute the step value now (under lock) so the accumulator is
        // updated atomically and the thread only needs to do I/O.
        float ox = profile.offset_x[curIdx];
        float oy = profile.offset_y[curIdx];

        float rng_range = global.randomness_percent / 100.0f;
        if (rng_range > 0.0f) {
            std::mt19937 rng(std::random_device{}());
            std::uniform_real_distribution<float> dist(-rng_range, rng_range);
            ox *= (1.0f + dist(rng));
            oy *= (1.0f + dist(rng));
        }

        float sm  = global.screenMultiplier();
        float mag = macroSettings.magnification;
        ss.accX += ox * mag * sm;
        ss.accY += oy * mag * sm;
        int idx = (int)std::round(ss.accX);
        int idy = (int)std::round(ss.accY);
        ss.accX -= (float)idx;
        ss.accY -= (float)idy;

        {
            char buf[96];
            snprintf(buf, sizeof(buf),
                "Semi step[%d] send=(%d,%d)  rem=(%.4f,%.4f)",
                curIdx, idx, idy, ss.accX, ss.accY);
            DLOG("ACTION", buf);
        }

        auto state = std::make_unique<RunState>();
        state->running->store(true);
        auto runPtr = state->running;
        state->thread = std::thread([this, idx, idy, runPtr]() {
            sendMouseMove(idx, idy);
            sendLMB(true);
            std::this_thread::sleep_for(milliseconds(30));
            sendLMB(false);
            runPtr->store(false);
        });
        m_runs[macroUUID] = std::move(state);
        return;
    }

    // ── Auto mode ─────────────────────────────────────────────────────────────
    auto it = m_runs.find(macroUUID);
    if (it != m_runs.end() && it->second->running->load()) {
        DLOG("INFO", "CompensationEngine::start: already running: " + macroUUID);
        return;
    }

    DLOG("ACTION", "CompensationEngine: thread starting for macro " + macroUUID);
    auto state = std::make_unique<RunState>();
    state->running->store(true);
    state->autoLMB = true;
    auto runPtr = state->running;
    state->thread = std::thread([this, macroUUID, runPtr]() {
        runLoop(macroUUID, runPtr);
    });
    m_runs[macroUUID] = std::move(state);
}

// ── stop / stopAll ────────────────────────────────────────────────────────────

void CompensationEngine::stop(const std::string& macroUUID) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_runs.find(macroUUID);
    if (it == m_runs.end()) return;
    DLOG("ACTION", "CompensationEngine: stopping thread for macro " + macroUUID);
    it->second->running->store(false);
    if (it->second->autoLMB) sendLMB(false);
    if (it->second->thread.joinable()) it->second->thread.detach();
    m_runs.erase(it);
}

void CompensationEngine::stopAll() {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& [uuid, state] : m_runs) {
        state->running->store(false);
        if (state->autoLMB) sendLMB(false);
        if (state->thread.joinable()) state->thread.detach();
    }
    m_runs.clear();
}

bool CompensationEngine::isRunning(const std::string& macroUUID) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_runs.find(macroUUID);
    return it != m_runs.end() && it->second->running->load();
}

// ── runLoop (auto mode only) ──────────────────────────────────────────────────

void CompensationEngine::runLoop(const std::string& macroUUID,
                                  std::shared_ptr<std::atomic<bool>> running) {
    Profile        profile;
    MacroSettings  macroSettings;
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

    float sm        = global.screenMultiplier();
    float mag       = macroSettings.magnification;
    float rng_range = global.randomness_percent / 100.0f;
    float stepMs    = 60000.0f / profile.rpm;   // dt = 60 s / RPM

    {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "params: screenMul=%.6f  mag=%.2f  rng=%.1f%%  stepMs=%.1f  smooth=%s(%d)",
            sm, mag, global.randomness_percent, stepMs,
            macroSettings.use_smoothing ? "on" : "off",
            macroSettings.smoothing_steps);
        DLOG("INFO", buf);
    }

    // Sanity-check: warn if the total pixel displacement is below 0.5px.
    {
        float maxOY = 0.0f;
        for (int i = 0; i < bullets; ++i)
            maxOY = std::max(maxOY, std::abs(profile.offset_y[i]));
        float totalMaxPx = maxOY * std::abs(mag * sm) * (float)bullets;
        if (totalMaxPx < 0.5f) {
            char warn[256];
            float needed = (maxOY > 0.0f)
                ? 0.5f / (maxOY * std::abs(mag) * (float)bullets) : 0.0f;
            snprintf(warn, sizeof(warn),
                "WARNING: max total Y displacement = %.4fpx across %d bullets — "
                "will never send a move. Increase sensitivity/FOV so |screenMul| >= %.4f",
                totalMaxPx, bullets, needed);
            DLOG("ERROR", warn);
        }
    }

    std::mt19937                          rngEng(std::random_device{}());
    std::uniform_real_distribution<float> dist(-rng_range, rng_range);

    int smoothSteps = macroSettings.use_smoothing
                    ? std::max(1, macroSettings.smoothing_steps)
                    : 1;

    // Request 1 ms OS timer resolution for accurate sleep_until on Windows.
    timeBeginPeriod(1);

    // Initial 5 ms delay, then hold LMB for the duration of this auto run.
    auto targetTime = steady_clock::now() + milliseconds(5);
    std::this_thread::sleep_until(targetTime);
    sendLMB(true);

    float lastDx_f = 0.0f, lastDy_f = 0.0f;
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
            targetTime += duration_cast<nanoseconds>(duration<double, std::milli>(stepMs));
            std::this_thread::sleep_until(targetTime);
        } else {
            float  subDx = dx_f / (float)smoothSteps;
            float  subDy = dy_f / (float)smoothSteps;
            double subMs = (double)stepMs / smoothSteps;
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
                targetTime += duration_cast<nanoseconds>(duration<double, std::milli>(subMs));
                std::this_thread::sleep_until(targetTime);
            }
        }
    };

    for (int i = 0; i < bullets && running->load(); ++i)
        applyStep(profile.offset_x[i], profile.offset_y[i]);

    float lastOX = profile.offset_x[bullets - 1];
    float lastOY = profile.offset_y[bullets - 1];
    while (running->load())
        applyStep(lastOX, lastOY);

    // LMB_UP is sent by stop() via the autoLMB flag — do not double-send here.
    timeEndPeriod(1);
    running->store(false);
}
