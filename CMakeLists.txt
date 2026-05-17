cmake_minimum_required(VERSION 3.20)
project(TotalControl VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Katalogi nagłówków
include_directories(include)

# Pliki źródłowe
add_executable(TotalControl WIN32
    src/main.cpp
    src/CameraController.cpp
    src/SequencerEngine.cpp
    src/visualization/Renderer3D.cpp
    src/visualization/Overlay2D.cpp
    src/visualization/CameraPreview.cpp
)

# Windows natywne biblioteki — zero zewnętrznych zależności
target_link_libraries(TotalControl PRIVATE
    d3d11.lib
    d3dcompiler.lib
    dxgi.lib
    d2d1.lib
    dwrite.lib
    windowscodecs.lib
    user32.lib
    gdi32.lib
    comctl32.lib
)

# Tryb Release — optymalizacja latencji
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(TotalControl PRIVATE /O2 /GL)
endif()

# Tryb Debug — diagnostyka
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(TotalControl PRIVATE /Od /Zi)
endif()