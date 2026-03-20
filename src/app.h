#pragma once
#include "data.h"
#include <vector>
#include <string>

class App {
public:
    static App& get();

    void init();

    // ── Profiles ────────────────────────────────────────────────────────────
    std::vector<Profile>&  profiles()               { return m_profiles; }
    void                   addProfile(Profile p);
    void                   updateProfile(const Profile& p);
    void                   deleteProfile(const std::string& uuid);
    Profile*               findProfile(const std::string& uuid);

    // ── Macros ──────────────────────────────────────────────────────────────
    std::vector<Macro>&    macros()                 { return m_macros; }
    void                   addMacro(Macro m);
    void                   updateMacro(const Macro& m);
    void                   deleteMacro(const std::string& uuid);
    Macro*                 findMacro(const std::string& uuid);
    void                   toggleMacro(const std::string& uuid);

    // ── Settings ────────────────────────────────────────────────────────────
    GlobalSettings&        settings()               { return m_settings; }
    void                   saveSettings();

    const std::string&     appDir()    const        { return m_appDir; }

private:
    App() = default;

    std::string            m_appDir;
    std::string            m_profilesDir;
    std::string            m_macrosDir;

    std::vector<Profile>   m_profiles;
    std::vector<Macro>     m_macros;
    GlobalSettings         m_settings;

    void saveProfile(const Profile& p);
    void saveMacro(const Macro& m);
    void loadProfiles();
    void loadMacros();
    void loadSettings();
    void ensureDir(const std::string& path);
};
