#include "PipeServer.h"
#include <string>

namespace TotalControl {

PipeServer::PipeServer(const wchar_t* pipeName, PipeHandlerFn handler)
    : m_pipeName(pipeName), m_handler(std::move(handler))
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

PipeServer::~PipeServer() {
    Stop();
    if (m_stopEvent != INVALID_HANDLE_VALUE) CloseHandle(m_stopEvent);
}

void PipeServer::Stop() {
    m_running = false;
    if (m_stopEvent != INVALID_HANDLE_VALUE) SetEvent(m_stopEvent);
}

void PipeServer::Run() {
    while (m_running) {
        HANDLE pipe = CreateNamedPipeW(
            m_pipeName.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 4096, 4096, 0, nullptr);

        if (pipe == INVALID_HANDLE_VALUE) break;

        // Czekaj na klienta lub sygnał stop
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ConnectNamedPipe(pipe, &ov);

        HANDLE handles[2] = { ov.hEvent, m_stopEvent };
        DWORD  wr = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        CloseHandle(ov.hEvent);

        if (wr != WAIT_OBJECT_0) {          // stop lub błąd
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            break;
        }

        // Odczyt jednej linii (do '\n')
        std::string buf;
        char ch; DWORD read;
        while (ReadFile(pipe, &ch, 1, &read, nullptr) && read == 1) {
            if (ch == '\n') break;
            if (ch != '\r') buf += ch;
        }

        // UTF-8 → wide
        int wlen = MultiByteToWideChar(CP_UTF8, 0, buf.c_str(), -1, nullptr, 0);
        std::wstring req(wlen > 0 ? wlen - 1 : 0, L'\0');
        if (wlen > 1) MultiByteToWideChar(CP_UTF8, 0, buf.c_str(), -1, req.data(), wlen);

        // Wywołaj handler
        std::wstring resp;
        bool continueRunning = m_handler(req, resp);

        // Wide → UTF-8 + '\n'
        int ulen = WideCharToMultiByte(CP_UTF8, 0, resp.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string respU8(ulen > 0 ? ulen - 1 : 0, '\0');
        if (ulen > 1) WideCharToMultiByte(CP_UTF8, 0, resp.c_str(), -1, respU8.data(), ulen, nullptr, nullptr);
        respU8 += '\n';

        DWORD written;
        WriteFile(pipe, respU8.c_str(), static_cast<DWORD>(respU8.size()), &written, nullptr);
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);

        if (!continueRunning) break;
    }
}

} // namespace TotalControl
