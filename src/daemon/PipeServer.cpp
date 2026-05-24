#include "PipeServer.h"
#include <string>

namespace TotalControl {

// Maksymalna długość jednej linii JSON w bajtach (UTF-8).
// Klient wysyłający dłuższą linię otrzyma obcięte żądanie — celowe zabezpieczenie.
static constexpr size_t kMaxLineBytes = 65536U;

PipeServer::PipeServer(const wchar_t* pipeName, PipeHandlerFn handler)
    : m_pipeName(pipeName), m_handler(std::move(handler))
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // m_stopEvent == nullptr oznacza błąd — Stop() i Run() sprawdzają INVALID_HANDLE_VALUE
}

PipeServer::~PipeServer() {
    Stop();
    if (m_stopEvent != INVALID_HANDLE_VALUE && m_stopEvent != nullptr)
        CloseHandle(m_stopEvent);
}

void PipeServer::Stop() {
    m_running = false;
    if (m_stopEvent != INVALID_HANDLE_VALUE && m_stopEvent != nullptr)
        SetEvent(m_stopEvent);
}

void PipeServer::Run() {
    if (m_pipeName.empty())                      return;
    if (m_stopEvent == INVALID_HANDLE_VALUE)     return;
    if (m_stopEvent == nullptr)                  return;

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
        if (ov.hEvent == nullptr) {
            CloseHandle(pipe);
            break;
        }
        ConnectNamedPipe(pipe, &ov);

        HANDLE handles[2] = { ov.hEvent, m_stopEvent };
        const DWORD wr = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        CloseHandle(ov.hEvent);

        if (wr != WAIT_OBJECT_0) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            break;
        }

        // Odczyt jednej linii JSON (do '\n') z górnym limitem kMaxLineBytes
        std::string buf;
        buf.reserve(256);
        char ch = '\0';
        DWORD nRead = 0;
        while (buf.size() < kMaxLineBytes &&
               ReadFile(pipe, &ch, 1, &nRead, nullptr) && nRead == 1) {
            if (ch == '\n') break;
            if (ch != '\r') buf += ch;
        }

        // UTF-8 → wide
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, buf.c_str(), -1, nullptr, 0);
        std::wstring req(wlen > 0 ? static_cast<size_t>(wlen - 1) : 0U, L'\0');
        if (wlen > 1)
            MultiByteToWideChar(CP_UTF8, 0, buf.c_str(), -1, req.data(), wlen);

        // Wywołaj handler
        std::wstring resp;
        const bool continueRunning = m_handler(req, resp);

        // Wide → UTF-8 + '\n'
        const int ulen = WideCharToMultiByte(
            CP_UTF8, 0, resp.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string respU8(ulen > 0 ? static_cast<size_t>(ulen - 1) : 0U, '\0');
        if (ulen > 1)
            WideCharToMultiByte(CP_UTF8, 0, resp.c_str(), -1, respU8.data(), ulen,
                                nullptr, nullptr);
        respU8 += '\n';

        DWORD written = 0;
        // Ignorujemy wynik WriteFile: błąd = klient rozłączył się; pipe i tak zamykamy poniżej
        if (!WriteFile(pipe, respU8.c_str(), static_cast<DWORD>(respU8.size()),
                       &written, nullptr)) {
            // klient rozłączony podczas zapisu — kontynuujemy do CloseHandle
        }
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);

        if (!continueRunning) break;
    }
}

} // namespace TotalControl
