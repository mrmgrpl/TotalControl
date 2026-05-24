cmake_minimum_required(VERSION 3.20)
project(TotalControl VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
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
add_executable(TotalControlCLI src/cli/main.cpp)

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

# ─── TotalControlGUI.exe — graficzny interfejs (placeholder) ─────────────────
# add_executable(TotalControlGUI src/gui/main.cpp)
# ... (do implementacji)

message(STATUS "TotalControl | CrSDK include : ${CRSDK_INCLUDE}")
message(STATUS "TotalControl | CrSDK lib     : ${CRSDK_LIB}")
message(STATUS "TotalControl | CrSDK bin     : ${CRSDK_BIN}")
