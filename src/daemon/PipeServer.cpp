#include "PipeServer.h"
#include <cassert>
#include <string>
#include <thread>

namespace TotalControl {

// Maximum length of a single JSON line in bytes (UTF-8).
// Clients sending a longer line get a truncated request — intentional safety cap.
static constexpr size_t kMaxLineBytes = 65536U;

// Pipe handles here are always created with FILE_FLAG_OVERLAPPED (required so
// Stop() can cancel a pending ConnectNamedPipe in the accept loop via
// WaitForMultipleObjects). Per Win32 docs, EVERY ReadFile/WriteFile on such a
// handle must supply a real OVERLAPPED structure — calling them with
// lpOverlapped=nullptr (the previous code) is undefined behavior. It usually
// "worked" with only one or two concurrent clients, but under real
// concurrent multi-client load (see Change log: 4-camera test) it caused a
// client's request to be silently swallowed — GUI's SendRequest blocked
// forever and SRV never logged having received it. These helpers block
// synchronously (GetOverlappedResult(..., bWait=TRUE)) so ServeClient's
// logic below is otherwise unchanged.
static bool OverlappedReadByte(HANDLE pipe, char& outCh, DWORD& outRead) {
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) return false;
    BOOL ok = ReadFile(pipe, &outCh, 1, nullptr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) { CloseHandle(ov.hEvent); return false; }
    ok = GetOverlappedResult(pipe, &ov, &outRead, TRUE);
    CloseHandle(ov.hEvent);
    return ok != 0;
}

static bool OverlappedWriteAll(HANDLE pipe, const char* data, DWORD size) {
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) return false;
    BOOL ok = WriteFile(pipe, data, size, nullptr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) { CloseHandle(ov.hEvent); return false; }
    DWORD written = 0;
    ok = GetOverlappedResult(pipe, &ov, &written, TRUE);
    CloseHandle(ov.hEvent);
    return ok != 0 && written == size;
}

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
// Run() spawns kAcceptorThreads copies of AcceptorLoop() and blocks until
// Stop() is signalled. Each running AcceptorLoop() independently cycles
// create-instance -> wait-for-client -> hand off -> repeat, so up to
// kAcceptorThreads pipe instances are simultaneously listening at all times.
void PipeServer::Run() {
    assert(!m_pipeName.empty());
    assert(m_stopEvent != INVALID_HANDLE_VALUE && m_stopEvent != nullptr);

    if (m_pipeName.empty())                    return;
    if (m_stopEvent == INVALID_HANDLE_VALUE)   return;
    if (m_stopEvent == nullptr)                return;

    for (int i = 0; i < kAcceptorThreads; ++i)
        m_acceptors[i] = std::thread([this] { AcceptorLoop(); });

    WaitForSingleObject(m_stopEvent, INFINITE);

    for (auto& t : m_acceptors)
        if (t.joinable()) t.join();
}

// One acceptor pool slot: creates a pipe instance, waits (cancelably, via
// m_stopEvent) for a client, hands the connected client off to a detached
// worker thread, then loops back to listen again. Never blocks on a
// specific client past the handoff — only ConnectNamedPipe is awaited here,
// which m_stopEvent reliably interrupts — so Run()'s join() above always
// completes promptly regardless of how long individual clients stay
// connected (previously a single such loop meant a burst of near-
// simultaneous client connects — e.g. the GUI opening one pipe per camera
// track at sequencer start — serialised through one listening instance at a
// time; later connects lost the race and failed or connected much later
// than expected; see Change log).
void PipeServer::AcceptorLoop() {
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
            // Stop signal or error — discard pipe and exit this acceptor
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            break;
        }

        // Client connected — hand off to a worker thread and loop back immediately
        std::thread([this, pipe]() { ServeClient(pipe); }).detach();
    }
}

// ── Client handler (runs in detached worker thread) ───────────────────────────
// Reads JSON Lines from pipe, invokes handler (concurrently with other
// clients' threads — see class comment in PipeServer.h), writes JSON
// response. Exits when client disconnects; a handler returning
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
               OverlappedReadByte(pipe, ch, nRead) && nRead == 1) {
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

        // Invoke handler — runs concurrently with other clients' handler
        // calls; CommandHandler::Handle() does its own per-camera locking.
        std::wstring resp;
        bool continueRunning = m_handler(req, resp);

        // Wide → UTF-8 + '\n'
        const int ulen = WideCharToMultiByte(
            CP_UTF8, 0, resp.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string respU8(ulen > 0 ? static_cast<size_t>(ulen - 1) : 0U, '\0');
        if (ulen > 1)
            WideCharToMultiByte(CP_UTF8, 0, resp.c_str(), -1,
                                respU8.data(), ulen, nullptr, nullptr);
        respU8 += '\n';

        if (!OverlappedWriteAll(pipe, respU8.c_str(), static_cast<DWORD>(respU8.size())))
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
