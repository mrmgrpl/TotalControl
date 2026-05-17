// TotalControl — Sony A7R IVA camera control for TSE 2026
// (c) Andrzej Nowak — GPL v3

#include <windows.h>

int WINAPI WinMain(
    _In_     HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_     LPSTR     lpCmdLine,
    _In_     int       nCmdShow)
{
    MessageBoxW(nullptr,
        L"TotalControl v1.0\nTSE 2026 — Burgos, Spain",
        L"TotalControl",
        MB_OK | MB_ICONINFORMATION);

    return 0;
}