cmake_minimum_required(VERSION 4.3.3)
project(TotalControl VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── CrSDK ───────────────────────────────────────────────────────────────────
set(CRSDK_ROOT    "${CMAKE_SOURCE_DIR}/external/CrSDK")
set(CRSDK_INCLUDE "${CRSDK_ROOT}/include")
set(CRSDK_LIB     "${CRSDK_ROOT}/lib/Cr_Core.lib")
set(CRSDK_BIN     "${CRSDK_ROOT}/bin")

foreach(_check
    "${CRSDK_INCLUDE}/CameraRemote_SDK.h"
    "${CRSDK_INCLUDE}/IDeviceCallback.h"
    "${CRSDK_LIB}"
    "${CRSDK_BIN}/Cr_Core.dll")
    if(NOT EXISTS "${_check}")
        message(FATAL_ERROR "Brak pliku CrSDK: ${_check}")
    endif()
endforeach()

# ─── TotalControlSRV.exe — daemon (trzyma połączenie z kamerą, pipe server) ──
set(SRV_SOURCES
    src/main.cpp
    src/CameraController.cpp
    src/daemon/PipeServer.cpp
    src/daemon/CommandHandler.cpp
    src/SequencerEngine.cpp
    src/visualization/Renderer3D.cpp
    src/visualization/Overlay2D.cpp
    src/visualization/CameraPreview.cpp
    src/srv.rc
)

add_executable(TotalControlSRV ${SRV_SOURCES})

target_include_directories(TotalControlSRV PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CRSDK_INCLUDE}
)

target_link_libraries(TotalControlSRV PRIVATE
    ${CRSDK_LIB}
    d3d11.lib dxgi.lib d3dcompiler.lib
    d2d1.lib dwrite.lib
    windowscodecs.lib
    kernel32.lib user32.lib gdi32.lib
    ole32.lib oleaut32.lib uuid.lib shell32.lib
)

target_compile_definitions(TotalControlSRV PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    _WIN32_WINNT=0x0A00
)

target_compile_options(TotalControlSRV PRIVATE
    /W4 /WX /MP /utf-8
    /external:I "${CRSDK_INCLUDE}" /external:W0
)

# Kopiowanie DLL-i CrSDK po buildzie
add_custom_command(TARGET TotalControlSRV POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CRSDK_BIN}"
        "$<TARGET_FILE_DIR:TotalControlSRV>"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:TotalControlSRV>/CrAdapter"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CRSDK_BIN}/Cr_PTP_IP.dll"
        "${CRSDK_BIN}/Cr_PTP_USB.dll"
        "${CRSDK_BIN}/libusb-1.0.dll"
        "${CRSDK_BIN}/libssh2.dll"
        "$<TARGET_FILE_DIR:TotalControlSRV>/CrAdapter/"
    COMMENT "Kopiowanie CrSDK DLL -> output + CrAdapter/"
)

# ─── TotalControlCLI.exe — thin CLI client (bez CrSDK) ───────────────────────
add_executable(TotalControlCLI src/cli/main.cpp src/cli/cli.rc)

target_include_directories(TotalControlCLI PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(TotalControlCLI PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    _WIN32_WINNT=0x0A00
)

target_compile_options(TotalControlCLI PRIVATE
    /W4 /WX /MP /utf-8
)

target_link_libraries(TotalControlCLI PRIVATE
    kernel32.lib user32.lib
)

# TotalControlCLI.exe obok TotalControlSRV.exe
add_custom_command(TARGET TotalControlCLI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:TotalControlCLI>"
        "$<TARGET_FILE_DIR:TotalControlSRV>/TotalControlCLI.exe"
    COMMENT "Kopiowanie TotalControlCLI.exe -> output SRV"
)

# ─── SQLite3 — amalgamation, static lib ──────────────────────────────────────
set(SQLITE_ROOT "${CMAKE_SOURCE_DIR}/external/sqlite")
add_library(sqlite3 STATIC "${SQLITE_ROOT}/sqlite3.c")
target_include_directories(sqlite3 PUBLIC ${SQLITE_ROOT})
target_compile_options(sqlite3 PRIVATE /W0 /MP)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
)

# ─── Dear ImGui — built as a static library (no /WX, no warnings) ────────────
set(IMGUI_ROOT     "${CMAKE_SOURCE_DIR}/external/imgui")
set(IMGUI_BACKENDS "${IMGUI_ROOT}/backends")

add_library(imgui STATIC
    "${IMGUI_ROOT}/imgui.cpp"
    "${IMGUI_ROOT}/imgui_draw.cpp"
    "${IMGUI_ROOT}/imgui_tables.cpp"
    "${IMGUI_ROOT}/imgui_widgets.cpp"
    "${IMGUI_ROOT}/imgui_demo.cpp"
    "${IMGUI_BACKENDS}/imgui_impl_win32.cpp"
    "${IMGUI_BACKENDS}/imgui_impl_dx11.cpp"
)

target_include_directories(imgui PUBLIC
    ${IMGUI_ROOT}
    ${IMGUI_BACKENDS}
)

target_compile_definitions(imgui PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    _WIN32_WINNT=0x0A00
)

# No /WX — third-party code, warnings suppressed
target_compile_options(imgui PRIVATE /W0 /MP /utf-8)

# ─── TotalControlGUI.exe — mission control GUI ───────────────────────────────
add_executable(TotalControlGUI WIN32
    src/gui/main_gui.cpp
    src/gui/App.cpp
    src/gui/PipeClient.cpp
    src/gui/Database.cpp
    src/gui/IqpClient.cpp
    src/gui/BesselCalc.cpp
    src/gui/gui.rc
)

target_include_directories(TotalControlGUI PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/gui
    ${IMGUI_ROOT}
    ${IMGUI_BACKENDS}
)

target_compile_definitions(TotalControlGUI PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    _WIN32_WINNT=0x0A00
)

target_compile_options(TotalControlGUI PRIVATE
    /W4 /WX /MP /utf-8
)

target_link_libraries(TotalControlGUI PRIVATE
    imgui
    sqlite3
    d3d11.lib dxgi.lib
    winhttp.lib
    kernel32.lib user32.lib gdi32.lib
    ole32.lib oleaut32.lib uuid.lib shell32.lib
)

# Copy TotalControlData.db next to SRV exe (read-only reference database)
add_custom_command(TARGET TotalControlGUI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/data/TotalControlData.db"
        "$<TARGET_FILE_DIR:TotalControlSRV>/TotalControlData.db"
    COMMENT "Copy TotalControlData.db -> SRV output dir"
)

# Copy GUI exe next to SRV exe
add_custom_command(TARGET TotalControlGUI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:TotalControlGUI>"
        "$<TARGET_FILE_DIR:TotalControlSRV>/TotalControlGUI.exe"
    COMMENT "Copy TotalControlGUI.exe -> SRV output dir"
)

message(STATUS "TotalControl | CrSDK include : ${CRSDK_INCLUDE}")
message(STATUS "TotalControl | CrSDK lib     : ${CRSDK_LIB}")
message(STATUS "TotalControl | CrSDK bin     : ${CRSDK_BIN}")
