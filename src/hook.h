#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <map>

// Installs a WH_MOUSE_LL hook.
//  • In normal mode   : forwards button-down/up events to the CompensationEngine.
//  • In listen mode   : captures the next button press (for macro registration UI).
// All intercepted events are forwarded to the OS — no clicks are blocked.
class HookManager {
public:
    static HookManager& get();

    void install();
    void uninstall();

    // ── Macro-registration button listener ───────────────────────────────────
    void startListening();          // enter "press any button" mode
    void stopListening();
    bool isListening()  const { return m_listening.load(); }
    bool hasListened()  const { return m_listened.load(); }
    int  listenedButton() const { return m_listenedButton.load(); }
    void clearListened() { m_listened.store(false); m_listenedButton.store(-1); }

private:
    HookManager() = default;

    HHOOK               m_hook = nullptr;
    std::atomic<bool>   m_listening{false};
    std::atomic<bool>   m_listened{false};
    std::atomic<int>    m_listenedButton{-1};

    std::map<int,bool>  m_pressed;   // last known down-state per button

    void onButtonDown(int button);
    void onButtonUp(int button);

    static int  buttonFromMsg(WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK hookProc(int nCode, WPARAM wParam, LPARAM lParam);
};
