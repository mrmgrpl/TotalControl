#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

// ─── Helpers: encoding ────────────────────────────────────────────────────────

static std::string WtoU8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// ─── Pipe ────────────────────────────────────────────────────────────────────

static HANDLE TryOpenPipe() {
    const wchar_t* name = L"\\\\.\\pipe\\TotalControl";
    HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) return h;
    if (GetLastError() == ERROR_PIPE_BUSY) {
        WaitNamedPipeW(name, 2000);
        h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0,
                        nullptr, OPEN_EXISTING, 0, nullptr);
    }
    return h;
}

static bool Talk(HANDLE pipe, const std::string& req, std::string& resp) {
    std::string line = req + "\n";
    DWORD wr;
    if (!WriteFile(pipe, line.c_str(), static_cast<DWORD>(line.size()), &wr, nullptr))
        return false;
    resp.clear();
    char ch; DWORD rd;
    while (ReadFile(pipe, &ch, 1, &rd, nullptr) && rd == 1) {
        if (ch == '\n') break;
        if (ch != '\r') resp += ch;
    }
    return true;
}

// ─── Minimal JSON readers ─────────────────────────────────────────────────────

static std::string JStr(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\":\"";
    auto pos = j.find(k);
    if (pos == std::string::npos) return "";
    pos += k.size();
    auto end = j.find('"', pos);
    return (end != std::string::npos) ? j.substr(pos, end - pos) : "";
}

static std::string JNum(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\":";
    auto pos = j.find(k);
    if (pos == std::string::npos) return "";
    pos += k.size();
    auto end = j.find_first_of(",}", pos);
    return (end != std::string::npos) ? j.substr(pos, end - pos) : j.substr(pos);
}

static bool JBool(const std::string& j, const char* key, bool def = false) {
    std::string v = JNum(j, key);
    if (v == "true")  return true;
    if (v == "false") return false;
    return def;
}

// ─── JSON builders ────────────────────────────────────────────────────────────

static std::string Q(const std::string& s) { return "\"" + s + "\""; }

// ─── argv helpers ─────────────────────────────────────────────────────────────

static std::string Arg(const std::vector<std::string>& args, const std::string& flag) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == flag) return args[i + 1];
    return "";
}

// ─── Daemon auto-start ────────────────────────────────────────────────────────

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (auto* s = wcsrchr(buf, L'\\')) *(s + 1) = L'\0';
    return buf;
}

static bool LaunchDaemon() {
    std::wstring dir    = ExeDir();
    std::wstring daemon = dir + L"TotalControl.exe";
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(daemon.c_str(), nullptr, nullptr, nullptr,
                        FALSE, CREATE_NEW_CONSOLE, nullptr, dir.c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// ─── Usage ────────────────────────────────────────────────────────────────────

static void Usage() {
    fputs(
        "Usage:\n"
        "  tc status\n"
        "  tc shoot [--ss 1/100] [--iso 100] [--f 2.8] [--mode M|A|S|P] [--focus MF|AF-S|AF-C]\n"
        "  tc get <prop>      shutter-speed | iso | f-number | exposure-mode | focus-mode | battery | remaining\n"
        "  tc set <prop> <val>\n"
        "  tc quit\n"
        "  tc --trigger-capture                   (gPhoto2 alias for shoot)\n"
        "  tc --get-config <prop>\n"
        "  tc --set-config <prop>=<val>\n",
        stderr
    );
}

// ─── wmain ────────────────────────────────────────────────────────────────────

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 2) { Usage(); return 1; }

    std::vector<std::string> args;
    args.reserve(argc - 1);
    for (int i = 1; i < argc; ++i)
        args.push_back(WtoU8(argv[i]));

    // ── Build JSON request ────────────────────────────────────────────────────
    std::string req;
    const std::string& cmd = args[0];

    if (cmd == "status") {
        req = R"({"cmd":"status"})";
    }
    else if (cmd == "quit") {
        req = R"({"cmd":"quit"})";
    }
    else if (cmd == "shoot" || cmd == "--trigger-capture") {
        req = "{\"cmd\":\"shoot\"";
        auto ss  = Arg(args, "--ss");    if (!ss.empty())   req += ",\"ss\":"    + Q(ss);
        auto iso = Arg(args, "--iso");   if (!iso.empty())  req += ",\"iso\":"   + iso;
        auto f   = Arg(args, "--f");     if (!f.empty())    req += ",\"f\":"     + f;
        auto mode= Arg(args, "--mode");  if (!mode.empty()) req += ",\"mode\":"  + Q(mode);
        auto foc = Arg(args, "--focus"); if (!foc.empty())  req += ",\"focus\":" + Q(foc);
        req += "}";
    }
    else if ((cmd == "get" || cmd == "--get-config") && args.size() >= 2) {
        req = "{\"cmd\":\"get\",\"prop\":" + Q(args[1]) + "}";
    }
    else if (cmd == "set" && args.size() >= 3) {
        req = "{\"cmd\":\"set\",\"prop\":" + Q(args[1]) + ",\"val\":" + Q(args[2]) + "}";
    }
    else if (cmd == "--set-config" && args.size() >= 2) {
        auto& kv = args[1];
        auto eq = kv.find('=');
        if (eq == std::string::npos) { Usage(); return 1; }
        req = "{\"cmd\":\"set\",\"prop\":" + Q(kv.substr(0, eq)) +
              ",\"val\":"                  + Q(kv.substr(eq + 1)) + "}";
    }
    else {
        Usage(); return 1;
    }

    // ── Connect (auto-start daemon if needed) ─────────────────────────────────
    HANDLE pipe = TryOpenPipe();
    if (pipe == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND) {
            fprintf(stderr, "tc: pipe error %lu\n", err);
            return 1;
        }
        // Daemon not running — launch it
        if (!LaunchDaemon()) {
            fprintf(stderr, "tc: cannot start TotalControl.exe (dir: %s)\n",
                    WtoU8(ExeDir()).c_str());
            return 1;
        }
        fprintf(stderr, "tc: starting daemon");
        for (int i = 0; i < 20 && pipe == INVALID_HANDLE_VALUE; ++i) {
            Sleep(500);
            fputc('.', stderr);
            pipe = TryOpenPipe();
        }
        fputc('\n', stderr);
        if (pipe == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "tc: daemon did not start in time\n");
            return 1;
        }
    }

    // ── Send / receive ────────────────────────────────────────────────────────
    std::string resp;
    bool ok = Talk(pipe, req, resp);
    CloseHandle(pipe);

    if (!ok) {
        fprintf(stderr, "tc: pipe I/O error\n");
        return 1;
    }

    // ── Interpret response ────────────────────────────────────────────────────
    if (!JBool(resp, "ok")) {
        std::string e = JStr(resp, "err");
        std::string m = JStr(resp, "msg");
        if (m.empty()) fprintf(stderr, "tc: error: %s\n", e.c_str());
        else           fprintf(stderr, "tc: error: %s - %s\n", e.c_str(), m.c_str());
        return 1;
    }

    if (cmd == "get" || cmd == "--get-config") {
        printf("%s\n", JStr(resp, "val").c_str());
    }
    else if (cmd == "shoot" || cmd == "--trigger-capture") {
        std::string lat = JNum(resp, "latency_ms");
        if (!lat.empty()) printf("latency_ms=%s\n", lat.c_str());
    }
    else if (cmd == "status") {
        auto ps = [&](const char* key)  { auto v = JStr(resp, key); if (!v.empty()) printf("%s=%s\n", key, v.c_str()); };
        auto pn = [&](const char* key)  { auto v = JNum(resp, key); if (!v.empty()) printf("%s=%s\n", key, v.c_str()); };
        pn("connected");
        ps("model");
        pn("battery");
        pn("remaining");
        ps("ss");
        pn("iso");
        pn("f");
        ps("mode");
        ps("focus");
        ps("store");
    }

    return 0;
}
