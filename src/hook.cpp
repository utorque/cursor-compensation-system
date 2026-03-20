#include "hook.h"
#include "app.h"
#include "compensation.h"

HookManager& HookManager::get() {
    static HookManager instance;
    return instance;
}

void HookManager::install() {
    if (m_hook) return;
    m_hook = SetWindowsHookExA(WH_MOUSE_LL, hookProc, GetModuleHandleA(nullptr), 0);
}

void HookManager::uninstall() {
    if (!m_hook) return;
    UnhookWindowsHookEx(m_hook);
    m_hook = nullptr;
}

void HookManager::startListening() {
    m_listenedButton.store(-1);
    m_listened.store(false);
    m_listening.store(true);
}

void HookManager::stopListening() {
    m_listening.store(false);
}

int HookManager::buttonFromMsg(WPARAM wParam, LPARAM lParam) {
    switch (wParam) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:   return 1;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:   return 2;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:   return 3;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP: {
            auto* mhs = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            return (HIWORD(mhs->mouseData) == XBUTTON1) ? 4 : 5;
        }
        default: return -1;
    }
}

LRESULT CALLBACK HookManager::hookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        bool isDown = (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
                       wParam == WM_MBUTTONDOWN || wParam == WM_XBUTTONDOWN);
        bool isUp   = (wParam == WM_LBUTTONUP   || wParam == WM_RBUTTONUP   ||
                       wParam == WM_MBUTTONUP   || wParam == WM_XBUTTONUP);

        if (isDown || isUp) {
            int btn = buttonFromMsg(wParam, lParam);
            if (btn > 0) {
                HookManager& hm = get();
                if (isDown) hm.onButtonDown(btn);
                else        hm.onButtonUp(btn);
            }
        }
    }
    // Always pass the event on — nothing is blocked
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void HookManager::onButtonDown(int button) {
    // ── Listen mode (macro registration) ────────────────────────────────────
    if (m_listening.load()) {
        m_listenedButton.store(button);
        m_listened.store(true);
        m_listening.store(false);
        return;   // don't trigger compensation while capturing a button
    }

    if (m_pressed[button]) return;   // already tracked as down
    m_pressed[button] = true;

    // ── Trigger compensation for every enabled macro on this button ──────────
    for (const auto& macro : App::get().macros()) {
        if (macro.enabled && macro.trigger_button == button)
            CompensationEngine::get().start(macro.uuid);
    }
}

void HookManager::onButtonUp(int button) {
    m_pressed[button] = false;

    // ── Stop compensation for macros on this button ──────────────────────────
    for (const auto& macro : App::get().macros()) {
        if (macro.enabled && macro.trigger_button == button)
            CompensationEngine::get().stop(macro.uuid);
    }
}
