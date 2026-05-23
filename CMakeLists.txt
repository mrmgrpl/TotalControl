cmake_minimum_required(VERSION 3.20)
project(TotalControl VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── CrSDK ───────────────────────────────────────────────────────────────────
set(CRSDK_ROOT    "${CMAKE_SOURCE_DIR}/external/CrSDK")
set(CRSDK_INCLUDE "${CRSDK_ROOT}/include")
set(CRSDK_LIB     "${CRSDK_ROOT}/lib/Cr_Core.lib")
set(CRSDK_BIN     "${CRSDK_ROOT}/bin")

# Walidacja — zatrzymuje build jeśli SDK nie na miejscu
foreach(_check
    "${CRSDK_INCLUDE}/CameraRemote_SDK.h"
    "${CRSDK_INCLUDE}/IDeviceCallback.h"
    "${CRSDK_LIB}"
    "${CRSDK_BIN}/Cr_Core.dll")
    if(NOT EXISTS "${_check}")
        message(FATAL_ERROR "Brak pliku CrSDK: ${_check}")
    endif()
endforeach()

# ─── Źródła ──────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/CameraController.cpp
    src/SequencerEngine.cpp
    src/visualization/Renderer3D.cpp
    src/visualization/Overlay2D.cpp
    src/visualization/CameraPreview.cpp
)

add_executable(TotalControl ${SOURCES})

# ─── Includes ────────────────────────────────────────────────────────────────
target_include_directories(TotalControl PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CRSDK_INCLUDE}
)

# ─── Linkowanie ──────────────────────────────────────────────────────────────
target_link_libraries(TotalControl PRIVATE
    ${CRSDK_LIB}
    d3d11.lib dxgi.lib d3dcompiler.lib
    d2d1.lib dwrite.lib
    windowscodecs.lib
    kernel32.lib user32.lib gdi32.lib
    ole32.lib oleaut32.lib uuid.lib shell32.lib
)

# ─── Definicje i flagi ───────────────────────────────────────────────────────
target_compile_definitions(TotalControl PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    _WIN32_WINNT=0x0A00
)

target_compile_options(TotalControl PRIVATE
    /W4 /WX- /MP /utf-8
)

# ─── Kopiowanie DLL-i po buildzie ────────────────────────────────────────────
add_custom_command(TARGET TotalControl POST_BUILD
    # DLL-e główne → katalog exe
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CRSDK_BIN}"
        "$<TARGET_FILE_DIR:TotalControl>"
    # DLL-e transportowe → podkatalog CrAdapter\
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:TotalControl>/CrAdapter"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CRSDK_BIN}/Cr_PTP_IP.dll"
        "${CRSDK_BIN}/Cr_PTP_USB.dll"
        "${CRSDK_BIN}/libusb-1.0.dll"
        "${CRSDK_BIN}/libssh2.dll"
        "$<TARGET_FILE_DIR:TotalControl>/CrAdapter/"
    COMMENT "Kopiowanie CrSDK DLL -> output + CrAdapter/"
)

message(STATUS "TotalControl | CrSDK include : ${CRSDK_INCLUDE}")
message(STATUS "TotalControl | CrSDK lib     : ${CRSDK_LIB}")
message(STATUS "TotalControl | CrSDK bin     : ${CRSDK_BIN}")