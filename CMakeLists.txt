cmake_minimum_required(VERSION 3.20)
project(CursorCompensation VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ── Dear ImGui ────────────────────────────────────────────────────────────────
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.6
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()
set(IMGUI_DIR ${imgui_SOURCE_DIR})

# ── nlohmann/json ─────────────────────────────────────────────────────────────
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests  OFF CACHE INTERNAL "")
set(JSON_Install     OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# ── Executable ────────────────────────────────────────────────────────────────
add_executable(CursorCompensation WIN32
    src/main.cpp
    src/app.cpp
    src/compensation.cpp
    src/hook.cpp
    src/ui.cpp
    # ImGui core
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    # ImGui Win32 + D3D11 backend
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

target_include_directories(CursorCompensation PRIVATE
    src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(CursorCompensation PRIVATE
    nlohmann_json::nlohmann_json
    d3d11
    dxgi
    ole32       # CoCreateGuid
    shell32     # Shell_NotifyIcon
    user32
    gdi32
    dwmapi
)

if(MSVC)
    target_compile_options(CursorCompensation PRIVATE /W3 /MP /utf-8)
    # Remove default console subsystem added by CMake for WIN32 targets
    set_target_properties(CursorCompensation PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS"
    )
    # Static runtime so the .exe works without VC redistributables
    set_property(TARGET CursorCompensation PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
