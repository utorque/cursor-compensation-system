#include "compensation.h"
#include "app.h"

#include <cmath>
#include <random>
#include <chrono>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace std::chrono;

CompensationEngine& CompensationEngine::get() {
    static CompensationEngine instance;
    return instance;
}

void CompensationEngine::sendMouseMove(int dx, int dy) {
    if (dx == 0 && dy == 0) return;
    INPUT in{};
    in.type    = INPUT_MOUSE;
    in.mi.dx   = dx;
    in.mi.dy   = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(INPUT));
}

void CompensationEngine::start(const std::string& macroUUID) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_runs.find(macroUUID);
    if (it != m_runs.end() && it->second->running.load()) return;  // already running

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
        if (!m) { running->store(false); return; }

        Profile* p = App::get().findProfile(m->profile_uuid);
        if (!p || p->bullets == 0) { running->store(false); return; }

        profile      = *p;
        macroSettings = m->settings;
        global        = App::get().settings();
    }

    profile.updateBullets();
    int bullets = profile.bullets;
    if (bullets == 0) { running->store(false); return; }

    // ── Pre-compute parameters ────────────────────────────────────────────────
    float sm          = global.screenMultiplier();     // e.g. ≈ −0.028
    float mag         = macroSettings.magnification;
    float rng_range   = global.randomness_percent / 100.0f;  // e.g. 0.05

    // ms between each offset step  (60 000 / RPM)
    float stepMs      = 60000.0f / profile.rpm;

    std::mt19937                          rngEng(std::random_device{}());
    std::uniform_real_distribution<float> dist(-rng_range, rng_range);

    int smoothSteps = macroSettings.use_smoothing
                    ? std::max(1, macroSettings.smoothing_steps)
                    : 1;

    // ── Wait 5 ms (mirrors the original Lua script's initial delay) ──────────
    std::this_thread::sleep_for(milliseconds(5));

    float lastDx_f = 0.0f, lastDy_f = 0.0f;

    auto applyStep = [&](float ox, float oy) {
        // randomness
        float rx = 1.0f + dist(rngEng);
        float ry = 1.0f + dist(rngEng);
        ox *= rx;
        oy *= ry;

        float dx_f = ox * mag * sm;
        float dy_f = oy * mag * sm;
        lastDx_f = dx_f;
        lastDy_f = dy_f;

        if (smoothSteps <= 1) {
            sendMouseMove((int)std::round(dx_f), (int)std::round(dy_f));
            std::this_thread::sleep_for(duration<float, std::milli>(stepMs));
        } else {
            // Linear interpolation across sub-steps (Smoothing function from README)
            float prevX = 0.0f, prevY = 0.0f;
            float subMs = stepMs / (float)smoothSteps;
            for (int s = 1; s <= smoothSteps && running->load(); ++s) {
                float xi = (float)s * dx_f / (float)smoothSteps;
                float yi = (float)s * dy_f / (float)smoothSteps;
                int sdx = (int)std::round(xi - prevX);
                int sdy = (int)std::round(yi - prevY);
                sendMouseMove(sdx, sdy);
                prevX = xi; prevY = yi;
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
