#include "ui.h"
#include "app.h"
#include "hook.h"
#include "compensation.h"
#include "imgui.h"

#include <string>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace UI {

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::vector<float> parseFloatCSV(const char* str) {
    std::vector<float> out;
    std::istringstream ss(str);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        auto b = tok.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        tok = tok.substr(b, tok.find_last_not_of(" \t\r\n") - b + 1);
        if (tok.empty()) continue;
        try { out.push_back(std::stof(tok)); } catch (...) {}
    }
    return out;
}

static std::string formatFloatCSV(const std::vector<float>& v) {
    std::string s;
    s.reserve(v.size() * 10);
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        char buf[32];
        snprintf(buf, sizeof(buf), "%.4f", v[i]);
        s += buf;
    }
    return s;
}

// ─── Profile editor state ────────────────────────────────────────────────────

static struct {
    bool        open   = false;
    bool        isNew  = true;
    std::string uuid;
    char        name[256]         = {};
    char        offsetX[1 << 16] = {};
    char        offsetY[1 << 16] = {};
    float       rpm               = 600.0f;
    char        err[256]          = {};
} g_pe;

static void openNewProfile() {
    g_pe = {};
    g_pe.open  = true;
    g_pe.isNew = true;
    g_pe.rpm   = 600.0f;
}

static void openEditProfile(const Profile& p) {
    g_pe      = {};
    g_pe.open  = true;
    g_pe.isNew = false;
    g_pe.uuid  = p.uuid;
    g_pe.rpm   = p.rpm;
    strncpy_s(g_pe.name,    p.name.c_str(),              _TRUNCATE);
    auto xs = formatFloatCSV(p.offset_x);
    auto ys = formatFloatCSV(p.offset_y);
    strncpy_s(g_pe.offsetX, xs.c_str(),                  _TRUNCATE);
    strncpy_s(g_pe.offsetY, ys.c_str(),                  _TRUNCATE);
}

// ─── Macro editor state ──────────────────────────────────────────────────────

static struct {
    bool          open      = false;
    bool          isNew     = true;
    std::string   uuid;
    char          name[256] = {};
    int           button    = 4;
    std::string   profileUUID;
    MacroSettings settings;
    bool          listening = false;
    char          err[256]  = {};
} g_me;

static void openNewMacro() {
    g_me          = {};
    g_me.open     = true;
    g_me.isNew    = true;
    g_me.button   = 4;
    g_me.settings = MacroSettings{};
}

static void openEditMacro(const Macro& m) {
    g_me             = {};
    g_me.open        = true;
    g_me.isNew       = false;
    g_me.uuid        = m.uuid;
    g_me.button      = m.trigger_button;
    g_me.profileUUID = m.profile_uuid;
    g_me.settings    = m.settings;
    strncpy_s(g_me.name, m.name.c_str(), _TRUNCATE);
}

// ─── Profile editor window ───────────────────────────────────────────────────

static void renderProfileEditor() {
    if (!g_pe.open) return;

    ImGui::SetNextWindowSize(ImVec2(720, 580), ImGuiCond_FirstUseEver);
    const char* title = g_pe.isNew ? "New Profile###PE" : "Edit Profile###PE";
    if (!ImGui::Begin(title, &g_pe.open)) { ImGui::End(); return; }

    ImGui::InputText("Name##pe", g_pe.name, sizeof(g_pe.name));
    ImGui::InputFloat("RPM##pe", &g_pe.rpm, 1.0f, 10.0f, "%.1f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Steps per minute — sets inter-step delay to 60000/RPM ms.");

    ImGui::Separator();
    ImGui::TextWrapped(
        "Enter offset values as comma-separated decimals.\n"
        "  Offset X: positive = cursor right, negative = cursor left.\n"
        "  Offset Y: positive = cursor down,  negative = cursor up.\n"
        "Both arrays must have the same number of entries (bullets).");
    ImGui::Spacing();

    ImGui::Text("Offset X  (comma-separated):");
    ImGui::InputTextMultiline("##oxpe", g_pe.offsetX, sizeof(g_pe.offsetX),
                              ImVec2(-1, 110));

    ImGui::Text("Offset Y  (comma-separated):");
    ImGui::InputTextMultiline("##oype", g_pe.offsetY, sizeof(g_pe.offsetY),
                              ImVec2(-1, 110));

    {
        auto xs = parseFloatCSV(g_pe.offsetX);
        auto ys = parseFloatCSV(g_pe.offsetY);
        ImGui::Text("Detected bullets: X=%d  Y=%d  (effective = min)",
                    (int)xs.size(), (int)ys.size());
    }

    if (g_pe.err[0])
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", g_pe.err);

    if (ImGui::Button("Save##pe")) {
        if (!g_pe.name[0]) {
            strncpy_s(g_pe.err, "Name cannot be empty.", _TRUNCATE);
        } else {
            Profile p;
            p.uuid     = g_pe.isNew ? generateUUID() : g_pe.uuid;
            p.name     = g_pe.name;
            p.rpm      = g_pe.rpm;
            p.offset_x = parseFloatCSV(g_pe.offsetX);
            p.offset_y = parseFloatCSV(g_pe.offsetY);
            p.updateBullets();
            if (g_pe.isNew) App::get().addProfile(std::move(p));
            else            App::get().updateProfile(p);
            g_pe.open = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel##pe")) g_pe.open = false;

    ImGui::End();
}

// ─── Macro editor window ─────────────────────────────────────────────────────

static void renderMacroEditor() {
    if (!g_me.open) return;

    // Poll button listener
    if (g_me.listening && HookManager::get().hasListened()) {
        g_me.button = HookManager::get().listenedButton();
        HookManager::get().clearListened();
        g_me.listening = false;
    }

    ImGui::SetNextWindowSize(ImVec2(480, 430), ImGuiCond_FirstUseEver);
    const char* title = g_me.isNew ? "Register Macro###ME" : "Edit Macro###ME";
    if (!ImGui::Begin(title, &g_me.open)) {
        if (g_me.listening) { HookManager::get().stopListening(); g_me.listening = false; }
        ImGui::End(); return;
    }

    ImGui::InputText("Name##me", g_me.name, sizeof(g_me.name));

    ImGui::Separator();
    ImGui::Text("Trigger button: %s", buttonName(g_me.button));
    if (g_me.listening) {
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "  Press any mouse button...");
        if (ImGui::Button("Cancel##listen")) {
            HookManager::get().stopListening();
            g_me.listening = false;
        }
    } else {
        if (ImGui::Button("Listen for button")) {
            HookManager::get().startListening();
            g_me.listening = true;
        }
    }

    ImGui::Separator();
    ImGui::Text("Profile:");

    auto& profs = App::get().profiles();
    if (profs.empty()) {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1),
            "No profiles — create one in the Profiles tab first.");
    } else {
        Profile* sel = App::get().findProfile(g_me.profileUUID);
        const char* preview = sel ? sel->name.c_str() : "— select —";
        if (ImGui::BeginCombo("##profileme", preview)) {
            for (const auto& p : profs) {
                bool active = (p.uuid == g_me.profileUUID);
                if (ImGui::Selectable(p.name.c_str(), active))
                    g_me.profileUUID = p.uuid;
                if (active) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();
    ImGui::Text("Profile settings:");
    ImGui::SliderFloat("Magnification##me", &g_me.settings.magnification,
                       0.1f, 20.0f, "%.2fx");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Multiplies the abstract offset values before applying the screen multiplier.");
    ImGui::Checkbox("Smoothing##me", &g_me.settings.use_smoothing);
    if (g_me.settings.use_smoothing) {
        ImGui::SliderInt("Smoothing steps##me", &g_me.settings.smoothing_steps, 1, 10);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Each offset step is linearly interpolated across N sub-steps.");
    }

    if (g_me.err[0])
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", g_me.err);

    if (ImGui::Button("Save##me")) {
        if (!g_me.name[0]) {
            strncpy_s(g_me.err, "Name cannot be empty.", _TRUNCATE);
        } else if (g_me.profileUUID.empty()) {
            strncpy_s(g_me.err, "Please select a profile.", _TRUNCATE);
        } else {
            Macro m;
            m.uuid           = g_me.isNew ? generateUUID() : g_me.uuid;
            m.name           = g_me.name;
            m.trigger_button = g_me.button;
            m.profile_uuid   = g_me.profileUUID;
            m.settings       = g_me.settings;
            m.enabled        = true;
            if (g_me.isNew) App::get().addMacro(std::move(m));
            else            App::get().updateMacro(m);
            if (g_me.listening) { HookManager::get().stopListening(); g_me.listening = false; }
            g_me.open = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel##me")) {
        if (g_me.listening) { HookManager::get().stopListening(); g_me.listening = false; }
        g_me.open = false;
    }

    ImGui::End();
}

// ─── Tab: Home ───────────────────────────────────────────────────────────────

static void renderHomeTab() {
    ImGui::TextDisabled("Registered macros — toggle on/off to activate in the background.");
    ImGui::Separator();

    auto& macros = App::get().macros();
    if (macros.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1),
            "No macros yet. Go to the Macros tab and click '+ Register Macro'.");
        return;
    }

    constexpr ImGuiTableFlags tflags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable("##home", 5, tflags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("",         ImGuiTableColumnFlags_WidthFixed,   50);
        ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Button",   ImGuiTableColumnFlags_WidthFixed,  120);
        ImGui::TableSetupColumn("Profile",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status",   ImGuiTableColumnFlags_WidthFixed,   80);
        ImGui::TableHeadersRow();

        for (auto& macro : macros) {
            ImGui::TableNextRow();
            ImGui::PushID(macro.uuid.c_str());

            // Enabled checkbox
            ImGui::TableSetColumnIndex(0);
            bool en = macro.enabled;
            if (ImGui::Checkbox("##en", &en))
                App::get().toggleMacro(macro.uuid);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", macro.name.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s", buttonName(macro.trigger_button));

            ImGui::TableSetColumnIndex(3);
            Profile* p = App::get().findProfile(macro.profile_uuid);
            if (p) ImGui::Text("%s", p->name.c_str());
            else   ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "(missing)");

            ImGui::TableSetColumnIndex(4);
            bool running = CompensationEngine::get().isRunning(macro.uuid);
            if (running)
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1), "ACTIVE");
            else if (macro.enabled)
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1), "Ready");
            else
                ImGui::TextColored(ImVec4(0.45f,0.45f,0.45f,1), "Off");

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ─── Tab: Profiles ───────────────────────────────────────────────────────────

static void renderProfilesTab() {
    if (ImGui::Button("+ New Profile"))
        openNewProfile();
    ImGui::Separator();

    auto& profs = App::get().profiles();
    if (profs.empty()) {
        ImGui::TextColored(ImVec4(0.6f,0.6f,0.6f,1),
            "No profiles yet. Click '+ New Profile' to create one.");
        return;
    }

    static std::string deleteUUID;

    constexpr ImGuiTableFlags tf =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable("##profiles", 5, tf)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("RPM",     ImGuiTableColumnFlags_WidthFixed,  70);
        ImGui::TableSetupColumn("Bullets", ImGuiTableColumnFlags_WidthFixed,  70);
        ImGui::TableSetupColumn("UUID",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 115);
        ImGui::TableHeadersRow();

        for (const auto& p : profs) {
            ImGui::TableNextRow();
            ImGui::PushID(p.uuid.c_str());

            ImGui::TableSetColumnIndex(0); ImGui::Text("%s", p.name.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.0f", p.rpm);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%d", p.bullets);
            ImGui::TableSetColumnIndex(3); ImGui::TextDisabled("%s", p.uuid.c_str());
            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("Edit"))   openEditProfile(p);
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) deleteUUID = p.uuid;

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (!deleteUUID.empty()) ImGui::OpenPopup("Delete Profile?##dp");

    if (ImGui::BeginPopupModal("Delete Profile?##dp", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete this profile? Macros referencing it will show (missing).");
        ImGui::Separator();
        if (ImGui::Button("Delete", ImVec2(110, 0))) {
            App::get().deleteProfile(deleteUUID);
            deleteUUID.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0))) {
            deleteUUID.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ─── Tab: Macros ─────────────────────────────────────────────────────────────

static void renderMacrosTab() {
    if (ImGui::Button("+ Register Macro"))
        openNewMacro();
    ImGui::Separator();

    auto& macros = App::get().macros();
    if (macros.empty()) {
        ImGui::TextColored(ImVec4(0.6f,0.6f,0.6f,1),
            "No macros yet. Click '+ Register Macro' to bind a mouse button to a profile.");
        return;
    }

    static std::string deleteMacroUUID;

    constexpr ImGuiTableFlags tf =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable("##macros", 6, tf)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Button",  ImGuiTableColumnFlags_WidthFixed,  115);
        ImGui::TableSetupColumn("Profile", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Mag.",    ImGuiTableColumnFlags_WidthFixed,   55);
        ImGui::TableSetupColumn("On",      ImGuiTableColumnFlags_WidthFixed,   35);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed,  115);
        ImGui::TableHeadersRow();

        for (const auto& m : macros) {
            ImGui::TableNextRow();
            ImGui::PushID(m.uuid.c_str());

            ImGui::TableSetColumnIndex(0); ImGui::Text("%s", m.name.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::Text("%s", buttonName(m.trigger_button));
            ImGui::TableSetColumnIndex(2);
            {
                Profile* p = App::get().findProfile(m.profile_uuid);
                if (p) ImGui::Text("%s", p->name.c_str());
                else   ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "(missing)");
            }
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.1fx", m.settings.magnification);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%s", m.enabled ? "Y" : "N");
            ImGui::TableSetColumnIndex(5);
            if (ImGui::SmallButton("Edit"))   openEditMacro(m);
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) deleteMacroUUID = m.uuid;

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (!deleteMacroUUID.empty()) ImGui::OpenPopup("Delete Macro?##dm");

    if (ImGui::BeginPopupModal("Delete Macro?##dm", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete this macro?");
        ImGui::Separator();
        if (ImGui::Button("Delete", ImVec2(110, 0))) {
            CompensationEngine::get().stop(deleteMacroUUID);
            App::get().deleteMacro(deleteMacroUUID);
            deleteMacroUUID.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0))) {
            deleteMacroUUID.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ─── Tab: Settings ───────────────────────────────────────────────────────────

static void renderSettingsTab() {
    GlobalSettings& s = App::get().settings();

    ImGui::Text("Global Compensation Settings");
    ImGui::TextDisabled("These affect every macro.");
    ImGui::Separator();

    bool dirty = false;
    dirty |= ImGui::SliderFloat("Sensitivity", &s.sensitivity, 0.05f, 3.0f, "%.3f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Input sensitivity scalar (game setting).");

    dirty |= ImGui::SliderFloat("FOV", &s.fov, 20.0f, 150.0f, "%.1f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Field-of-view scalar (game setting).");

    dirty |= ImGui::SliderFloat("Randomness", &s.randomness_percent,
                                0.0f, 25.0f, "%.1f %%");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Adds ±N%% random variation to each offset step.\n"
                          "Keeps movement natural; default 5%%.");

    ImGui::Spacing();
    ImGui::Separator();
    float sm = s.screenMultiplier();
    ImGui::Text("Computed screen multiplier:  %.6f", sm);
    ImGui::TextDisabled("  = -0.03 × (sensitivity × 3) × (FOV / 100)");

    ImGui::Separator();
    if (ImGui::Button("Save Settings") || (dirty && ImGui::IsItemDeactivatedAfterEdit()))
        App::get().saveSettings();
    ImGui::SameLine();
    ImGui::TextDisabled("  Saved to:  <exe dir>\\settings.json");

    (void)dirty; // suppress unused warning; save is explicit
}

// ─── Main window ─────────────────────────────────────────────────────────────

void renderMainWindow() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    constexpr ImGuiWindowFlags wf =
        ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoResize   |
        ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_NoCollapse  |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##root", nullptr, wf);

    ImGui::Text("Cursor Compensation System");
    ImGui::SameLine();
    ImGui::TextDisabled("v1.0.0");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 180);
    ImGui::TextDisabled("Minimize to hide to tray");
    ImGui::Separator();

    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Home"))     { renderHomeTab();     ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Profiles")) { renderProfilesTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Macros"))   { renderMacrosTab();   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Settings")) { renderSettingsTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    ImGui::End();

    // Float editor windows on top
    renderProfileEditor();
    renderMacroEditor();
}

} // namespace UI
