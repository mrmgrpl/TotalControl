#include "PipeServer.h"
#include <cassert>
#include <string>
#include <thread>

namespace TotalControl {

// Maximum length of a single JSON line in bytes (UTF-8).
// Clients sending a longer line get a truncated request — intentional safety cap.
static constexpr size_t kMaxLineBytes = 65536U;

PipeServer::PipeServer(const wchar_t* pipeName, PipeHandlerFn handler)
    : m_pipeName(pipeName), m_handler(std::move(handler))
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
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

// ── Accept loop ───────────────────────────────────────────────────────────────
// Creates a new pipe instance, waits for a client, then spawns ServeClient in a
// detached thread and immediately loops back to accept the next client.
void PipeServer::Run() {
    assert(!m_pipeName.empty());
    assert(m_stopEvent != INVALID_HANDLE_VALUE && m_stopEvent != nullptr);

    if (m_pipeName.empty())                    return;
    if (m_stopEvent == INVALID_HANDLE_VALUE)   return;
    if (m_stopEvent == nullptr)                return;

    while (m_running) {
        HANDLE pipe = CreateNamedPipeW(
            m_pipeName.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);

        if (pipe == INVALID_HANDLE_VALUE) break;

        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ov.hEvent == nullptr) { CloseHandle(pipe); break; }

        ConnectNamedPipe(pipe, &ov);

        HANDLE handles[2] = { ov.hEvent, m_stopEvent };
        const DWORD wr = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        CloseHandle(ov.hEvent);

        if (wr != WAIT_OBJECT_0) {
            // Stop signal or error — discard pipe and exit accept loop
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            break;
        }

        // Client connected — hand off to a worker thread and loop back immediately
        std::thread([this, pipe]() { ServeClient(pipe); }).detach();
    }
}

// ── Client handler (runs in detached worker thread) ───────────────────────────
// Reads JSON Lines from pipe, invokes handler under m_handlerMtx (serialised),
// writes JSON response. Exits when client disconnects; a handler returning
// false (e.g. "quit") also stops the whole server, not just this client —
// otherwise the accept loop in Run() keeps the process alive as long as any
// other client (e.g. the GUI's persistent connection) stays connected.
void PipeServer::ServeClient(HANDLE pipe) {
    assert(pipe != INVALID_HANDLE_VALUE);
    assert(pipe != nullptr);

    while (m_running) {
        // Read one JSON line (up to '\n') capped at kMaxLineBytes
        std::string buf;
        buf.reserve(256);
        char   ch       = '\0';
        DWORD  nRead    = 0;
        bool   gotData  = false;

        // Bounded by kMaxLineBytes — NASA rule 2
        while (buf.size() < kMaxLineBytes &&
               ReadFile(pipe, &ch, 1, &nRead, nullptr) && nRead == 1) {
            gotData = true;
            if (ch == '\n') break;
            if (ch != '\r') buf += ch;
        }
        if (!gotData || buf.empty()) break; // client disconnected

        // UTF-8 → wide
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, buf.c_str(), -1, nullptr, 0);
        std::wstring req(wlen > 0 ? static_cast<size_t>(wlen - 1) : 0U, L'\0');
        if (wlen > 1)
            MultiByteToWideChar(CP_UTF8, 0, buf.c_str(), -1, req.data(), wlen);

        // Invoke handler — serialised across all client threads
        std::wstring resp;
        bool continueRunning = true;
        {
            std::lock_guard lk(m_handlerMtx);
            continueRunning = m_handler(req, resp);
        }

        // Wide → UTF-8 + '\n'
        const int ulen = WideCharToMultiByte(
            CP_UTF8, 0, resp.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string respU8(ulen > 0 ? static_cast<size_t>(ulen - 1) : 0U, '\0');
        if (ulen > 1)
            WideCharToMultiByte(CP_UTF8, 0, resp.c_str(), -1,
                                respU8.data(), ulen, nullptr, nullptr);
        respU8 += '\n';

        DWORD written = 0;
        if (!WriteFile(pipe, respU8.c_str(),
                       static_cast<DWORD>(respU8.size()), &written, nullptr))
            break; // client disconnected during write

        if (!continueRunning) {
            Stop(); // full shutdown requested — unblocks Run()'s accept loop too
            break;
        }
    }

    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

} // namespace TotalControl
