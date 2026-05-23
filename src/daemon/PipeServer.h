#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <string>

namespace TotalControl {

// handler(request_json) → response_json
// Zwraca false → pętla pipe kończy się (quit)
using PipeHandlerFn = std::function<bool(const std::wstring&, std::wstring&)>;

class PipeServer {
public:
    PipeServer(const wchar_t* pipeName, PipeHandlerFn handler);
    ~PipeServer();

    void Run();   // blokująca pętla accept/handle
    void Stop();  // thread-safe, przerywa Run()

private:
    std::wstring      m_pipeName;
    PipeHandlerFn     m_handler;
    HANDLE            m_stopEvent = INVALID_HANDLE_VALUE;
    std::atomic<bool> m_running   { true };
};

} // namespace TotalControl
