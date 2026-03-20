#include "app.h"
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

App& App::get() {
    static App instance;
    return instance;
}

void App::init() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string exe(path);
    m_appDir      = exe.substr(0, exe.find_last_of("\\/"));
    m_profilesDir = m_appDir + "\\profiles";
    m_macrosDir   = m_appDir + "\\macros";

    ensureDir(m_profilesDir);
    ensureDir(m_macrosDir);

    loadSettings();
    loadProfiles();
    loadMacros();
}

void App::ensureDir(const std::string& path) {
    CreateDirectoryA(path.c_str(), nullptr);
}

// ── Settings ─────────────────────────────────────────────────────────────────

void App::loadSettings() {
    std::string p = m_appDir + "\\settings.json";
    std::ifstream f(p);
    if (!f.good()) return;
    try {
        nlohmann::json j;
        f >> j;
        m_settings = GlobalSettings::fromJson(j);
    } catch (...) {}
}

void App::saveSettings() {
    std::string p = m_appDir + "\\settings.json";
    std::ofstream f(p);
    if (f.good()) f << m_settings.toJson().dump(4);
}

// ── Profiles ──────────────────────────────────────────────────────────────────

void App::loadProfiles() {
    m_profiles.clear();
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(m_profilesDir, ec)) {
        if (entry.path().extension() != ".json") continue;
        try {
            std::ifstream f(entry.path());
            nlohmann::json j;
            f >> j;
            m_profiles.push_back(Profile::fromJson(j));
        } catch (...) {}
    }
}

void App::saveProfile(const Profile& p) {
    std::string path = m_profilesDir + "\\" + p.uuid + ".json";
    std::ofstream f(path);
    if (f.good()) f << p.toJson().dump(4);
}

void App::addProfile(Profile p) {
    if (p.uuid.empty()) p.uuid = generateUUID();
    p.updateBullets();
    saveProfile(p);
    m_profiles.push_back(std::move(p));
}

void App::updateProfile(const Profile& p) {
    for (auto& ep : m_profiles) {
        if (ep.uuid != p.uuid) continue;
        ep = p;
        ep.updateBullets();
        saveProfile(ep);
        return;
    }
}

void App::deleteProfile(const std::string& uuid) {
    m_profiles.erase(
        std::remove_if(m_profiles.begin(), m_profiles.end(),
            [&](const Profile& p) { return p.uuid == uuid; }),
        m_profiles.end());
    DeleteFileA((m_profilesDir + "\\" + uuid + ".json").c_str());
}

Profile* App::findProfile(const std::string& uuid) {
    for (auto& p : m_profiles)
        if (p.uuid == uuid) return &p;
    return nullptr;
}

// ── Macros ────────────────────────────────────────────────────────────────────

void App::loadMacros() {
    m_macros.clear();
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(m_macrosDir, ec)) {
        if (entry.path().extension() != ".json") continue;
        try {
            std::ifstream f(entry.path());
            nlohmann::json j;
            f >> j;
            m_macros.push_back(Macro::fromJson(j));
        } catch (...) {}
    }
}

void App::saveMacro(const Macro& m) {
    std::string path = m_macrosDir + "\\" + m.uuid + ".json";
    std::ofstream f(path);
    if (f.good()) f << m.toJson().dump(4);
}

void App::addMacro(Macro m) {
    if (m.uuid.empty()) m.uuid = generateUUID();
    saveMacro(m);
    m_macros.push_back(std::move(m));
}

void App::updateMacro(const Macro& m) {
    for (auto& em : m_macros) {
        if (em.uuid != m.uuid) continue;
        em = m;
        saveMacro(em);
        return;
    }
}

void App::deleteMacro(const std::string& uuid) {
    m_macros.erase(
        std::remove_if(m_macros.begin(), m_macros.end(),
            [&](const Macro& m) { return m.uuid == uuid; }),
        m_macros.end());
    DeleteFileA((m_macrosDir + "\\" + uuid + ".json").c_str());
}

Macro* App::findMacro(const std::string& uuid) {
    for (auto& m : m_macros)
        if (m.uuid == uuid) return &m;
    return nullptr;
}

void App::toggleMacro(const std::string& uuid) {
    for (auto& m : m_macros) {
        if (m.uuid != uuid) continue;
        m.enabled = !m.enabled;
        saveMacro(m);
        return;
    }
}
