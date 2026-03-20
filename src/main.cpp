// Cursor Compensation System — Win32 + Direct3D 11 + Dear ImGui entry point
// ─────────────────────────────────────────────────────────────────────────────
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "app.h"
#include "hook.h"
#include "compensation.h"
#include "ui.h"

// ─── D3D11 globals ───────────────────────────────────────────────────────────
static ID3D11Device*            g_device          = nullptr;
static ID3D11DeviceContext*     g_ctx             = nullptr;
static IDXGISwapChain*          g_swapChain       = nullptr;
static ID3D11RenderTargetView*  g_rtv             = nullptr;
static UINT                     g_resizeW = 0, g_resizeH = 0;

static bool CreateDeviceD3D(HWND hWnd);
static void CleanupDeviceD3D();
static void CreateRenderTarget();
static void CleanupRenderTarget();

// ─── System tray ─────────────────────────────────────────────────────────────
#define WM_TRAYICON  (WM_USER + 1)
#define IDM_SHOW     2001
#define IDM_EXIT     2002

static NOTIFYICONDATAA g_nid = {};

static void TrayAdd(HWND hWnd) {
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hWnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIconA(nullptr, IDI_APPLICATION);
    strncpy_s(g_nid.szTip, "Cursor Compensation System", _TRUNCATE);
    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

static void TrayRemove() {
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
}

static void TrayMenu(HWND hWnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU hm = CreatePopupMenu();
    AppendMenuA(hm, MF_STRING, IDM_SHOW, "Show");
    AppendMenuA(hm, MF_STRING, IDM_EXIT, "Exit");
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hm, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(hm);
}

// ─── Window procedure ────────────────────────────────────────────────────────
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return TRUE;

    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            ShowWindow(hWnd, SW_HIDE);   // minimize to tray
            return 0;
        }
        g_resizeW = LOWORD(lParam);
        g_resizeH = HIWORD(lParam);
        return 0;

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hWnd, SW_SHOW);
            SetForegroundWindow(hWnd);
        } else if (lParam == WM_RBUTTONUP) {
            TrayMenu(hWnd);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDM_EXIT)  { PostQuitMessage(0); return 0; }
        if (LOWORD(wParam) == IDM_SHOW)  {
            ShowWindow(hWnd, SW_SHOW);
            SetForegroundWindow(hWnd);
            return 0;
        }
        break;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;  // suppress alt-menu beep
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

// ─── Entry point ─────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    App::get().init();

    // Register window class
    WNDCLASSEXA wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconA(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = "CursorCompSystem";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowA(
        wc.lpszClassName, "Cursor Compensation System",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 680,
        nullptr, nullptr, hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassA(wc.lpszClassName, hInstance);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    TrayAdd(hwnd);

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename  = nullptr;  // don't write imgui.ini

    ImGui::StyleColorsDark();
    // Slightly tweak the dark theme for readability
    ImGuiStyle& style          = ImGui::GetStyle();
    style.WindowRounding       = 4.0f;
    style.FrameRounding        = 3.0f;
    style.ScrollbarRounding    = 3.0f;
    style.GrabRounding         = 3.0f;
    style.TabRounding           = 3.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_ctx);

    // Install global mouse hook (must be on the thread that pumps messages)
    HookManager::get().install();

    // ── Main loop ────────────────────────────────────────────────────────────
    const ImVec4 clearCol = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    bool done = false;

    while (!done) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // Handle resize
        if (g_resizeW && g_resizeH) {
            CleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        UI::renderMainWindow();

        ImGui::Render();
        const float cc[4] = { clearCol.x, clearCol.y, clearCol.z, clearCol.w };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, cc);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);   // vsync
    }

    // ── Cleanup ──────────────────────────────────────────────────────────────
    CompensationEngine::get().stopAll();
    HookManager::get().uninstall();
    TrayRemove();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, hInstance);
    return 0;
}

// ─── D3D11 helpers ───────────────────────────────────────────────────────────
static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd              = {};
    sd.BufferCount                       = 2;
    sd.BufferDesc.Format                 = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator  = 60;
    sd.BufferDesc.RefreshRate.Denominator= 1;
    sd.Flags                             = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage                       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                      = hWnd;
    sd.SampleDesc.Count                  = 1;
    sd.Windowed                          = TRUE;
    sd.SwapEffect                        = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION,
        &sd, &g_swapChain, &g_device, &level, &g_ctx);

    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION,
            &sd, &g_swapChain, &g_device, &level, &g_ctx);

    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_ctx)       { g_ctx->Release();       g_ctx       = nullptr; }
    if (g_device)    { g_device->Release();    g_device    = nullptr; }
}

static void CreateRenderTarget() {
    ID3D11Texture2D* pBack = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&pBack));
    if (pBack) {
        g_device->CreateRenderTargetView(pBack, nullptr, &g_rtv);
        pBack->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}
