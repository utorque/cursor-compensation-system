#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>   // CoCreateGuid

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// ─── Utilities ────────────────────────────────────────────────────────────────

inline std::string generateUUID() {
    GUID g;
    CoCreateGuid(&g);
    char buf[40];
    snprintf(buf, sizeof(buf),
        "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1],
        g.Data4[2], g.Data4[3], g.Data4[4],
        g.Data4[5], g.Data4[6], g.Data4[7]);
    return std::string(buf);
}

inline const char* buttonName(int btn) {
    switch (btn) {
        case 1: return "Left (LMB)";
        case 2: return "Right (RMB)";
        case 3: return "Middle (MMB)";
        case 4: return "Side 1 (XB1)";
        case 5: return "Side 2 (XB2)";
        default: return "Unknown";
    }
}

// ─── Profile ──────────────────────────────────────────────────────────────────

struct Profile {
    std::string uuid;
    std::string name;
    std::vector<float> offset_x;   // horizontal delta per step (abstract units)
    std::vector<float> offset_y;   // vertical delta per step   (abstract units)
    float rpm     = 600.0f;        // action rate — steps per minute
    int   bullets = 0;             // auto-derived: min(offset_x.size, offset_y.size)
    bool  semi    = false;         // true = semi-auto (one click → one step + LMB click)

    void updateBullets() {
        bullets = (int)std::min(offset_x.size(), offset_y.size());
    }

    nlohmann::json toJson() const {
        return {
            {"uuid",     uuid},
            {"name",     name},
            {"offset_x", offset_x},
            {"offset_y", offset_y},
            {"rpm",      rpm},
            {"bullets",  bullets},
            {"semi",     semi}
        };
    }

    static Profile fromJson(const nlohmann::json& j) {
        Profile p;
        p.uuid     = j.value("uuid",     generateUUID());
        p.name     = j.value("name",     "Unnamed Profile");
        p.offset_x = j.value("offset_x", std::vector<float>{});
        p.offset_y = j.value("offset_y", std::vector<float>{});
        p.rpm      = j.value("rpm",      600.0f);
        p.bullets  = j.value("bullets",  0);
        p.semi     = j.value("semi",     false);
        return p;
    }
};

// ─── Per-macro settings (applied on top of profile data) ─────────────────────

struct MacroSettings {
    float magnification  = 1.0f;   // multiplies abstract offsets before scaling
    bool  use_smoothing  = true;   // interpolate each step over sub-steps
    int   smoothing_steps = 3;     // number of sub-steps per offset entry

    nlohmann::json toJson() const {
        return {
            {"magnification",   magnification},
            {"use_smoothing",   use_smoothing},
            {"smoothing_steps", smoothing_steps}
        };
    }

    static MacroSettings fromJson(const nlohmann::json& j) {
        MacroSettings s;
        s.magnification   = j.value("magnification",   1.0f);
        s.use_smoothing   = j.value("use_smoothing",   true);
        s.smoothing_steps = j.value("smoothing_steps", 3);
        return s;
    }
};

// ─── Macro ────────────────────────────────────────────────────────────────────

struct Macro {
    std::string  uuid;
    std::string  name;
    int          trigger_button  = 4;   // 1-LMB 2-RMB 3-MMB 4-XB1 5-XB2
    std::string  profile_uuid;
    MacroSettings settings;
    bool         enabled         = true;

    nlohmann::json toJson() const {
        return {
            {"uuid",           uuid},
            {"name",           name},
            {"trigger_button", trigger_button},
            {"profile_uuid",   profile_uuid},
            {"settings",       settings.toJson()},
            {"enabled",        enabled}
        };
    }

    static Macro fromJson(const nlohmann::json& j) {
        Macro m;
        m.uuid           = j.value("uuid",           generateUUID());
        m.name           = j.value("name",           "Unnamed Macro");
        m.trigger_button = j.value("trigger_button", 4);
        m.profile_uuid   = j.value("profile_uuid",   std::string{});
        if (j.contains("settings")) m.settings = MacroSettings::fromJson(j.at("settings"));
        m.enabled        = j.value("enabled",        true);
        return m;
    }
};

// ─── Global settings ──────────────────────────────────────────────────────────

struct GlobalSettings {
    float sensitivity         = 0.4f;  // input sensitivity scalar
    float fov                 = 78.0f; // field-of-view scalar
    float randomness_percent  = 5.0f;  // ±N% random variation on each offset

    // Derived pixel-scale formula (matches original Lua implementation):
    //   screenMultiplier = -0.03 * (sensitivity * 3) * (fov / 100)
    float screenMultiplier() const {
        return -0.03f * (sensitivity * 3.0f) * (fov / 100.0f);
    }

    nlohmann::json toJson() const {
        return {
            {"sensitivity",        sensitivity},
            {"fov",                fov},
            {"randomness_percent", randomness_percent}
        };
    }

    static GlobalSettings fromJson(const nlohmann::json& j) {
        GlobalSettings s;
        s.sensitivity        = j.value("sensitivity",        0.4f);
        s.fov                = j.value("fov",                78.0f);
        s.randomness_percent = j.value("randomness_percent", 5.0f);
        return s;
    }
};
